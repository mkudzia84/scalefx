/*
 * HubFX ESP32-S3 — incremental bring-up sketch.
 *
 *   Started as a NO-OP scaffold (BoardServer<> alone) to validate the
 *   wire surface boots clean.  Policies are now being added back one
 *   at a time, so this sketch can be flashed at any commit and the
 *   user can confirm boot + IDENTIFY before another layer goes in.
 *
 *   Stack so far:
 *     - BoardServicePolicy        (auto, lifecycle + INIT_READY + STATUS)
 *     - IndicatorServicePolicy    (auto, status LEDs)
 *     - PortServicePolicy         (BoardOf<>, hub-local port enumeration)
 *     - RoleServicePolicy         (BoardOf<>, hub-local role dispatch)
 *     - StorageService            (LittleFS + SD_MMC 4-bit SDIO)
 *     - AudioService              (8-channel mixer + TAS5825P, dual-core)
 *     - AlertService              (system-alert chimes on channel 0)
 *     - ExpanderService           (USB host + IDENTIFY of remote boards)
 *     - TopologyService           (GUID-addressed port/role surface)
 *     - InputDispatcherService    (RC/SBUS/Jeti channel-event fanout)
 *     - LandingLightService       (servo+LED-group landing lights, empty defs)
 *     - LightFxEffectService      (LED program runtime, one default "AllOn" program)
 *
 *   Ports declared on the HubFX PCB:
 *     - 8 × PWM    (PCA9685 channels with per-rail INA226 V/I sense)
 *     - 1 × Input  (IN_1, multi-modal PULSE / SBUS / JETI_EX)
 *     - 11 × Servo (IN_2..IN_12, output actuator headers)
 *
 *   The full sketch (every effect on top of this base) is parked next
 *   to this file as `hubfx_esp32s3.ino.full`.
 */

#define FIRMWARE_VERSION "2.3.0-noop"
#define BUILD_NUMBER     100

#include <Arduino.h>
#include <Wire.h>

#include <platform/sfx_platform.h>
#include <serial/diag_log.h>
#include <server/board_of.h>

// ── Port hardware drivers ────────────────────────────────────────────
#include <ports/pwm_port.h>
#include <ports/servo_port.h>
#include <ports/esp_input_port.h>
#include <pwm/pca9685.h>
#include <power/ina226.h>
#include <power/ina226_sensor.h>

// Storage (LittleFS flash + SD_MMC 4-bit SDIO) — compiled in via
// SFX_HAS_STORAGE + SFX_HAS_STORAGE_SERVER (platformio.ini build_flags).
#include <storage/bring_up.h>
#include <server/storage_service.h>

// Audio (8-channel WAV mixer + I²S out + TAS5825P codec) — compiled
// in via SFX_HAS_AUDIO.  `EspDualCoreAudio<Mixer>` owns the two-phase
// boot dance (Core 0 codec probe + mixer alloc → Core 1 I²S task →
// Core 0 codec activate to PLAY).
#include <audio/audio_mixer.h>
#include <audio/esp_i2s_output.h>
#include <audio/esp_dual_core_audio.h>
#include <audio/upload_exclusivity.h>
#include <codec/tas5825_p_codec.h>
#include <server/audio_service.h>

#include "effects/alerts/alert_service.h"
#include "effects/input/input_dispatcher.h"
#include "effects/landing_lights/landing_light_service.h"
#include "effects/lightfx/lightfx_service.h"
#include "expanders/expander_service.h"
#include "topology/topology_service.h"

// ════════════════════════════════════════════════════════════════════════
//  Board pin / address map (HubFX 8-channel rev — see PINOUT.md)
// ════════════════════════════════════════════════════════════════════════

namespace Gpio {
    // I²C bus — shared with TAS5825P codec, PCA9685, INA226 monitors.
    constexpr int I2C_SDA        =  8;
    constexpr int I2C_SCL        =  9;

    // On-board status LED.
    constexpr int LED_CONNECTION = 48;
    constexpr int LED_ERROR      = -1;

    // RC header pins — IN_1 is the only input-capable net (Rule 31);
    // IN_2..IN_12 are output-only servo headers.
    constexpr int IN_1           =  5;
    constexpr int IN_2           =  6;
    constexpr int IN_3           =  7;
    constexpr int IN_4           = 10;
    constexpr int IN_5           = 11;
    constexpr int IN_6           = 12;
    constexpr int IN_7           = 13;
    constexpr int IN_8           = 14;
    constexpr int IN_9           = 15;
    constexpr int IN_10          =  4;
    constexpr int IN_11          =  3;
    constexpr int IN_12          =  2;

    // I²S to TAS5825P codec (Philips standard mode, 48 kHz / 16-bit).
    constexpr int I2S_DOUT       = 16;   // TAS_DI  → codec DIN
    constexpr int I2S_BCLK       = 17;   // TAS_BCK → codec BCK
    constexpr int I2S_LRCLK      = 18;   // TAS_FS  → codec FS / LRCLK

    // SD card — 4-bit SDIO via SD_MMC slot 1.  GPIO matrix routed; the
    // PCB designer's authoritative map skips GPIO 43/44 because UART0
    // (CH343 console bridge) is hardwired to those pins.
    constexpr int SD_CMD         = 38;
    constexpr int SD_CLK         = 39;
    constexpr int SD_D0          = 40;
    constexpr int SD_D1          = 41;
    constexpr int SD_D2          = 42;
    constexpr int SD_D3          = 45;
}

namespace I2cAddr {
    constexpr uint8_t PCA9685    = 0x70;   // 8-channel PWM driver
    constexpr uint8_t TAS5825P   = 0x4C;   // Class-H audio codec (ADDR → GND)

    // Per-channel current/voltage monitors — channel index maps to PWM
    // port index (CH1 = port 0, CH8 = port 7).
    constexpr uint8_t INA226_CH1 = 0x40;
    constexpr uint8_t INA226_CH2 = 0x41;
    constexpr uint8_t INA226_CH3 = 0x42;
    constexpr uint8_t INA226_CH4 = 0x43;
    constexpr uint8_t INA226_CH5 = 0x44;
    constexpr uint8_t INA226_CH6 = 0x45;
    constexpr uint8_t INA226_CH7 = 0x4A;
    constexpr uint8_t INA226_CH8 = 0x4F;
}

namespace Uart {
    constexpr uint8_t IN_1 = 1;            // UART1 claimed by InputPort on IN_1
}

namespace Sense {
    constexpr float    INA226_SHUNT_OHMS = 0.1f;    // 0.1 Ω shunt
    constexpr float    INA226_MAX_AMPS   = 3.2f;    // ±3.2 A range
}

namespace Pwm {
    /// 1526 Hz — the PCA9685 POR ceiling and well above the LED
    /// flicker-fusion threshold.
    constexpr uint16_t FREQ_HZ = 1526;
}

namespace Codec {
    /// 3S LiPo (~12 V nominal) — selects the AGAIN table at activate.
    constexpr auto SUPPLY_VOLTAGE = sfx_audio::tas5825::Supply::V12;
}

// CodecAdapter specialization for HubFX's TAS5825P.  MUST come after
// the `Gpio::` / `Codec::` namespaces — the header references them.
#include "effects/audio_codec.h"

// ── Policy aliases ───────────────────────────────────────────────────

using Mixer        = AudioMixer<EspI2SOutput, TAS5825PCodec>;
using AudioService = AudioServicePolicy<Mixer>;
using AlertService = hubfx::effects::alerts::AlertServicePolicyT<Mixer>;

// HubFX-flavoured expander service: two USB-OTG ports, four-entry GUID
// history cache (enough for hot-swap of one full generation).
using HubFxExpanderService = hubfx::expanders::ExpanderServicePolicyT<2, 4>;
using HubFxTopologyService =
    hubfx::topology::TopologyServicePolicyT<HubFxExpanderService>;

// Input dispatcher — bound to the concrete topology type so it can
// subscribe to RCIN / SBUS / JETIEX role events from any board (local
// or remote).  Pure plumbing: no capability bit, no wire surface, no
// hardware claim.  Effects (Engine/Gun/LightProgram/…) register their
// `TriggerInput`s against this once they come online.
using InputDispatcherService =
    hubfx::effects::input::InputDispatcherServicePolicyT<HubFxTopologyService>;

// Landing-light service — manages (servo, LED-group) port claims +
// per-light state machines.  LightFx + GearControl forward state
// changes through its `setState()` API rather than touching the
// underlying ports directly.  Empty `_defs` table at first; `configure()`
// gets called before `board.begin()` once we have a default landing-
// light topology to ship.
using LandingLightService =
    hubfx::effects::landing::LandingLightServicePolicyT<HubFxTopologyService>;

// LightFX effect service — owns the program table + `LightController`
// and dispatches the LIGHTFX_* wire surface.  Programs activate by
// pushing LED queues through the topology to (potentially remote) PWM
// ports.
using LightFxEffectService =
    hubfx::effects::lightfx::LightFxEffectServicePolicyT<HubFxTopologyService,
                                                          LandingLightService>;

// ── Board class ──────────────────────────────────────────────────────
//
// BoardOf<> auto-prepends BoardServicePolicy + IndicatorServicePolicy +
// PortServicePolicy + RoleServicePolicy.  User policies follow.

class HubFxBoard : public sfx_core::BoardOf<HubFxBoard,
                                             HubFxExpanderService,
                                             HubFxTopologyService,
                                             InputDispatcherService,
                                             LandingLightService,
                                             LightFxEffectService,
                                             StorageService,
                                             AudioService,
                                             AlertService> {
public:
    // ── Hardware drivers — declaration order matters (init order) ────
    PCA9685 pca;
    INA226  ina[8];

    sfx_peripherals::Pca9685PwmPort pwm[8] = {
        {pca, 0}, {pca, 1}, {pca, 2}, {pca, 3},
        {pca, 4}, {pca, 5}, {pca, 6}, {pca, 7},
    };
    sfx_peripherals::Ina226VoltageSensor vSense[8] = {
        {ina[0]}, {ina[1]}, {ina[2]}, {ina[3]},
        {ina[4]}, {ina[5]}, {ina[6]}, {ina[7]},
    };
    sfx_peripherals::Ina226CurrentSensor iSense[8] = {
        {ina[0]}, {ina[1]}, {ina[2]}, {ina[3]},
        {ina[4]}, {ina[5]}, {ina[6]}, {ina[7]},
    };

    // ── Input ports — IN_1 (multi-modal: PULSE / SBUS / JETI_EX) ─────
    sfx_peripherals::EspInputPort in[1] = {
        {Gpio::IN_1, Uart::IN_1},
    };

    // ── Servo OUTPUT ports — IN_2..IN_12 as actuator headers ─────────
    sfx_peripherals::MicroservoPort servoOut[11] = {
        {Gpio::IN_2},  {Gpio::IN_3},  {Gpio::IN_4},
        {Gpio::IN_5},  {Gpio::IN_6},  {Gpio::IN_7},
        {Gpio::IN_8},  {Gpio::IN_9},  {Gpio::IN_10},
        {Gpio::IN_11}, {Gpio::IN_12},
    };

    // ── Hardware-init result snapshot ────────────────────────────────
    //
    // Captured during `initHardware()` (which runs BEFORE Serial / DiagLog
    // are up) so we can replay the readings to the diag stream from
    // `logHardwareStatus()` after `board.begin()` lights everything up.
    struct InaProbe {
        uint8_t  addr        = 0;
        uint8_t  wireAck     = 0xFF;   // Wire.endTransmission() status, 0 = ACK
        bool     begun       = false; // driver bring-up succeeded (chip reachable + configured)
        bool     idMatches   = false; // mfg/die match canonical TI INA226 IDs
        uint16_t mfgId       = 0;
        uint16_t dieId       = 0;
    };
    struct HwInitState {
        bool     pcaPreAck         = false;
        bool     pcaBegun          = false;
        uint8_t  pcaMode1          = 0xFF;
        uint8_t  pcaMode2          = 0xFF;
        uint8_t  pcaPrescale       = 0xFF;
        // Per-channel ACK after each pre-fire setChannel(k, 0) in
        // initHardware() — runs the exact write Pca9685PwmPort::begin()
        // does, with a bus probe between each, so a future regression
        // can pinpoint the channel/write that wedges the chip.
        bool     pcaAfterCh[8]     = {false};
        // Post-board.begin() probe.  `pcaPostInitAck` is rewritten by
        // the recovery path when it fires; `pcaPostInitAckRaw` is the
        // single original probe value (never overwritten), so logging
        // can show whether the recovery actually had to do anything.
        bool     pcaPostInitAckRaw = false;
        bool     pcaPostInitAck    = false;
        uint8_t  pcaPostInitMode1  = 0xFF;
        InaProbe ina[8];
    };
    HwInitState hwInit{};

    /// Silent hardware probe — runs BEFORE `board.begin()` so the
    /// Pca9685PwmPort instances can `setDuty(0)` against a live chip.
    /// All visible output is deferred to `logHardwareStatus()` so the
    /// readings can land in the DiagLog ring instead of being dropped
    /// pre-Serial.
    ///
    /// Wire clock 400 kHz — the earlier "missing INA ch7/ch8" turned
    /// out to be an `MAX_EXPECTED_I2C` overflow (cap was 8, we register
    /// 10 chips), not a bus-speed problem.  Now that the cap is 32 and
    /// the buffer fix has landed, the bus runs reliably at 400 kHz on
    /// this PCB rev.  Drop to 100 kHz here if a future board variant
    /// adds new long traces and the longer-distance chips start NACK-ing.
    void initHardware() {
        Wire.begin(Gpio::I2C_SDA, Gpio::I2C_SCL);
        Wire.setClock(400000);

        // General-call SWRST before any per-chip probe — recovers a
        // PCA9685 whose address comparator latched out of normal state
        // (most commonly from a previous boot's debug code writing
        // 0xFF as a register pointer, which is undefined on the PCA).
        // The PCA honours gen-call by default; INA226s and the codec
        // ignore the broadcast.  See `PCA9685::broadcastReset()`.
        PCA9685::broadcastReset(Wire);
        delay(2);

        // PCA9685 pre-ACK probe + driver begin + post-init register snapshot.
        hwInit.pcaPreAck = pcaBusProbe(I2cAddr::PCA9685);
        hwInit.pcaBegun  = pca.begin(Wire, I2cAddr::PCA9685, Pwm::FREQ_HZ);
        if (hwInit.pcaBegun) {
            hwInit.pcaMode1    = pcaRead(I2cAddr::PCA9685, 0x00);
            hwInit.pcaMode2    = pcaRead(I2cAddr::PCA9685, 0x01);
            hwInit.pcaPrescale = pcaRead(I2cAddr::PCA9685, 0xFE);

            // Pre-fire the same writes BoardOf::begin() will later
            // issue per PWM port (one setChannel(ch, 0) per channel),
            // probing the bus between each.  If the address-comparator
            // wedge regresses we want a per-channel breadcrumb showing
            // exactly when it happened.  Pca9685PwmPort::begin() is
            // idempotent against this so re-firing in board.begin()
            // is harmless.
            for (uint8_t k = 0; k < 8; k++) {
                pca.setChannel(k, 0);
                hwInit.pcaAfterCh[k] = pcaBusProbe(I2cAddr::PCA9685);
            }
        }

        // Per-INA probe + driver begin + mfg/die ID read.  The driver
        // now bails on non-canonical IDs (returns false, _available
        // stays false) so calling `bootMfgId()`/`bootDieId()` is safe
        // either way — they reflect the readback done before the
        // canonical-ID gate.  See ina226.cpp for the rationale; the
        // gate exists because clones at INA addresses have been seen
        // to wedge OTHER chips on the bus when written to (HubFX rev
        // hit this with a clone @ 0x40 corrupting the PCA9685 @ 0x70).
        constexpr uint8_t kInaAddrs[8] = {
            I2cAddr::INA226_CH1, I2cAddr::INA226_CH2,
            I2cAddr::INA226_CH3, I2cAddr::INA226_CH4,
            I2cAddr::INA226_CH5, I2cAddr::INA226_CH6,
            I2cAddr::INA226_CH7, I2cAddr::INA226_CH8,
        };
        for (uint8_t k = 0; k < 8; k++) {
            hwInit.ina[k].addr = kInaAddrs[k];
            Wire.beginTransmission(kInaAddrs[k]);
            hwInit.ina[k].wireAck = Wire.endTransmission();
            if (hwInit.ina[k].wireAck != 0) continue;

            hwInit.ina[k].begun =
                ina[k].begin(Wire, kInaAddrs[k],
                             Sense::INA226_SHUNT_OHMS, Sense::INA226_MAX_AMPS);
            // Even when begin() returns false (non-canonical chip,
            // not driven), the driver still captured the boot IDs
            // before bailing — surface them for the boot log.
            hwInit.ina[k].mfgId     = ina[k].bootMfgId();
            hwInit.ina[k].dieId     = ina[k].bootDieId();
            hwInit.ina[k].idMatches = ina[k].isCanonical();
        }
    }

    /// Replay the captured hardware-init snapshot to DiagLog.  Called
    /// from `setup()` right after `board.begin()` so the entries land
    /// in the ring (live-streamed too once `setWireMinLevel(INFO)` is
    /// applied — see setup()).  Re-probes the PCA9685 in-place because
    /// `board.begin()` walks every declared port's `port->begin()`
    /// which writes to the chip — useful for catching a regression
    /// where one of those writes wedges the address comparator.
    void logHardwareStatus() {
        SFX_LOG_INFO("[I2C] Wire up: SDA=GP%d SCL=GP%d @ 400 kHz",
                     Gpio::I2C_SDA, Gpio::I2C_SCL);

        // Re-probe the PCA — board.begin() has run by the time we get
        // here, so the port-policy port->begin() calls have all fired
        // against it.  If the chip went silent in the meantime (a
        // known ESP32 + PCA9685 quirk — see public references in
        // commit notes), reset it via gen-call SWRST and re-run
        // `pca.begin()` to restore frequency / AI / ALLCALL.  After
        // that, walk the PWM ports once more to push each channel's
        // last duty back to the chip.
        hwInit.pcaPostInitAck    = pcaBusProbe(I2cAddr::PCA9685);
        hwInit.pcaPostInitAckRaw = hwInit.pcaPostInitAck;   // freeze the pre-recovery state
        hwInit.pcaPostInitMode1  = pcaRead(I2cAddr::PCA9685, 0x00);
        if (!hwInit.pcaPostInitAck) {
            SFX_LOG_WARN("[PCA] post-init probe failed — chip wedged during "
                         "board.begin() port-init.  Running recovery: SWRST "
                         "→ re-init → re-push duties.");
            PCA9685::broadcastReset(Wire);
            delay(2);
            const bool reinitOk = pca.begin(Wire, I2cAddr::PCA9685, Pwm::FREQ_HZ);
            if (reinitOk) {
                for (uint8_t k = 0; k < 8; k++) {
                    pwm[k].setDuty(pwm[k].duty());   // restore last commanded
                }
            }
            hwInit.pcaPostInitAck   = pcaBusProbe(I2cAddr::PCA9685);
            hwInit.pcaPostInitMode1 = pcaRead(I2cAddr::PCA9685, 0x00);
            if (hwInit.pcaPostInitAck) {
                SFX_LOG_WARN("[PCA] recovery OK — chip back at 0x%02X "
                             "MODE1=0x%02X (reinit %s)",
                             I2cAddr::PCA9685, hwInit.pcaPostInitMode1,
                             reinitOk ? "succeeded" : "FAILED");
            } else {
                SFX_LOG_ERROR("[PCA] recovery FAILED — chip still silent "
                              "after SWRST + re-init.  PWM unavailable.");
            }
        }

        if (!hwInit.pcaPreAck) {
            SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: NO ACK on pre-begin probe",
                          I2cAddr::PCA9685);
        }
        if (!hwInit.pcaBegun) {
            SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: begin() failed", I2cAddr::PCA9685);
        } else {
            // Post-begin MODE1 should be 0x21 (AI|ALLCALL — RESTART
            // self-clears per §7.3.1.1).  Anything else means a driver
            // regression or the chip is wedged before this read.
            SFX_LOG_INFO("[PCA] PCA9685 @ 0x%02X: %u Hz  MODE1=0x%02X MODE2=0x%02X PRESCALE=0x%02X",
                         I2cAddr::PCA9685, Pwm::FREQ_HZ,
                         hwInit.pcaMode1, hwInit.pcaMode2, hwInit.pcaPrescale);

            // Per-channel pre-fire trail.  Collapse to a single line
            // listing any channels where the post-write probe failed.
            // If the line says "ch0..ch7 OK" the address comparator
            // survived all 8 writes — the wedge regression hasn't fired.
            uint8_t wedgedAt = 0xFF;
            for (uint8_t k = 0; k < 8; k++) {
                if (!hwInit.pcaAfterCh[k]) { wedgedAt = k; break; }
            }
            if (wedgedAt == 0xFF) {
                SFX_LOG_INFO("[PCA] PCA9685 @ 0x%02X: 8/8 per-channel writes ACKed "
                             "(no address-comparator wedge)", I2cAddr::PCA9685);
            } else {
                SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: address comparator WEDGED "
                              "after pre-fire setChannel(%u, 0) — earlier %u "
                              "channel writes ACKed.  Investigate driver MODE1 "
                              "/ ALLCALLADR state.",
                              I2cAddr::PCA9685, (unsigned)wedgedAt,
                              (unsigned)wedgedAt);
            }

        }
        if (hwInit.pcaPostInitAck) {
            SFX_LOG_INFO("[PCA] PCA9685 @ 0x%02X: post-board.begin() OK  MODE1=0x%02X",
                         I2cAddr::PCA9685, hwInit.pcaPostInitMode1);
        } else {
            SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: WENT SILENT after board.begin() — "
                          "a PortServicePolicy port->begin() write may have wedged the chip.",
                          I2cAddr::PCA9685);
        }

        uint8_t inaOk      = 0;
        uint8_t inaClones  = 0;
        for (uint8_t k = 0; k < 8; k++) {
            const auto& p = hwInit.ina[k];
            if (p.wireAck != 0) {
                SFX_LOG_WARN("[INA] ch%u @ 0x%02X: NO ACK (Wire status=%u)",
                             (unsigned)(k + 1), p.addr, (unsigned)p.wireAck);
                continue;
            }
            if (!p.begun) {
                // Driver bailed.  Almost always because the chip is a
                // non-canonical clone — surface its boot IDs so the
                // field engineer can match the chip and replace it.
                inaClones++;
                SFX_LOG_WARN("[INA] ch%u @ 0x%02X: NOT DRIVEN — non-canonical IDs "
                             "mfg=0x%04X die=0x%04X (expected 0x5449/0x2260, "
                             "TI INA226).  Clone detected — refusing to drive "
                             "to protect other chips on the shared I²C bus.",
                             (unsigned)(k + 1), p.addr, p.mfgId, p.dieId);
                continue;
            }
            // begun=true ⇒ canonical TI INA226 (the driver only
            // configures chips that pass the ID check; clones are
            // surfaced in the `!p.begun` branch above).
            SFX_LOG_INFO("[INA] ch%u @ 0x%02X: OK  mfg=0x%04X die=0x%04X (TI INA226)",
                         (unsigned)(k + 1), p.addr, p.mfgId, p.dieId);
            inaOk++;
        }
        if (inaClones > 0) {
            SFX_LOG_WARN("[INA] %u/8 monitors up (%u clone%s skipped — replace "
                         "to restore full V/I sense)",
                         inaOk, inaClones, inaClones == 1 ? "" : "s");
        } else {
            SFX_LOG_INFO("[INA] %u/8 monitors up (all genuine TI INA226)", inaOk);
        }
    }

    /// Low-level Wire helpers for diagnostic-only register reads.  Bare
    /// transactions so we don't go through the driver's class hierarchy
    /// when validating its post-init state.
    static bool pcaBusProbe(uint8_t addr) {
        Wire.beginTransmission(addr);
        return Wire.endTransmission() == 0;
    }
    static uint8_t pcaRead(uint8_t addr, uint8_t reg) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) return 0xFF;
        Wire.requestFrom((int)addr, 1);
        return Wire.available() ? (uint8_t)Wire.read() : 0xFF;
    }

    /// Per-loop maintenance — refresh INA226 cached readings.  The
    /// input port is driven by edge-IRQ (pulse mode) or the UART RX
    /// FIFO (SBUS / Jeti EX) — no tick needed here.
    void pollSense() {
        for (uint8_t k = 0; k < 8; k++) ina[k].update();
    }


    // ── Port declarations (compile-time, consumed by BoardOf<>) ──────

    static constexpr auto kPwmPorts = sfx_core::ports::list(
        sfx_core::ports::pwm_array<&HubFxBoard::pwm, 8>()
            .with_vSense_array<&HubFxBoard::vSense>()
            .with_iSense_array<&HubFxBoard::iSense>());

    static constexpr auto kInputPorts = sfx_core::ports::list(
        sfx_core::ports::input_array<&HubFxBoard::in, 1>());

    static constexpr auto kServoPorts = sfx_core::ports::list(
        sfx_core::ports::servo_array<&HubFxBoard::servoOut, 11>());

    static constexpr const char* kName = "HubFx";
};

HubFxBoard board;

// Dual-core audio bring-up helper.  Owns the atomic flags, the Core 1
// consumer task, the producer task spawn, and the two-phase orchestration.
// Codec type is inferred from `Mixer::Codec`.
static EspDualCoreAudio<Mixer> audio;

// Default LightFx program list installed before `board.begin()` so the
// LED runtime has something to dispatch.  One entry: "AllOn" drives
// every hub-local PWM channel (0..7) at full brightness, constant on.
// Switch to it from the CLI with `light:program-select 0` (the user-
// facing wire surface — LIGHTFX_PROGRAM_SELECT).
static hubfx::effects::lightfx::Program kLightFxPrograms[1] = {};

static void buildDefaultLightFxPrograms() {
    using namespace hubfx::effects::lightfx;
    using hubfx::effects::PortRef;
    Program& p = kLightFxPrograms[0];
    std::strncpy(p.name, "AllOn", sizeof(p.name) - 1);
    p.numChannels = 8;
    for (uint8_t ch = 0; ch < 8; ++ch) {
        LedChannelSpec& spec = p.channels[ch];
        spec.addr                    = PortRef::local(PortKind::Pwm, ch);
        spec.perChannelBrightnessPct = 100;
        spec.events[0]               = LightEvent::on(/*brightnessPct=*/100,
                                                      /*durationMs=*/0);  // 0 = terminal / hold
        spec.numEvents               = 1;
    }
    p.numLandings = 0;
}

void setup() {
    // I²C + PCA9685 + INA226 — must run before board.begin() so the
    // PWM port descriptors point at a live chip (begin() will call
    // setDuty(0) per port).
    board.initHardware();

    // Storage — LittleFS + SD_MMC probe.  Both backends self-recover
    // via SD_INIT / FLASH_STATUS_REQ from the host if anything's amiss.
    bringUpStorage({
        .clk = Gpio::SD_CLK, .cmd = Gpio::SD_CMD,
        .d0  = Gpio::SD_D0,  .d1  = Gpio::SD_D1,
        .d2  = Gpio::SD_D2,  .d3  = Gpio::SD_D3,
    });

    // LightFx programs — MUST be installed BEFORE `board.begin()` so
    // the LightController is configured before any LIGHTFX_* command
    // can land.
    buildDefaultLightFxPrograms();
    board.policy<LightFxEffectService>().configure(kLightFxPrograms, 1);

    // Policy pack lifecycle — Serial, DiagLog, indicator pins, port
    // registry binding, every policy's begin().  Master role, no
    // upstream watchdog.  BoardOf<>::begin() takes (version, build, ...)
    // because the prefix comes from `kName`.
    board.begin(FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);
    board.setConnectionTimeoutEnabled(false);

    // Attach `LedAnimator` to every hub-local PWM port so the LightFx
    // programs can drive them.  `LightFxEffectService::begin()` already
    // recorded the claim, but the underlying port has no role-variant
    // populated until ROLE_ATTACH lands — without this loop the role
    // service NACKs LED_QUEUE_LOAD with ROLE_KIND_MISMATCH and the
    // LEDs stay dark.  In production this is normally driven from
    // YAML config or Studio's role-attach UI; we do it inline here
    // until config-load lands.
    {
        using hubfx::effects::PortRef;
        auto& topo = board.policy<HubFxTopologyService>();
        uint8_t attached = 0;
        for (uint8_t ch = 0; ch < 8; ++ch) {
            if (topo.attachRole(PortRef::local(PortKind::Pwm, ch),
                                RoleKind::LedAnimator)) {
                ++attached;
            }
        }
        SFX_LOG_INFO("[LightFx] attached LedAnimator to %u/8 hub PWM ports",
                     (unsigned)attached);
    }

    // Stream every INFO+ log line live as TAG_ASYNC LOG_MESSAGE packets
    // so the host's `subscribe` command sees the device tail in real
    // time.  Entries continue to be buffered in the ring so DIAG_HISTORY
    // still works for historical / late-attach inspection.  Bump to
    // `DiagLevel::DEBUG` (here and via `setMinLevel`) when investigating
    // the LightFx → LedAnimator wire path — there are DEBUG traces at
    // every encode/decode/tick boundary.
    DiagLog::instance().setWireMinLevel(DiagLevel::INFO);

    // Replay the silent initHardware() snapshot now that DiagLog has a
    // serial sink (the probes happened before board.begin() so we
    // captured them into hwInit{} and emit here).
    board.logHardwareStatus();

    // I²C bus introspection — register the scan callback and mark every
    // chip physically wired on this PCB rev as "expected".
    board.enableI2CScan(Wire);
    board.addExpectedI2CDevice(I2cAddr::TAS5825P);
    board.addExpectedI2CDevice(I2cAddr::PCA9685);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH1);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH2);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH3);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH4);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH5);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH6);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH7);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH8);

    // Audio bring-up: Phase 1 (Core 0 codec probe + mixer alloc) →
    // Phase 1B Core 1 task launch (I²S init + producer spawn + consumer
    // loop) → Phase 2 (wait for I²S ready + 50 ms + codec activate to PLAY).
    audio.begin({
        .i2sDout  = Gpio::I2S_DOUT,
        .i2sBclk  = Gpio::I2S_BCLK,
        .i2sLrclk = Gpio::I2S_LRCLK,
    });

    // Storage ↔ audio exclusivity (Rule 28): pause the mixer on upload
    // start, resume on end.  Without this, raw-stream uploads starve
    // the audio task and you hear glitches mid-transfer.
    wireUploadExclusivity<Mixer>(board.policy<StorageService>());

    // Alert chimes — re-use the per-board init/error WAVs already on
    // the SD card as the four severity slots.  This is install-time
    // configuration; a YAML-driven path will land with ConfigService.
    {
        hubfx::effects::alerts::AlertServiceConfig cfg;
        cfg.enabled = true;
        cfg.channel = hubfx::effects::audio::HubFxLayout::Alert;
        strncpy(cfg.info.path,
                "/sounds/sys/hubfx_initialized.wav", sizeof(cfg.info.path) - 1);
        strncpy(cfg.warning.path,
                "/sounds/sys/lightfx_detected.wav", sizeof(cfg.warning.path) - 1);
        strncpy(cfg.error.path,
                "/sounds/sys/lightfx_fw_error.wav", sizeof(cfg.error.path) - 1);
        strncpy(cfg.critical.path,
                "/sounds/sys/gunfx_fw_error.wav", sizeof(cfg.critical.path) - 1);
        cfg.info.volume     = 70;
        cfg.warning.volume  = 80;
        cfg.error.volume    = 90;
        cfg.critical.volume = 100;
        board.policy<AlertService>().configure(cfg);
    }

    // ExpanderService log hooks — echo connect / identified / disconnect
    // events to the diag log as they happen during USB enumeration.
    // Studio / CLI also subscribe via the async wire events
    // (EXPANDER_CONNECTED / IDENTIFIED / DISCONNECTED) regardless.
    auto& exp = board.policy<HubFxExpanderService>();
    exp.onConnect([](const hubfx::expanders::ExpanderEntry& e) {
        SFX_LOG_INFO("[Expander] mount %s addr=%u vid=%04X pid=%04X",
                     hubfx::expanders::ExpanderKind::getName(e.kind),
                     e.usbAddr, e.vid, e.pid);
    });
    exp.onIdentified([](const hubfx::expanders::ExpanderEntry& e) {
        SFX_LOG_INFO("[Expander] identified %s guid=%s v%s",
                     hubfx::expanders::ExpanderKind::getName(e.kind),
                     e.spec.guid, e.spec.firmwareVersion);
    });
    exp.onDisconnect([](const hubfx::expanders::ExpanderEntry& e) {
        SFX_LOG_INFO("[Expander] unmount %s addr=%u guid=%s",
                     hubfx::expanders::ExpanderKind::getName(e.kind),
                     e.usbAddr, e.spec.guid);
    });

    // Auto-begun policies — surface their initial state so the boot
    // log confirms they came up clean.
    SFX_LOG_INFO("[InputDispatcher] up — %u/%u bindings",
                 (unsigned)board.policy<InputDispatcherService>().numBindings(),
                 (unsigned)InputDispatcherService::kMaxBindings);
    SFX_LOG_INFO("[LandingLight] up — %u/%u lights configured",
                 (unsigned)board.policy<LandingLightService>().count(),
                 (unsigned)hubfx::effects::landing::kMaxLandingLights);
    SFX_LOG_INFO("[LightFx] up — 1 program configured (\"AllOn\", 8 PWM channels)");

    SFX_LOG_INFO("HubFX v%s build %u — 8 PWM / 1 input / 11 servo-out + storage + audio + alerts + USB host + topology + input-dispatcher + landing + lightfx",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER);
}

void loop() {
    // Upload is exclusive on HubFX (Rule 28).  While a file upload is
    // in progress, drain only the storage server — audio is suspended
    // by `wireUploadExclusivity()` above, so the COBS reader, fill
    // buffer, and SD writer get the full Core 0 budget.  Raw-stream
    // mode bypasses the COBS framer.
    auto& storage = board.policy<StorageService>();
    if (storage.isUploadActive()) {
        if (storage.isStreamActive()) {
            storage.processStream();
        } else {
            board.process();
        }
        storage.checkUploadTimeout();
        return;
    }

    board.process();
    storage.checkUploadTimeout();
    board.pollSense();
    vTaskDelay(pdMS_TO_TICKS(1));
}
