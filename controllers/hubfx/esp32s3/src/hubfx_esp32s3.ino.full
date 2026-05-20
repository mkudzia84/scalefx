/*
 * HubFX ESP32-S3 — board declaration.
 *
 *  ── PWM ports (8) — PCA9685 channels with per-rail INA226 sense ────
 *  PCA9685 channel │ HubFX CH │ INA226 addr
 *      0           │ CH1      │ 0x40   (also battery / rail input)
 *      1           │ CH2      │ 0x41
 *      2           │ CH3      │ 0x42
 *      3           │ CH4      │ 0x43
 *      4           │ CH5      │ 0x44
 *      5           │ CH6      │ 0x45
 *      6           │ CH7      │ 0x4A
 *      7           │ CH8      │ 0x4F
 *
 *  ── Input port (1) — multi-modal RC receiver line on IN_1 ──────────
 *  Port idx │ Net    │ GPIO │ UART │ Modes
 *      0    │ IN_1   │  5   │  1   │ PULSE (RC PWM) / SBUS / JETI_EX / UART_RAW
 *
 *  The mode is selected at runtime by the attached role
 *  (`RcPwmInputRole` → PULSE, `SbusInputRole` → SBUS,
 *  `JetiExInputRole` → JETI_EX).  Rule 31 caps the declared input count
 *  ≤ free UART peripherals — UART0 is the USB console, so this one
 *  port consumes UART1, leaving UART2 free for future expansion.
 *
 *  ── Servo OUTPUT (11) — RC headers repurposed as actuator outputs ──
 *  Port idx │ Net    │ GPIO    Port idx │ Net    │ GPIO
 *      0    │ IN_2   │  6         6    │ IN_8   │ 14
 *      1    │ IN_3   │  7         7    │ IN_9   │ 15
 *      2    │ IN_4   │ 10         8    │ IN_10  │  4
 *      3    │ IN_5   │ 11         9    │ IN_11  │  3
 *      4    │ IN_6   │ 12        10    │ IN_12  │  2
 *      5    │ IN_7   │ 13
 *
 *  H-bridge ports: none on this PCB.
 *
 *  Not yet declared:
 *    - TAS5825P audio (subsystem, not a port — needs AudioServicePolicy)
 *    - SD card / LittleFS / USB Host (subsystems, not ports)
 */

#define FIRMWARE_VERSION "2.3.0"
#define BUILD_NUMBER 52

#include <Arduino.h>
#include <Wire.h>

#include <platform/sfx_platform.h>
#include <serial/diag_log.h>
#include <serial/serial.h>

#include <server/board_of.h>
#include <ports/pwm_port.h>
#include <ports/servo_port.h>
#include <ports/esp_input_port.h>
#include <pwm/pca9685.h>
#include <power/ina226.h>
#include <power/ina226_sensor.h>

// Storage (LittleFS flash + SD_MMC 4-bit SDIO) — both compiled in via
// SFX_HAS_STORAGE + SFX_HAS_STORAGE_SERVER (see platformio.ini build_flags).
// `bringUpStorage()` does the hardware probe in one call.
#include <storage/bring_up.h>
#include <server/storage_service.h>

// Audio (8-channel WAV mixer + I²S output + TAS5825P codec) — compiled
// in via SFX_HAS_AUDIO.  `EspDualCoreAudio<Mixer>` owns the two-phase
// boot dance (Core 0 codec probe + mixer alloc → Core 1 I²S task →
// Core 0 codec activate to PLAY) so the sketch never sees the atomics
// or task hooks.  The helper extracts the codec type from
// `Mixer::Codec` and dispatches probe/activate through a `CodecAdapter`
// trait the board specializes below — fully template-monomorphized,
// no `std::function`, no virtual dispatch.  Passive-DAC boards inherit
// the primary template (no-op) and the probe/activate paths compile out.
//
// Codec variant on this PCB is the TAS5825**P** (Class-H + Hybrid-Pro,
// RHB QFN-32), NOT the M-variant — pin map + init flow cross-referenced
// with `tests/hw/sfx_test_p/`.  P-flow is permissive (no FS_MON gate
// between HIZ and PLAY).  PDN / MUTE are hardwired HIGH via 10 kΩ
// pull-ups on this rev; codec is enabled + unmuted from power-on, mode
// control is purely via DEVICE_CTRL2 over I²C.
#include <audio/audio_mixer.h>
#include <audio/esp_i2s_output.h>
#include <audio/esp_dual_core_audio.h>
#include <audio/upload_exclusivity.h>
#include <codec/tas5825_p_codec.h>
#include <server/audio_service.h>

#include "expanders/expander_service.h"
#include "topology/topology_service.h"
#include "effects/audio_layout.h"
#include "effects/input/input_dispatcher.h"
#include "effects/alerts/alert_service.h"
#include "effects/landing_lights/landing_light_service.h"
#include "effects/lightfx/lightfx_service.h"
#include "effects/gearcontrol/gearcontrol_service.h"
#include "effects/enginefx/enginefx_service.h"
#include "effects/gunfx/gunfx_service.h"

// Topology service binds to the HubFX-flavoured expander service (2 USB
// ports, 4-entry GUID history).  Spelled out at the sketch level so
// neither library has to depend on the other.
using HubFxTopologyService =
    hubfx::topology::TopologyServicePolicyT<hubfx::expanders::ExpanderServicePolicy>;

// LandingLight + LightFx + GearControl effect services — all
// parameterised on the concrete topology type so dispatch stays
// static (Rule 33).
using InputDispatcherService =
    hubfx::effects::input::InputDispatcherServicePolicyT<HubFxTopologyService>;
using LandingLightService =
    hubfx::effects::landing::LandingLightServicePolicyT<HubFxTopologyService>;
using LightFxEffectService =
    hubfx::effects::lightfx::LightFxEffectServicePolicyT<HubFxTopologyService,
                                                          LandingLightService>;
using GearControlService =
    hubfx::effects::gearctrl::GearControlServicePolicyT<HubFxTopologyService,
                                                         LandingLightService>;

// `StorageService` is the platform-selected alias from sfx_storage —
// ESP32-S3 picks `StorageServicePolicy<Esp32StoragePolicy>` (64 KB PSRAM
// fill buffer, single-core synchronous writes; see Rule 28).

// Audio stack — board-specific composition.  ESP32-S3 + TAS5825P for
// the HubFX 8-channel rev.  Replace with `PCM5102ACodec` for a board
// with a passive DAC (paired with the `NoCodec` adapter on
// `EspDualCoreAudio`).
using Mixer        = AudioMixer<EspI2SOutput, TAS5825PCodec>;
using AudioService = AudioServicePolicy<Mixer>;

// EngineFX requires the mixer type — declared after `Mixer` above.
using EngineFxService =
    hubfx::effects::enginefx::EngineFxServicePolicyT<Mixer,
                                                      HubFxTopologyService,
                                                      InputDispatcherService>;
using GunFxService =
    hubfx::effects::gunfx::GunFxServicePolicyT<Mixer,
                                                HubFxTopologyService,
                                                InputDispatcherService>;
using AlertService =
    hubfx::effects::alerts::AlertServicePolicyT<Mixer>;

// ════════════════════════════════════════════════════════════════════════
//  Board pin / address map (DevKitC-1 + HubFX 8-channel rev)
// ════════════════════════════════════════════════════════════════════════
//
//  The named-constant blocks below mirror the PCB silkscreen: every
//  net on the board has exactly one entry here, and every consumer
//  below uses the symbolic name (never a raw literal).  Adding /
//  re-routing a net means updating one line in this section.

/// GPIO pin numbers, named by their schematic net.
namespace Gpio {
    constexpr int I2C_SDA        =  8;
    constexpr int I2C_SCL        =  9;
    constexpr int LED_CONNECTION = 48;   ///< on-board status LED
    constexpr int LED_ERROR      = -1;   ///< no dedicated error LED on this rev

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

    // SD card — 4-bit SDIO via SD_MMC slot 1.  ESP32-S3 GPIO matrix
    // routes any GPIO; nets match the PCB silkscreen.
    constexpr int SD_CMD         = 40;   ///< net SD_DCMD,  U26.43
    constexpr int SD_CLK         = 41;   ///< net SD_CLK,   U26.44
    constexpr int SD_D0          = 42;   ///< net SD_DATA0, U26.45
    constexpr int SD_D1          = 43;   ///< net SD_DATA1, U26.47
    constexpr int SD_D2          = 44;   ///< net SD_DATA2, U26.48
    constexpr int SD_D3          = 47;   ///< net SD_DATA3, U26.51

    // I²S to TAS5825P codec (Philips standard mode, 48 kHz / 16-bit
    // stereo).  Pin map cross-referenced with tests/hw/sfx_test_p/:
    constexpr int I2S_DOUT       = 16;   ///< net TAS_DI,   U26.22 → codec DIN
    constexpr int I2S_BCLK       = 17;   ///< net TAS_BCK,  U26.23 → codec BCK
    constexpr int I2S_LRCLK      = 18;   ///< net TAS_FS,   U26.24 → codec FS / LRCLK
}

/// I²C device addresses on Wire (SDA/SCL above).
namespace I2cAddr {
    constexpr uint8_t PCA9685    = 0x70;   ///< 8-channel PWM driver
    constexpr uint8_t TAS5825P   = 0x4C;   ///< Class-H audio codec (ADDR pin → GND)

    // Per-channel current/voltage monitors — channel index maps to
    // PWM port index (CH1 = port 0, CH8 = port 7).
    constexpr uint8_t INA226_CH1 = 0x40;
    constexpr uint8_t INA226_CH2 = 0x41;
    constexpr uint8_t INA226_CH3 = 0x42;
    constexpr uint8_t INA226_CH4 = 0x43;
    constexpr uint8_t INA226_CH5 = 0x44;
    constexpr uint8_t INA226_CH6 = 0x45;
    constexpr uint8_t INA226_CH7 = 0x4A;
    constexpr uint8_t INA226_CH8 = 0x4F;
}

/// ESP32 UART peripheral numbers (UART0 is the USB-CDC console).
namespace Uart {
    constexpr uint8_t IN_1 = 1;            ///< claimed by InputPort on IN_1
}

/// Per-channel current-sense hardware constants (matches the per-channel rev).
namespace Sense {
    constexpr float    INA226_SHUNT_OHMS = 0.1f;   ///< 0.1 Ω shunt
    constexpr float    INA226_MAX_AMPS   = 3.2f;   ///< ±3.2 A range
}

/// PCA9685 PWM-driver settings.
namespace Pwm {
    /// 1526 Hz — the chip's POR ceiling and well above the LED
    /// flicker-fusion threshold.
    constexpr uint16_t FREQ_HZ = 1526;
}

/// Audio codec hardware constants for the HubFX 3S-LiPo rev.
namespace Codec {
    /// 3S LiPo (~12 V nominal) — selects AGAIN table at activate-time.
    constexpr auto SUPPLY_VOLTAGE = sfx_audio::tas5825::Supply::V12;
}

// ── Board class ───────────────────────────────────────────────────────

class HubFxBoard : public sfx_core::BoardOf<HubFxBoard,
                                              hubfx::expanders::ExpanderServicePolicy,
                                              HubFxTopologyService,
                                              InputDispatcherService,
                                              LandingLightService,
                                              LightFxEffectService,
                                              GearControlService,
                                              EngineFxService,
                                              GunFxService,
                                              AlertService,
                                              StorageService,
                                              AudioService> {
public:
    // Hardware drivers — declaration order matters (init order).
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

    // Bring the chips up.  Must run before board.begin() — the
    // Pca9685PwmPort begin() calls setDuty(0) which needs the chip
    // already alive.
    void initHardware() {
        Wire.begin(Gpio::I2C_SDA, Gpio::I2C_SCL);
        Wire.setClock(400000);
        if (!pca.begin(Wire, I2cAddr::PCA9685, Pwm::FREQ_HZ)) {
            SFX_LOG_WARN("PCA9685 @ 0x%02X: init failed", I2cAddr::PCA9685);
        } else {
            SFX_LOG_INFO("PCA9685 @ 0x%02X: %u Hz", I2cAddr::PCA9685, Pwm::FREQ_HZ);
        }

        constexpr uint8_t kInaAddrs[8] = {
            I2cAddr::INA226_CH1, I2cAddr::INA226_CH2, I2cAddr::INA226_CH3, I2cAddr::INA226_CH4,
            I2cAddr::INA226_CH5, I2cAddr::INA226_CH6, I2cAddr::INA226_CH7, I2cAddr::INA226_CH8,
        };
        uint8_t inaOk = 0;
        for (uint8_t k = 0; k < 8; k++) {
            if (ina[k].begin(Wire, kInaAddrs[k],
                             Sense::INA226_SHUNT_OHMS, Sense::INA226_MAX_AMPS)) {
                inaOk++;
            } else {
                SFX_LOG_WARN("INA226 @ 0x%02X: not found", kInaAddrs[k]);
            }
        }
        SFX_LOG_INFO("INA226: %u/8 monitors up", inaOk);
    }

    // Per-loop maintenance: refresh INA226 cached readings.  The input
    // port is driven by edge IRQ (pulse mode) or the UART RX FIFO
    // (SBUS / Jeti EX) — no tick needed here.
    void pollSense() {
        for (uint8_t k = 0; k < 8; k++) ina[k].update();
    }

    // ── Port declarations (compile-time, consumed by BoardOf<>) ───────
    //
    //   8 × PWM    (PCA9685 channels with INA226 sense)
    //   1 × Input  (IN_1 — PULSE / SBUS / JETI_EX, role-selected)
    //  11 × Servo  (IN_2..IN_12 — output actuator headers)

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

// CodecAdapter specialization for HubFX's TAS5825P — lives in its own
// header so all board-specific audio plumbing (channel map + codec
// wiring) is co-located in `effects/`.  Must come AFTER the `Gpio::`
// and `Codec::` namespaces above; the header references them directly.
#include "effects/audio_codec.h"

// Dual-core audio bring-up — `EspDualCoreAudio<>` owns the atomic
// flags, the Core 1 consumer task, the producer task spawn, and the
// two-phase orchestration.  Codec is inferred from `Mixer::Codec`.
static EspDualCoreAudio<Mixer> audio;

void setup() {
    // ── Hardware ──────────────────────────────────────────────────────
    board.initHardware();
    bringUpStorage({
        .clk = Gpio::SD_CLK, .cmd = Gpio::SD_CMD,
        .d0  = Gpio::SD_D0,  .d1  = Gpio::SD_D1,
        .d2  = Gpio::SD_D2,  .d3  = Gpio::SD_D3,
    });

    // ── Policy pack lifecycle ────────────────────────────────────────
    // TopologyService resolves its expander / role / registry deps
    // through `findPolicy<>()` inside its own begin().
    board.begin(FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);
    board.setConnectionTimeoutEnabled(false);  // master — no upstream watchdog

    // ── Audio ────────────────────────────────────────────────────────
    // Two-phase bring-up: Core 0 codec probe + mixer alloc, Core 1
    // I²S task, Core 0 codec activate to PLAY.  Codec-specific bits
    // are encapsulated in the `HubFxCodec` adapter type above.
    audio.begin({
        .i2sDout  = Gpio::I2S_DOUT,
        .i2sBclk  = Gpio::I2S_BCLK,
        .i2sLrclk = Gpio::I2S_LRCLK,
    });

    // Storage ↔ audio exclusivity (Rule 28): stop + suspend mixer on
    // upload start, resume on end.  STATUS_UPDATE suppression is
    // already auto-wired inside StorageServicePolicy.
    wireUploadExclusivity<Mixer>(board.policy<StorageService>());

    SFX_LOG_INFO("HubFX v%s build %u — 8 PWM / 1 input / 11 servo-out / USB / storage / audio",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER);
}

void loop() {
    // Upload is exclusive on HubFX (Rule 28).  While a file upload is
    // in progress, service ONLY the storage server + its timeout
    // check — every other subsystem is starved on purpose so the COBS
    // reader, 64 KB fill buffer, and synchronous SD writer get the
    // full Core 0 budget.  When raw-stream mode is active, bypass the
    // COBS framer entirely and let the storage server consume bytes
    // off Serial directly.
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

    // One-tick yield so the FreeRTOS idle task can refresh the watchdog
    // and any same-core lower-priority tasks (e.g. battery sampling
    // when it lands) get scheduled.  Audio is unaffected — both
    // producer and consumer live on Core 1.
    vTaskDelay(pdMS_TO_TICKS(1));
}
