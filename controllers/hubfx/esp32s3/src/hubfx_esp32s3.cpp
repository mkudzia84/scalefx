/*
 * HubFX ESP32-S3 — master controller firmware.
 *
 *   Every effect / service the master runs is a `*ServicePolicy` in
 *   the `BoardOf<>` template-parameter pack below.  Boot order is
 *   left-to-right; capability bits are OR'd into the IDENTIFY payload
 *   automatically.  See instructions/17-SYSTEM-SERVICES.md for the
 *   service-policy authoring rules.
 *
 *   Stack:
 *     - BoardServicePolicy        (auto, lifecycle + INIT_READY + STATUS)
 *     - IndicatorServicePolicy    (auto, status LEDs)
 *     - PortServicePolicy         (BoardOf<>, hub-local port enumeration)
 *     - RoleServicePolicy         (BoardOf<>, hub-local role dispatch)
 *     - ExpanderService           (USB host + IDENTIFY of remote boards)
 *     - TopologyService           (GUID-addressed port/role surface)
 *     - InputDispatcherService    (RC / SBUS / Jeti channel-event fanout)
 *     - LandingLightService       (servo + LED-group landing lights)
 *     - LightFxEffectService      (LED program runtime; programs loaded
 *                                  from /lightfx/programs/<name>.yaml)
 *     - GearControlService        (gear retract / extend state machines)
 *     - EngineFxService           (engine sound on mixer + throttle binding)
 *     - GunFxService              (gun trigger / smoke / recoil)
 *     - StorageService            (LittleFS + SD_MMC 4-bit SDIO)
 *     - AudioService              (8-channel mixer + TAS5825P, dual-core)
 *     - AlertService              (system-alert chimes on HubFxLayout::Alert)
 *     - ConfigServicePolicy       (six config stores side-by-side;
 *                                  CONFIG_RELOAD / CONFIG_SAVE wire surface)
 *
 *   Ports declared on the HubFX PCB (pin map selected by HUBFX_PCB_REV —
 *   see the Board pin/address map section below + PINOUT.md):
 *     - 8 × PWM    (PCA9685 channels; rev A adds per-channel INA226 V/I
 *                   sense — rev B monitors RAILS with 2 INA226 instead)
 *     - 2 × Input  (IN_1 UART1 multi-modal PULSE/SBUS/JETI_EX;
 *                   IN_2 UART2 Jeti EX Bus telemetry monitor → Rx;
 *                   rev B: split TX/RX headers, 2.2 kΩ bridge)
 *     - 10 × Servo (SRV1..SRV10 on rev B / IN_3..IN_12 on rev A)
 *
 *   Configuration sources (LittleFS) — /hubfx.yaml is the board master,
 *   every effect's details live in its own canonical sub-file:
 *
 *     /hubfx.yaml                  BOARD CONFIG:
 *                                    audio.codec_supply (PVDD rail)
 *                                    features.* (master enable matrix)
 *                                    ports[]     (hub-local port → role)
 *                                    expanders[] (remote boards: alias →
 *                                                 guid + nested port→role;
 *                                                 effects address by alias)
 *                                    inputs[]  (named RC channels —
 *                                               effects reference by name)
 *     ├── /alerts.yaml             severity → AlertSound + volume
 *     ├── /enginefx.yaml           engine wiring + sound pack
 *     ├── /landing.yaml            landing-light defs (servo + LED group)
 *     ├── /gearcontrol.yaml        gear defs (motor + LEDs by GUID PortRef)
 *     └── /lightfx.yaml            master brightness + program path list
 *           └── /lightfx/programs/<n>.yaml   one file per LED program,
 *                                            referenced by FULL PATH
 *                                            from /lightfx.yaml.
 *
 *   Missing files fall back to schema defaults so a bare board still
 *   boots functional.  See instructions/19-HUBFX-CONFIG-SCHEMA.md and
 *   media/README.md for the on-disk preset library.
 */

#define FIRMWARE_VERSION "2.41.0-hubfx"
#define BUILD_NUMBER     939

// Developer-facing diagnostic emission gate (set in platformio.ini).
// =1 keeps the periodic [mem]/[stack] snapshot, the boot static-
// footprint dump, live telemetry, and the preload-status dump; =0
// (release) compiles them out.  Fallback keeps the file compilable
// standalone.  Does NOT gate error logging or the hardware-fault probe.
#ifndef SFX_INSTRUMENTATION
#define SFX_INSTRUMENTATION 1
#endif

// No <Arduino.h> — the sketch is native: setup()/loop() are the only Arduino
// touch-point (the framework-arduino entry bridge calls them), and everything
// else is ESP-IDF / FreeRTOS.  Dropping framework=arduino + moving to app_main
// is the final tracked migration step (see 25-ARDUINO-REMOVAL.md).
#include <freertos/FreeRTOS.h>     // vTaskDelay / pdMS_TO_TICKS
#include <freertos/task.h>
#include "hubfx_i2c.h"            // hubI2cBus() (native I2C bus)
#include <esp_heap_caps.h>     // memory-instrumentation helper (Phase 4 polish 2026-05-27)
#include <esp_psram.h>
#include <esp_system.h>        // esp_reset_reason() — boot-time reset diagnosis

// Phase 5 of feature/idf-component-build (2026-05-28): pull esp-dsp's
// hand-tuned Xtensa LX7 SIMD into the build link graph.  The header is
// included so the symbol is referenced from setup() / the mixer at
// link time; the REQUIRES in src/CMakeLists.txt declares the IDF-side
// dependency.  Used by audio_mixer.ipp's produceBlock() (Phase 5).
#include <dsps_mulc.h>
#include <dsps_add.h>

#include <platform/sfx_platform.h>
#include <serial/diag_log.h>
#include <server/board_of.h>
#include <server/effect_clock.h>

// ── Port hardware drivers ────────────────────────────────────────────
#include <ports/pwm_port.h>
#include <ports/servo_port.h>
#include <ports/esp_input_port.h>
#include <jeti_ex/jeti_expander.h>   // HubFX-own Jeti telemetry (Version)
#include <pwm/pca9685.h>
#include <power/ina226.h>
#include <power/ina226_sensor.h>

// Storage (LittleFS flash + SD_MMC 4-bit SDIO) — compiled in via
// SFX_HAS_STORAGE + SFX_HAS_STORAGE_SERVER (platformio.ini build_flags).
#include <storage/bring_up.h>
#include <audio/audio_asset_cache.h>      // AudioAssetCache — PSRAM asset preloader
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
#include "effects/enginefx/enginefx_service.h"
#include "effects/gearcontrol/gearcontrol_service.h"
#include "effects/gunfx/gunfx_service.h"
#include "effects/input/input_dispatcher.h"
#include "effects/landing_lights/landing_light_service.h"
#include "effects/lightfx/lightfx_service.h"
#include "expanders/expander_service.h"
#include "topology/topology_service.h"

// Config — five parsed YAML stores fed into `ConfigServicePolicy`:
//   /hubfx.yaml     — BOARD CONFIG: audio.codec_supply + features matrix
//                     + ports[] role attachment
//   /alerts.yaml    — severity → AlertSound + volume
//   /enginefx.yaml  — engine wiring + sound pack
//   /landing.yaml   — landing-light defs (servo + LED group)
//   /lightfx.yaml   — master brightness + program-file path list,
//                     each loading /lightfx/programs/<n>.yaml in turn.
// Each store has its own onLoaded callback that fans the parsed data
// into the matching service.  Every effect references ports via
// {kind, idx} PortRefs (parsed by the shared port_ref_yaml.h helper)
// and assumes /hubfx.yaml's ports[] block already attached the role.
// See instructions/19-HUBFX-CONFIG-SCHEMA.md.
#include <config/config_store.h>
#include <server/multi_config_server.h>     // ConfigServicePolicy
#include <storage/storage_config_bridge.h>  // wireConfigStore<FlashModule>
#include <storage/flash.h>
#include "config/hubfx_config.h"
#include "config/alerts_config.h"
#include "config/enginefx_config.h"
#include "config/gunfx_config.h"
#include "config/landing_config.h"
#include "config/gearcontrol_config.h"
#include "config/lightfx_config.h"
#include "config/lightfx_program_selector.h"
#include "config/landing_activation.h"
#include "config/gear_activation.h"
#include "config/apply_hubfx_config.h"
#include "config/config_store_slot.h"
#include "config/telemetry_emitter.h"

// ════════════════════════════════════════════════════════════════════════
//  Board pin / address map — selected by HUBFX_PCB_REV (see PINOUT.md):
//    1 = 2025 8-channel rev A — single-wire RC inputs on IN_1/IN_2,
//        servo headers IN_3..IN_12, 8 × per-PWM-channel INA226, LED GPIO48.
//    2 = 2026-07 rev B (pcb-nextver) — dedicated INP + TELEM headers with
//        SPLIT TX/RX (header line on the TX pad, bridged to the RX pad by
//        a 2.2 kΩ resistor), servo headers SRV1..SRV10, 2 × INA226
//        (expander/USB rail + battery, low-side 10 mΩ), LED GPIO46.
//  Derived from the EasyEDA Telesis netlist in hardware/pcb-nextver/.
//  Override with -DHUBFX_PCB_REV=1 (platformio.ini) to flash a rev A board.
// ════════════════════════════════════════════════════════════════════════

#ifndef HUBFX_PCB_REV
#define HUBFX_PCB_REV 2
#endif

namespace Gpio {
    // I²C bus — shared with TAS5825P codec, PCA9685, INA226 monitors.
    // Same pins on every rev.
    constexpr int I2C_SDA        =  8;
    constexpr int I2C_SCL        =  9;

#if HUBFX_PCB_REV >= 2
    // On-board status LED (GREEN_LED net).
    constexpr int LED_CONNECTION = 46;
    constexpr int LED_ERROR      = -1;

    // Input interfaces (Rule 31: 2 inputs max) — both are split-pin
    // half-duplex headers: the header signal pin sits DIRECTLY on the TX
    // GPIO and reaches the RX GPIO through a 2.2 kΩ series bridge (R8 /
    // R4), so RX listens continuously while TX idles high-Z and drives
    // only its reply slot (EspInputPort txEnable/txDisable).
    constexpr int IN1_RX         =  2;   // JETIX_RX0    — INP header (RC / EX Bus)
    constexpr int IN1_TX         =  1;   // JETIX_TX0    — INP header line
    constexpr int IN2_RX         = 21;   // TELEMETRI_RX1 — TELEM header (downstream EX telemetry)
    constexpr int IN2_TX         =  3;   // TELEMETRI_TX1 — TELEM header line

    // Servo output headers SRV1..SRV10 (+6 V rail).
    constexpr int SERVO_PIN[10]  = { 5, 6, 7, 10, 11, 12, 13, 14, 15, 4 };
#else
    // On-board status LED.
    constexpr int LED_CONNECTION = 48;
    constexpr int LED_ERROR      = -1;

    // Single-wire RC inputs — IN_1 (main) + IN_2 (telemetry); TX is
    // matrix-routed onto the same pad for the Jeti reply slot.
    constexpr int IN1_RX         =  5;   // IN_1 header
    constexpr int IN2_RX         =  6;   // IN_2 header

    // Servo output headers IN_3..IN_12 (5 V rail).
    constexpr int SERVO_PIN[10]  = { 7, 10, 11, 12, 13, 14, 15, 4, 3, 2 };
#endif

    // I²S to TAS5825P codec (Philips standard mode, 48 kHz / 16-bit).
    // Same pins on every rev.
    constexpr int I2S_DOUT       = 16;   // TAS_DI  → codec DIN
    constexpr int I2S_BCLK       = 17;   // TAS_BCK → codec BCK
    constexpr int I2S_LRCLK      = 18;   // TAS_FS  → codec FS / LRCLK

    // SD card — 4-bit SDIO via SD_MMC slot 1.  GPIO matrix routed; the
    // PCB designer's authoritative map skips GPIO 43/44 because UART0
    // (CH343 console bridge) is hardwired to those pins.  Same on every rev.
    constexpr int SD_CMD         = 38;
    constexpr int SD_CLK         = 39;
    constexpr int SD_D0          = 40;
    constexpr int SD_D1          = 41;
    constexpr int SD_D2          = 42;
    constexpr int SD_D3          = 45;
}

namespace I2cAddr {
    constexpr uint8_t PCA9685  = 0x70;   // 8-channel PWM driver
    constexpr uint8_t TAS5825P = 0x4C;   // Class-H audio codec (ADDR → GND)
}

#if HUBFX_PCB_REV >= 2
// Rev B carries TWO INA226 rail monitors:
//   [0] U44 @ 0x41 (A1=GND A0=VS)   — battery total: low-side 10 mΩ (R9)
//       between the BAT- terminal and the ground plane, VBUS pin on VBAT.
//       Reads BATTERY voltage on the bus channel, POSITIVE current on load.
//   [1] U43 @ 0x44 — expander/USB-rail current: low-side 10 mΩ (R83) in the
//       USB1/USB4 ground return (GND_1).
// U43 originally strapped to 0x40, which COLLIDES with the PCA9685's HARDWARE
// address (all six A-pins grounded — firmware's 0x70 is only its all-call
// alias): reads at 0x40 wire-AND two chips (garbage IDs) and writes land in
// PCA MODE1 (the historic "clone wedge").  Relocated to 0x44 — bench rework
// (lift U43 A0, re-strap) 2026-07-14, confirmed clash-free via tests/hw/
// i2c_probe (2 canonical INA226, 0 clashes); PCB REV C bakes the restrap in.
// On a STOCK un-reworked rev B board 0x44 simply won't ACK — the canonical-ID
// gate logs "1/2 monitors up" and never drives 0x40.  Full writeup:
// PINOUT.md §Rev B + instructions/18.
constexpr uint8_t kInaAddrs[] = { 0x41, 0x44 };
constexpr const char* const kInaLabels[] = { "battery", "expander-rail" };

namespace Sense {
    constexpr float    INA226_SHUNT_OHMS = 0.010f;  // R83 / R9 — 10 mΩ
    constexpr float    INA226_MAX_AMPS   = 8.0f;    // 81.92 mV FS / 10 mΩ
}
#else
// Per-channel current/voltage monitor addresses — index maps directly to
// PWM port index (CH1 = port 0, CH8 = port 7).  The 0x4A / 0x4F outliers
// on ch7 / ch8 dodge the codec at 0x4C on the shared I²C bus.
constexpr uint8_t kInaAddrs[] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x4A, 0x4F,
};
constexpr const char* const* kInaLabels = nullptr;   // plain "ch N" logging

namespace Sense {
    constexpr float    INA226_SHUNT_OHMS = 0.1f;    // 0.1 Ω shunt
    constexpr float    INA226_MAX_AMPS   = 3.2f;    // ±3.2 A range
}
#endif

constexpr size_t kInaCount = sizeof(kInaAddrs) / sizeof(kInaAddrs[0]);

namespace Uart {
    constexpr uint8_t IN_1 = 1;            // UART1 — main Jeti EX Bus input (Rx channel data + telemetry to Rx)
    constexpr uint8_t IN_2 = 2;            // UART2 — Jeti EX Bus telemetry from a downstream slave (e.g. ESC): HubFX polls + collects, forwards over IN_1 to the Rx
}

namespace Pwm {
    /// 1526 Hz — the PCA9685 POR ceiling and well above the LED
    /// flicker-fusion threshold.
    constexpr uint16_t FREQ_HZ = 1526;
}

namespace Codec {
    /// TAS5825P PVDD-rail selector — picks the AGAIN table at codec
    /// activate.  Default 12 V (3S LiPo).  Driven by `/hubfx.yaml`'s
    /// `audio.codec_supply` field at boot via `applyHubFxConfigCallback`
    /// — declared mutable (not constexpr) so the YAML can override it
    /// before `audio.begin()` fires.  CONFIG_RELOAD updates the value
    /// but the codec only re-reads it on reboot.
    inline sfx_audio::tas5825::Supply SUPPLY_VOLTAGE =
        sfx_audio::tas5825::Supply::V12;
}

// CodecAdapter specialization for HubFX's TAS5825P.  MUST come after
// the `Gpio::` / `Codec::` namespaces — the header references them.
#include "effects/audio_codec.h"

// Board hardware probe — PCA9685 + INA226 boot diag + recovery, lifted
// out of the sketch into a dedicated helper.  Configured by the
// constexpr `kHubFxProbeCfg` below; instantiated as a member on
// HubFxBoard.
#include "hubfx_hw_probe.h"

static constexpr hubfx_hw::HubFxHardwareProbe::PinConfig kHubFxProbeCfg = {
    .sda          = Gpio::I2C_SDA,
    .scl          = Gpio::I2C_SCL,
    .i2cFreq      = 400000,
    .pcaAddr      = I2cAddr::PCA9685,
    .pwmFreqHz    = Pwm::FREQ_HZ,
    .inaShuntOhms = Sense::INA226_SHUNT_OHMS,
    .inaMaxAmps   = Sense::INA226_MAX_AMPS,
    .inaAddrs     = kInaAddrs,
    .inaCount     = kInaCount,
    .inaLabels    = kInaLabels,
};

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

// GearControl service — owns the per-gear (motor + door servos) state
// machines.  Talks to LandingLightService so a gear's program-bound
// landing-light pair follows retract/extend transitions automatically.
using GearControlService =
    hubfx::effects::gearctrl::GearControlServicePolicyT<HubFxTopologyService,
                                                         LandingLightService>;

// EngineFx service — engine startup / running / shutdown sound + idle
// jitter on a mixer channel, throttle-toggle bound to an RC channel
// via the input dispatcher.
using EngineFxService =
    hubfx::effects::enginefx::EngineFxServicePolicyT<Mixer,
                                                      HubFxTopologyService,
                                                      InputDispatcherService>;

// GunFx service — per-gun trigger fire + smoke generator + recoil
// servo on a mixer channel; input-dispatcher bindings for triggers.
using GunFxService =
    hubfx::effects::gunfx::GunFxServicePolicyT<Mixer,
                                                HubFxTopologyService,
                                                InputDispatcherService>;

// ── Board class ──────────────────────────────────────────────────────
//
// BoardOf<> auto-prepends BoardServicePolicy + IndicatorServicePolicy +
// PortServicePolicy + RoleServicePolicy.  User policies follow.

// Hub-local port counts — THE single source of truth.  Feeds three
// places that must agree: the backing C-arrays (pwm[]/in[]/servoOut[]),
// the `kXxxPorts` descriptor lists (`*_array<…, N>`), and the
// `PortRegistry` capacity (`PortCapacity<…>`, the Caps template arg of
// BoardOf — see board_of.h).  Sizing the registry EXACTLY to these
// reclaimed ~17 KB DRAM .bss vs the old generous 32/32/16/8 default.
//
// The registry covers HUB-LOCAL ports ONLY; expander ports (GearControl,
// LightFX, …) live on each expander's own registry and are reached by
// forwarding ROLE_ATTACH over CDC — adding expanders does NOT consume hub
// slots.  HubFX hub-local hardware is fixed by the PCB (both revs): 10
// servo headers + 8 PCA9685 PWM + 2 input interfaces.  Bump a count here
// (and the matching initializer list) when a board rev adds a header;
// every dependent size follows automatically.  A count below the
// initializer list is a compile error (too many initializers); a count
// the descriptor exceeds would make begin() clamp — so keep them equal.
namespace hubfx_caps {
constexpr size_t servo   = 10;   // Gpio::SERVO_PIN — SRV1..SRV10 (rev B) / IN_3..IN_12 (rev A)
constexpr size_t pwm     = 8;    // PCA9685 CH1..8
constexpr size_t hbridge = 0;    // no hub-local H-bridge on this rev
constexpr size_t input   = 2;    // IN_1 (UART1) + IN_2 (UART2) — Rule 31: ESP32-S3 max 2 inputs
}  // namespace hubfx_caps

// Servo header rail — rev B feeds SRV1..SRV10 from the +6 V buck; rev A's
// IN_3..IN_12 headers sat on the 5 V rail.  Flows to Go/Studio via the
// port descriptor (Rule 37) and drives sub-rail element duty scaling.
constexpr uint16_t kServoRail_mV = (HUBFX_PCB_REV >= 2) ? 6000 : 5000;

class HubFxBoard : public sfx_core::BoardOf<HubFxBoard,
                                             sfx::NativeUartStream,      // TStream (Rule 33)
                                             sfx_core::PortCapacity<     // registry sized from hubfx_caps
                                                 hubfx_caps::servo,
                                                 hubfx_caps::pwm,
                                                 hubfx_caps::hbridge,
                                                 hubfx_caps::input>,
                                             HubFxExpanderService,
                                             HubFxTopologyService,
                                             InputDispatcherService,
                                             LandingLightService,
                                             LightFxEffectService,
                                             GearControlService,
                                             EngineFxService,
                                             GunFxService,
                                             StorageService,
                                             AudioService,
                                             AlertService,
                                             ConfigServicePolicy> {
public:
    // ── Hardware drivers — declaration order matters (init order) ────
    PCA9685 pca;
    INA226  ina[kInaCount];

    sfx_peripherals::Pca9685PwmPort pwm[hubfx_caps::pwm] = {
        {pca, 0}, {pca, 1}, {pca, 2}, {pca, 3},
        {pca, 4}, {pca, 5}, {pca, 6}, {pca, 7},
    };
#if HUBFX_PCB_REV < 2
    // Rev A only — one INA226 per PWM channel feeds per-port V/I sense.
    // Rev B's two INAs monitor RAILS (battery + expander), not ports, so
    // the PWM descriptors carry no sense hookup there.
    sfx_peripherals::Ina226VoltageSensor vSense[hubfx_caps::pwm] = {
        {ina[0]}, {ina[1]}, {ina[2]}, {ina[3]},
        {ina[4]}, {ina[5]}, {ina[6]}, {ina[7]},
    };
    sfx_peripherals::Ina226CurrentSensor iSense[hubfx_caps::pwm] = {
        {ina[0]}, {ina[1]}, {ina[2]}, {ina[3]},
        {ina[4]}, {ina[5]}, {ina[6]}, {ina[7]},
    };
#endif

    // ── Input ports — IN_1 (PULSE/SBUS/JETI_EX) + IN_2 (Jeti EX Bus
    //    telemetry monitor on UART2).  Rev B uses the split TX/RX ctor
    //    (dedicated header pad); rev A the single-wire one. ────────────
#if HUBFX_PCB_REV >= 2
    sfx_peripherals::EspInputPort in[hubfx_caps::input] = {
        {Gpio::IN1_RX, Gpio::IN1_TX, Uart::IN_1},   // index 0 — main RC channel + telemetry-to-Rx
        {Gpio::IN2_RX, Gpio::IN2_TX, Uart::IN_2},   // index 1 — downstream EX Bus telemetry (e.g. ESC)
    };
#else
    sfx_peripherals::EspInputPort in[hubfx_caps::input] = {
        {Gpio::IN1_RX, Uart::IN_1},   // index 0 — main RC channel + telemetry-to-Rx
        {Gpio::IN2_RX, Uart::IN_2},   // index 1 — downstream EX Bus telemetry (e.g. ESC)
    };
#endif

    // ── Servo OUTPUT ports — Gpio::SERVO_PIN actuator headers ────────
    sfx_peripherals::MicroservoPort servoOut[hubfx_caps::servo] = {
        {Gpio::SERVO_PIN[0]}, {Gpio::SERVO_PIN[1]}, {Gpio::SERVO_PIN[2]},
        {Gpio::SERVO_PIN[3]}, {Gpio::SERVO_PIN[4]}, {Gpio::SERVO_PIN[5]},
        {Gpio::SERVO_PIN[6]}, {Gpio::SERVO_PIN[7]}, {Gpio::SERVO_PIN[8]},
        {Gpio::SERVO_PIN[9]},
    };

    // ── Hardware probe ───────────────────────────────────────────────
    //
    // PCA9685 + INA226 bring-up + boot diag + post-board.begin()
    // recovery all live in `hubfx_hw_probe.h`.  The probe captures a
    // silent snapshot during `init()` (before Serial / DiagLog are up)
    // and replays it via `logStatus()` once the wire stream is alive.
    hubfx_hw::HubFxHardwareProbe probe{pca, ina, pwm, kHubFxProbeCfg};

    // Thin forwarders to the probe — kept for the existing setup()
    // / loop() call sites.  See hubfx_hw_probe.h for implementations.
    void initHardware()      { probe.init(); }
    void logHardwareStatus() { probe.logStatus(); }
    void pollSense()         { probe.pollSense(); }


    // ── Port declarations (compile-time, consumed by BoardOf<>) ──────
    //
    // Rail voltages (Phase 0 of GunFX rollout, instructions/22):
    //   CH1..8   — 8 V buck output (drives heaters / fans / LED rings)
    //   inputs   — 3.3 V GPIO logic (IN_1 RC PWM/SBUS/Jeti EX; IN_2 Jeti EX Bus telemetry)
    //   servos   — kServoRail_mV (6 V buck on rev B; 5 V rail on rev A)
    // Effects use these to compute voltage-scaled PWM duty for sub-rail
    // elements (a 5 V smoke heater on the 8 V rail wants ~63 % duty).

#if HUBFX_PCB_REV < 2
    static constexpr auto kPwmPorts = sfx_core::ports::list(
        sfx_core::ports::pwm_array<&HubFxBoard::pwm, hubfx_caps::pwm>()
            .with_vSense_array<&HubFxBoard::vSense>()
            .with_iSense_array<&HubFxBoard::iSense>()
            .with_voltage_mV<8000>());
#else
    // Rev B: no per-channel INA — the PWM descriptors carry rail voltage only.
    static constexpr auto kPwmPorts = sfx_core::ports::list(
        sfx_core::ports::pwm_array<&HubFxBoard::pwm, hubfx_caps::pwm>()
            .with_voltage_mV<8000>());
#endif

    static constexpr auto kInputPorts = sfx_core::ports::list(
        sfx_core::ports::input_array<&HubFxBoard::in, hubfx_caps::input>()
            .with_voltage_mV<3300>());

    static constexpr auto kServoPorts = sfx_core::ports::list(
        sfx_core::ports::servo_array<&HubFxBoard::servoOut, hubfx_caps::servo>()
            .with_voltage_mV<kServoRail_mV>());

    static constexpr const char* kName = "HubFx";
};

HubFxBoard board;

/// Wire-protocol UART (Phase 4 of feature/idf-component-build, 2026-05-28).
/// Native ESP-IDF UART driver wrapped in a Stream-compatible class.
/// brought up explicitly here so the BoardOf<…, NativeUartStream, …>
/// hot path resolves UART read/write through TStream& direct calls
/// (no Stream* vtable per byte at 6 Mbps).  Pre-installed before
/// board.begin() so the wire is live the moment the protocol parser
/// starts running.
static sfx::NativeUartStream wireUart;

// Dual-core audio bring-up helper.  Owns the atomic flags, the Core 1
// consumer task, the producer task spawn, and the two-phase orchestration.
// Codec type is inferred from `Mixer::Codec`.
static EspDualCoreAudio<Mixer> audio;

// LightFx program catalog lives as per-file YAMLs under
// `/lightfx/programs/<name>.yaml`.  `applyHubFxConfig` walks the
// `lightfx.programs:` list from `/hubfx.yaml`, loads each file, and
// passes the resulting `Program[]` to LightFxEffectService.configure.
// A board with no program files boots LightFx empty — `light-programs`
// returns an empty list and no program activates by default.

// ── Config stores ───────────────────────────────────────────────────
//
// Five stores side-by-side on LittleFS, all wired into the same
// ConfigServicePolicy via `ConfigStoreSlot`.  CONFIG_RELOAD is
// path-routed — a single `config-reload` re-applies whichever file's
// path matches.
//
//   /hubfx.yaml     → applyHubFxConfig    — features enable matrix +
//                                            audio.codec_supply +
//                                            ports[] role attach +
//                                            inputs[] name map
//   /alerts.yaml    → applyAlertsConfig   — severity → AlertSound map
//   /enginefx.yaml  → applyEngineFxConfig — engine wiring + sounds
//   /landing.yaml   → applyLandingConfig  — landing-light defs
//   /lightfx.yaml   → applyLightFxConfig  — brightness + program paths +
//                                            program_selector ranges
static hubfx::config::ConfigStoreSlot<HubFxConfigSchema,    HubFxYamlPool>    kHubFx;
static hubfx::config::ConfigStoreSlot<AlertsConfigSchema,   AlertsYamlPool>   kAlerts;
static hubfx::config::ConfigStoreSlot<EngineFxConfigSchema, EngineFxYamlPool> kEngineFx;
static hubfx::config::ConfigStoreSlot<GunFxConfigSchema,    GunFxYamlPool>    kGunFx;
static hubfx::config::ConfigStoreSlot<LandingConfigSchema,  LandingYamlPool>  kLanding;
static hubfx::config::ConfigStoreSlot<GearControlConfigSchema, GearControlYamlPool> kGearCtrl;
static hubfx::config::ConfigStoreSlot<LightFxConfigSchema,  LightFxYamlPool>  kLightFx;

// Boot-complete marker — flipped true at the end of setup().  Two uses:
//   1) the input-driven drivers (RC program selector + landing/gear
//      activation) re-install on a config-reload ONLY after setup() did the
//      first install (the dispatcher must be fully up before they subscribe);
//   2) applyHubFxConfigCallback gates its post-boot role detach+reattach on
//      it — at boot the canonical applyPortRoles (line ~842) owns attachment;
//      only POST-boot reloads rebuild the live port/role layout.
static bool g_lightFxBooted = false;

// Re-assert ONLY the board-level master state (codec supply + the `features:`
// enable matrix that overrides each sub-file's local `enabled:`).  Does NOT
// touch the port/role layout — split out so the reload-complete hook can run
// it LAST without re-detaching roles the effect stores just re-claimed.
static void reassertHubFxFeatures(const HubFxConfig& cfg) {
    // Codec PVDD voltage — board-specific, written before audio bring-up.
    // /hubfx.yaml carries this because the codec is a board-level chip,
    // not an effect's concern.  `audio.begin()` later in setup() reads
    // `Codec::SUPPLY_VOLTAGE` via the CodecAdapter::probe() trait in
    // audio_codec.h.  CONFIG_RELOAD updates the variable but the codec
    // only re-reads on reboot — log a hint when the value changes.
    {
        sfx_audio::tas5825::Supply s;
        if (sfx_audio::tas5825::parseSupply(cfg.audio.codecSupply, s)) {
            if (s != Codec::SUPPLY_VOLTAGE) {
                SFX_LOG_INFO("[codec] supply %s → %s  (reboot to apply)",
                             sfx_audio::tas5825::supplyStr(Codec::SUPPLY_VOLTAGE),
                             sfx_audio::tas5825::supplyStr(s));
            }
            Codec::SUPPLY_VOLTAGE = s;
        } else if (cfg.audio.codecSupply[0]) {
            SFX_LOG_WARN("[codec] unknown audio.codec_supply='%s' — keeping %s",
                         cfg.audio.codecSupply,
                         sfx_audio::tas5825::supplyStr(Codec::SUPPLY_VOLTAGE));
        }
    }

    // NOTE (2026-07-26): the `features:` master-enable matrix was RETIRED.
    // Each effect's enable now lives ONLY in its own sub-config (`enginefx.yaml`
    // `enabled:` etc.) and is applied by that store's callback — hubfx.yaml is
    // pure port/input/audio mapping again.  Previously this re-applied
    // features.<effect> on every reload, which forced Studio to re-upload
    // hubfx.yaml on every effect toggle; that upload colliding with audio
    // playback wedged the flash (Rule 54).  No feature override here anymore.
    //
    // Refresh the runtime capability mask AFTER every effect store has applied
    // its own enable (this hook runs LAST in the reload) so INIT / Studio see
    // the true set of live effects.
    board.recomputeEnabledCapabilities();
}

// /hubfx.yaml store apply callback — fires on the initial load AND on every
// CONFIG_RELOAD.  Re-asserts the master features, then (POST-boot only)
// rebuilds the port/role layout from the fresh config so a Studio "Apply" of
// a changed `ports:` block takes effect WITHOUT a reboot.  The detach clears
// the live roles (releasing claims); applyPortRoles re-attaches per the new
// config; the effect stores' own reload callbacks then re-claim their ports.
// This runs FIRST in the reload loop (kHubFx is wired first), so the layout
// is rebuilt before any effect store re-claims.  Gated on g_lightFxBooted:
// at boot the canonical applyPortRoles at line ~842 owns attachment.
static void applyHubFxConfigCallback(const HubFxConfig& cfg) {
    reassertHubFxFeatures(cfg);
    if (g_lightFxBooted) {
        const uint8_t n =
            board.policy<HubFxTopologyService>().detachAllHubRoles();
        SFX_LOG_INFO("[config] reload: detached %u hub role(s), re-attaching "
                     "from /hubfx.yaml", (unsigned)n);
        hubfx::config::applyPortRoles<HubFxBoard, HubFxTopologyService>(
            board, cfg);
    }
}

static void applyAlertsConfigCallback(const AlertsConfig& cfg) {
    hubfx::config::applyAlertsConfig<HubFxBoard, AlertService>(board, cfg);
}

static void applyEngineFxConfigCallback(const EngineFxYamlConfig& cfg) {
    // Resolve `toggle.input` against /hubfx.yaml's named inputs[] —
    // kHubFx already loaded by the time this fires.
    hubfx::config::applyEngineFxConfig<HubFxBoard, EngineFxService>(
        board, cfg, kHubFx.data());
}

static void applyGunFxConfigCallback(const GunFxYamlConfig& cfg) {
    // Phase 2 of GunFX rollout + Rule 43: each gun's trigger.input /
    // rof.input / yaw.input / pitch.input is a NAME from /hubfx.yaml's
    // inputs[] block.  kHubFx.data() supplies the resolver table — the
    // apply translator turns names into resolved PortRef + channel
    // BEFORE handing the spec list to the service.
    //
    // Dispatcher subscriptions still happen in service.begin(), so a
    // reload updates the resolved table but doesn't re-subscribe —
    // a reboot rebinds cleanly.  Phase 4 polish: re-subscribe on reload.
    hubfx::config::applyGunFxConfig<HubFxBoard, GunFxService>(
        board, cfg, kHubFx.data());
}

// (g_lightFxBooted is defined near applyHubFxConfigCallback above — it gates
// both these input-driver re-installs and the reload role re-attach.)

// RC-channel auto-deploy driver — file-scope so the config-reload
// callback re-wires it on every Studio Apply (mirrors kLightFxSelector).
static hubfx::config::LandingActivationDriver kLandingActivation;

static void installLandingActivation(const LandingConfig& cfg) {
    kLandingActivation.install(
        &board.policy<InputDispatcherService>(),
        &board.policy<LandingLightService>(),
        cfg,
        kHubFx.data());
}

static void applyLandingConfigCallback(const LandingConfig& cfg) {
    hubfx::config::applyLandingConfig<HubFxBoard, LandingLightService>(board, cfg);
    // Re-wire RC-channel auto-deploy on every CONFIG_RELOAD (Studio Apply).
    // Skipped at boot — the setup block installs it once the dispatcher is up.
    if (g_lightFxBooted) installLandingActivation(cfg);
}

// RC-channel undercarriage up/down driver — file-scope so the config-reload
// callback re-wires it on every Studio Apply (mirrors kLandingActivation).
static hubfx::config::GearActivationDriver kGearActivation;

static void installGearActivation(const GearControlConfig& cfg) {
    kGearActivation.install(
        &board.policy<InputDispatcherService>(),
        &board.policy<GearControlService>(),
        cfg,
        kHubFx.data());
}

static void applyGearControlConfigCallback(const GearControlConfig& cfg) {
    hubfx::config::applyGearControlConfig<HubFxBoard, GearControlService>(board, cfg);
    // Re-wire the RC up/down channel on every CONFIG_RELOAD (Studio Apply).
    // Skipped at boot — the setup block installs it once the dispatcher is up.
    if (g_lightFxBooted) installGearActivation(cfg);
}

// RC program selector — file-scope so the config-reload callback can re-wire it
// (it was previously installed only once at boot, so a selector configured in
// Studio + Applied never took effect).  `g_lightFxBooted` (declared above with
// the landing-activation driver) gates the in-callback re-install to AFTER boot
// — at boot the dedicated setup block does the first install once the dispatcher
// is fully up.
static hubfx::config::LightFxProgramSelector kLightFxSelector;

// (Re)bind the selector against a freshly-applied lightfx config.  MUST run
// after applyLightFxConfig (which loads the programs the ranges resolve to).
// Handles enable/disable + input-rebind cleanly (install() drops the old
// subscription up-front).
static void installLightFxSelector(const LightFxYamlConfig& lf) {
    const auto* binding = lf.programSelector.enabled
        ? findInputByName(kHubFx.data(), lf.programSelector.input) : nullptr;
    if (lf.programSelector.enabled && !binding) {
        SFX_LOG_WARN("[lightfx-selector] input '%s' not in /hubfx.yaml inputs[] — selector dormant",
                     lf.programSelector.input);
    }
    kLightFxSelector.install(
        &board.policy<InputDispatcherService>(),
        &board.policy<LightFxEffectService>().controller(),
        lf,
        binding ? binding->port : hubfx::effects::PortRef{},
        (binding && binding->channelId > 0) ? (uint8_t)(binding->channelId - 1) : 0);
}

static void applyLightFxConfigCallback(const LightFxYamlConfig& cfg) {
    hubfx::config::applyLightFxConfig<HubFxBoard, LightFxEffectService>(
        board, cfg,
        // /hubfx.yaml store — v2 programs resolve channel names against
        // its LedAnimator port labels.  Loaded BEFORE LightFx by
        // construction (the sketch's loadOrFallback() ordering — kHubFx
        // first, kLightFx after).
        kHubFx.data(),
        // Program-file reader for `/lightfx/programs/<n>.yaml`.
        &storageReadFile<FlashModule>);
    // Re-wire the RC selector on every CONFIG_RELOAD (Studio Apply) so a newly
    // configured selector actually drives program switching.  Skipped at boot
    // (g_lightFxBooted == false) — the setup block installs it once the
    // InputDispatcher is fully initialised.
    if (g_lightFxBooted) installLightFxSelector(cfg);
}

#if SFX_INSTRUMENTATION
// ─── Memory-instrumentation helper (Phase 4 polish 2026-05-27) ──────
//
// Prints a single-line memory breakdown across the three pools that
// matter for the audio + SD subsystems:
//
//   PSRAM (OPI octal, 80 MHz, 8 MB on HubFX) — bulk audio storage
//     (decoded WAV float buffers, ring buffer).  ~95 % idle in
//     practice; not the bottleneck.
//   DRAM (internal SRAM, ~328 KB total) — task stacks, IDF baseline,
//     and (since 2026-05-27) the audio mixer's `_sdReadBuf` 32 KB
//     bounce buffer.  This is the scarce pool — log it so we catch
//     regressions early.
//   DMA-capable DRAM (subset of DRAM that can be a DMA target) —
//     this is what the SDMMC driver actually consumes when reading
//     into our internal-SRAM bounce.  Track separately because some
//     internal SRAM is reserved IRAM (instruction-only).
//
// Handle of our loop task (app_main spawns it).  Replaces Arduino-ESP32's
// global `loopTaskHandle` — the [stack] instrumentation reads its high-water
// mark.  Set in app_main; nullptr until then.
static TaskHandle_t loopTaskHandle = nullptr;

// Tagged `[mem]` so an operator can grep boot logs in one shot.
// Called from setup() and from a 30-second timer in loop().
static void logMemoryHeapCaps(const char* tag) {
    const size_t psramTotal     = esp_psram_get_size();
    const size_t psramFree      = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psramLargest   = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t dramTotal      = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t dramFree       = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t dramLargest    = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t dramDmaFree    = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t dramDmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    SFX_LOG_INFO(
        "[mem] %s | "
        "PSRAM %u/%u KB free (largest %u KB) | "
        "DRAM %u/%u KB free (largest %u KB) | "
        "DMA-cap %u KB free (largest %u KB)",
        tag,
        (unsigned)(psramFree / 1024),  (unsigned)(psramTotal / 1024),
        (unsigned)(psramLargest / 1024),
        (unsigned)(dramFree / 1024),   (unsigned)(dramTotal / 1024),
        (unsigned)(dramLargest / 1024),
        (unsigned)(dramDmaFree / 1024),
        (unsigned)(dramDmaLargest / 1024));

    // Phase 7 polish: surface task-stack high-water marks so we know
    // which stacks have headroom to reclaim.  uxTaskGetStackHighWaterMark
    // returns the SMALLEST recorded free-bytes value across the task's
    // lifetime — the closer to 0, the closer that task has come to
    // overflowing.  Configured stack - HWM = peak usage; anything with
    // > 2 KB headroom is a reclaim candidate (CONFIG_ARDUINO_LOOP_STACK,
    // _producerStackSize, _decoderStackSize, the Core 1 audio task).
    const auto reportStack = [](const char* name, TaskHandle_t h) {
        if (!h) return;
        const UBaseType_t hwm = uxTaskGetStackHighWaterMark(h);
        SFX_LOG_INFO("[stack] %-14s hwm=%u B (smaller = closer to overflow)",
                     name, (unsigned)hwm);
    };
    reportStack("loopTask",       loopTaskHandle);
    reportStack("audio-producer", Mixer::instance().producerHandle());
    reportStack("audio-decoder",  Mixer::instance().decoderHandle());
    reportStack("audio-consumer", Mixer::instance().consumerHandle());
}

// One-shot ground-truth dump of the static DRAM footprint — the
// `sizeof` of the single `board` global (every service policy is a
// member of it, so this IS the .bss cost of the whole policy pack) plus
// each config store's parsed-data struct (PSRAM-resident via
// ConfigStore, logged for completeness).  Replaces estimate-driven
// audits: grep `[footprint]` after a flash to see exactly what the
// policy pack and config schemas weigh on THIS build.  Boot-only.
static void logStaticFootprint() {
    SFX_LOG_INFO("[footprint] board global (.bss policy pack) = %u B (%u KB)",
                 (unsigned)sizeof(board), (unsigned)(sizeof(board) / 1024));
    SFX_LOG_INFO("[footprint] config data structs (PSRAM): "
                 "hubfx=%u alerts=%u enginefx=%u gunfx=%u landing=%u gear=%u lightfx=%u B",
                 (unsigned)sizeof(HubFxConfig),
                 (unsigned)sizeof(AlertsConfig),
                 (unsigned)sizeof(EngineFxYamlConfig),
                 (unsigned)sizeof(GunFxYamlConfig),
                 (unsigned)sizeof(LandingConfig),
                 (unsigned)sizeof(GearControlConfig),
                 (unsigned)sizeof(LightFxYamlConfig));
}
#endif // SFX_INSTRUMENTATION

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

    // ── Config — wire each ConfigStoreSlot against the policy.
    // Each `wire()` call: registers the store + facade with the policy,
    // binds the storage bridge, and hooks the apply callback to fire
    // on every successful load (initial + every CONFIG_RELOAD).
    {
        auto& cfgPolicy = board.policy<ConfigServicePolicy>();
        kHubFx   .wire(cfgPolicy, applyHubFxConfigCallback);
        kAlerts  .wire(cfgPolicy, applyAlertsConfigCallback);
        kEngineFx.wire(cfgPolicy, applyEngineFxConfigCallback);
        kGunFx   .wire(cfgPolicy, applyGunFxConfigCallback);
        kLanding .wire(cfgPolicy, applyLandingConfigCallback);
        kGearCtrl.wire(cfgPolicy, applyGearControlConfigCallback);
        kLightFx .wire(cfgPolicy, applyLightFxConfigCallback);

        // After ANY CONFIG_RELOAD, re-apply /hubfx.yaml's master `features:`
        // matrix LAST so it overrides each sub-file's local `enabled:` flag —
        // mirrors the boot-time re-apply below.  Without this, a reload of
        // (say) /gearcontrol.yaml would re-enable gears via its local
        // `enabled: true` even when features.gears is false.
        // Features-only re-assert (NOT the full callback) — the kHubFx store
        // callback already rebuilt the port/role layout FIRST in the reload
        // loop; re-running the full callback here (LAST, after the effect
        // stores re-claimed) would detach + re-attach again and drop those
        // claims.  This pass only re-stamps the master `features:` matrix.
        cfgPolicy.setReloadCompleteHook(
            [](void*) { reassertHubFxFeatures(kHubFx.data()); }, nullptr);
    }

    // Bring the wire-protocol UART up FIRST — board.begin() will read
    // bytes off it the moment its policy pack initializes.  ESP32-S3
    // default UART0 pins: TX=GPIO43, RX=GPIO44.
    //
    // 32 KB RX ring — sized to hold a FULL 16 KB stream-upload segment
    // burst with 2x margin.  The client blasts one whole 16 KB segment
    // before waiting for its FILE_UPLOAD_PROGRESS ACK, so up to 16 KB
    // can be in flight with no intra-segment backpressure; an 8 KB ring
    // overflowed (and stalled the upload) whenever processStream lagged
    // the 6 Mbps fill even briefly.  This regressed into view on
    // 2026-05-31 when the Arduino-Stream→sfx::Stream seam finally routed
    // the upload read through the bulk `NativeUartStream::readBytes`
    // override (before, `serial()` handed back an Arduino `Stream*` and
    // the slow per-byte `Stream::readBytes` polyfill was used — it
    // block-drained the ring continuously, masking the undersized
    // buffer).  DRAM for the extra 24 KB came free in the same Arduino-
    // removal work (Wire −21 KB, ESP32Servo −2 KB).  TX stays 8 KB (our
    // ACKs are tiny).
    wireUart.begin(UART_NUM_0, /*rx*/44, /*tx*/43,
                   sfx_core::BoardServerBase::BAUD_RATE,
                   /*rxBuf*/32768, /*txBuf*/8192);

    // Policy pack lifecycle — Serial, DiagLog, indicator pins, port
    // registry binding, every policy's begin().  Master role, no
    // upstream watchdog.  BoardOf<>::begin() takes (stream, version,
    // build, ledPin, errPin) because the prefix comes from `kName`.
    board.begin(wireUart, FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);
    board.setConnectionTimeoutEnabled(false);

    // AudioAssetCache: PSRAM-resident preloader for every WAV/MP3
    // played through the mixer.  MUST run before the config chain
    // below — each `applyXxxConfig` calls
    // `AudioAssetCache::setPreloadsForOwner(...)` so the loader can
    // begin pulling sound assets into PSRAM while the rest of setup
    // continues.  No DMA-cap allocations here (loader fread()s
    // directly into PSRAM via VFS-FAT's internal bouncer), so
    // pre-audio-init ordering is safe.  Loader runs on Core 0 prio
    // MAX-3.  Budget 6 MB; see audio_asset_cache.h.
    if (!AudioAssetCache::instance().begin()) {
        SFX_LOG_ERROR("[boot] AudioAssetCache::begin() failed — preload disabled");
    } else {
        SFX_LOG_INFO("[boot] AudioAssetCache::begin() OK");
    }

    // ── Boot config chain — load each store from its canonical path;
    // on miss, apply schema defaults so a bare board boots functional.
    // Order matters: /hubfx.yaml first (audio + features + ports +
    // inputs name map) → effect sub-files (engine / landing / lightfx
    // consume the name map) → re-apply /hubfx.yaml so its master
    // `features:` matrix overrides each sub-file's local `enabled:`.
    // Each apply also pushes its sound paths into the AssetCache via
    // `setPreloadsForOwner` — the loader (Core 0 prio MAX-3) starts
    // pulling them into PSRAM in the background while setup continues.
    kHubFx   .loadOrFallback();
    kAlerts  .loadOrFallback();
    kEngineFx.loadOrFallback();
    kGunFx   .loadOrFallback();   // was missing — on-device /gunfx.yaml never applied at boot, so GUN_FIRE_ONCE(id=0) hit UNKNOWN_ID (2026-05-23)
    kLanding .loadOrFallback();
    kGearCtrl.loadOrFallback();
    kLightFx .loadOrFallback();
    reassertHubFxFeatures(kHubFx.data());        // re-apply master enables
                                                 // (roles attached just below)

    // Port → role attachment is driven by `/hubfx.yaml`'s `ports:` block.
    // Empty table (no file / no block) falls back to the legacy default
    // of LedAnimator on every PWM port.  See applyPortRoles.
    hubfx::config::applyPortRoles<HubFxBoard, HubFxTopologyService>(
        board, kHubFx.data());

    // LightFx program selector — when /lightfx.yaml's `program_selector:`
    // block is populated, wire a Raw-µs TriggerInput against the named input
    // channel.  First install here (dispatcher fully up); the config-reload
    // callback re-wires it on every Studio Apply (see installLightFxSelector +
    // g_lightFxBooted above).
    installLightFxSelector(kLightFx.data());
    // Landing-light RC auto-deploy — for any /landing.yaml entry with an
    // `activation:` channel, subscribe a Boolean trigger that deploys above
    // the threshold and retracts below (fail-low on RC loss).  Re-wired on
    // every Studio Apply by applyLandingConfigCallback.
    installLandingActivation(kLanding.data());
    // Undercarriage RC up/down — when /gearcontrol.yaml's `input:` block
    // names a channel, subscribe a Boolean trigger that retracts above the
    // threshold and deploys below (failsafe = deploy on RC loss).  Re-wired
    // on every Studio Apply by applyGearControlConfigCallback.
    installGearActivation(kGearCtrl.data());
    g_lightFxBooted = true;

    // Mirror every INFO+ entry to the wire as TAG_ASYNC LOG_MESSAGE so
    // `subscribe` sees the device tail live.  Bump to DEBUG when chasing
    // LightFx ↔ LedAnimator path issues.
    DiagLog::instance().setWireMinLevel(DiagLevel::INFO);

    // Replay the silent initHardware() snapshot now that DiagLog has a
    // serial sink.
    board.logHardwareStatus();

    // I²C bus introspection — register the scan callback and mark every
    // chip physically wired on this PCB rev as "expected".
    board.enableI2CScan(hubI2cBus());
    board.addExpectedI2CDevice(I2cAddr::TAS5825P);
    board.addExpectedI2CDevice(I2cAddr::PCA9685);
    for (uint8_t k = 0; k < kInaCount; ++k) {
        board.addExpectedI2CDevice(kInaAddrs[k]);
    }
#if HUBFX_PCB_REV >= 2
    // 0x40 ACKs on rev B: it's the PCA9685's HARDWARE address (0x70 is the
    // all-call alias) colliding with the unused U43 INA226.  Expected so
    // scans don't flag it as a stranger — but NEVER driven (see kInaAddrs).
    board.addExpectedI2CDevice(0x40);
#endif

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

    // GearControl transit sounds — wire the service's audio hook into the
    // board mixer on the dedicated Gear channel.  Same playAsync semantics
    // as GunFx's onAudioCmd (preload-into-memory: transit loops are short
    // and wrap hot; SD seeks mid-loop stutter against the engine track).
    board.policy<GearControlService>().bindAudio(
        [](void*, const char* soundPath, uint8_t audioChannel,
           uint8_t outputMask, bool loop) {
            if (!soundPath) {
                Mixer::instance().stopAsync((int)audioChannel,
                                            AudioStopMode::Immediate);
                return;
            }
            if (!soundPath[0]) return;
            AudioPlaybackOptions opts;
            opts.volume         = 1.0f;
            opts.outputChannels = outputMask;
            if (loop) {
                opts.loop      = true;
                opts.loopCount = LOOP_INFINITE;
            }
            opts.preloadIntoMemory = true;
            Mixer::instance().playAsync(audioChannel, soundPath, opts);
        },
        nullptr,
        hubfx::effects::audio::HubFxLayout::Gear);

    // Same Rule-28 exclusivity for the Jeti IN_1 telemetry task: pause its
    // Core-0 UART servicing while an upload holds the pipeline, so it can't
    // steal cycles from the SD writer (the prior Jeti task's starvation mode).
    JetiEx::JetiExpander::instance().setUploadGate(
        [&board]() { return board.policy<StorageService>().isUploadActive(); });

    // Alert chimes — configuration is driven by `/alerts.yaml`
    // (severity → AlertSound + volume).  Initial config landed via
    // `applyAlertsConfig` above; CONFIG_RELOAD re-applies.  The mixer
    // channel is board-fixed at `HubFxLayout::Alert`.

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
    // Once an expander is Ready (accepting forwarded commands), (re)apply
    // the role attachments /hubfx.yaml declared for its GUID.  Covers both
    // boards that connect after boot and reconnects on a different USB
    // port — the config is GUID-addressed, not slot-addressed.
    exp.onReady([](const hubfx::expanders::ExpanderEntry& e) {
        auto& topo = board.policy<HubFxTopologyService>();
        // Declarative bringup (the standard expander pattern): push the board's
        // FULL /hubfx.yaml role set in ONE ROLE_BULK_ATTACH packet.  Atomic (no
        // racy per-port forwards) and updates the hub's cached roster, so
        // topo-roles / Studio reflect the applied config immediately on every
        // (re)connect.  An EMPTY block — a board with no configured roles (a
        // fresh / unconfigured expander) — is simply skipped; it comes up bare.
        uint8_t block[512];
        const size_t blockLen = hubfx::config::buildRoleBlockForGuid(
            kHubFx.data(), e.spec.guid, block, sizeof(block));
        const uint8_t roleCount = (blockLen >= 1) ? block[0] : 0;
        if (roleCount > 0) {
            if (topo.applyRoleConfig(e.spec.guid, block, blockLen)) {
                SFX_LOG_INFO("[hubfx-config] pushed %u role(s) to %s on connect",
                             (unsigned)roleCount, e.spec.guid);
            } else {
                SFX_LOG_WARN("[hubfx-config] role-config push to %s FAILED", e.spec.guid);
            }
        } else {
            SFX_LOG_INFO("[hubfx-config] %s: no configured roles — unconfigured bringup",
                         e.spec.guid);
        }
        // Audible "<board> ready" chime — fires HERE, i.e. only once the
        // expander is fully up (IDENTIFY decoded → ports enumerated → roles
        // enumerated → Handshake::Ready) AND its /hubfx.yaml roles have just
        // been (re)attached above.  GearControl gets its own sound.
        if (e.kind == hubfx::expanders::ExpanderKind::GearControl) {
            // QUEUE behind whatever's on the alert channel (e.g. the boot
            // announcement) so the chime doesn't clip it — drained by
            // AlertService::update() once the channel frees.
            board.policy<AlertService>().playSoundQueued(
                hubfx::effects::alerts::AlertSound::GearControlInitialized);
        }
    });
    exp.onDisconnect([](const hubfx::expanders::ExpanderEntry& e) {
        SFX_LOG_INFO("[Expander] unmount %s addr=%u guid=%s",
                     hubfx::expanders::ExpanderKind::getName(e.kind),
                     e.usbAddr, e.spec.guid);
        // Release any connection-loss Link slots owned by the departing board so
        // its input ports don't permanently squat the (few) kMaxLinks slots —
        // the GUID is stable per silicon, so this is the right reclaim trigger.
        if (e.spec.valid) {
            board.policy<InputDispatcherService>().forgetLinksForGuid(e.spec.guid);
        }
    });

    // Auto-begun policies — surface their initial state so the boot
    // log confirms they came up clean.
    SFX_LOG_INFO("[InputDispatcher] up — %u/%u bindings",
                 (unsigned)board.policy<InputDispatcherService>().numBindings(),
                 (unsigned)InputDispatcherService::kMaxBindings);
    SFX_LOG_INFO("[LandingLight] up — %u/%u lights configured",
                 (unsigned)board.policy<LandingLightService>().count(),
                 (unsigned)hubfx::effects::landing::kMaxLandingLights);
    SFX_LOG_INFO("[LightFx] up — %u program(s) loaded from /lightfx/programs/",
                 (unsigned)board.policy<LightFxEffectService>().controller().numPrograms());

    // Boot announcement — audio is live (audio.begin() above ran through
    // codec PLAY).  Plays /sounds/sys/init.mp3 on the Alert mixer channel
    // via the named-cue API.  Best-effort: no-op when features.alerts is
    // false or the MP3 is missing on SD.
    board.policy<AlertService>().playSound(
        hubfx::effects::alerts::AlertSound::Init);

    // Confirm the loop/LED-tick core: with -DARDUINO_RUNNING_CORE=0 this
    // must print 0 (off Core 1's audio tasks).  If it prints 1, the
    // framework ignored the flag → fall back to a dedicated Core-0 LED
    // task + I²C mutex.
    SFX_LOG_INFO("[boot] loopTask running on Core %d (audio is on Core 1)",
                 (int)xPortGetCoreID());

    // Reset-reason post-mortem (console is NONE — this rides the DiagLog wire).
    // The tell for the live-USB-replug restart: PANIC → a firmware crash wrote a
    // coredump (decode it); BROWNOUT → 3V3/VBUS sagged on the replug inrush (an
    // ELECTRICAL fault, NOT firmware — no coredump); INT_WDT/TASK_WDT → a task
    // hung the scheduler.  Logged at WARN so it surfaces even at INFO wire gate.
    {
        const esp_reset_reason_t rr = esp_reset_reason();
        const char* name = "OTHER";
        switch (rr) {
            case ESP_RST_POWERON:  name = "POWERON (normal cold boot)";       break;
            case ESP_RST_SW:       name = "SW (esp_restart)";                 break;
            case ESP_RST_PANIC:    name = "PANIC (firmware crash → coredump)"; break;
            case ESP_RST_INT_WDT:  name = "INT_WDT (interrupt watchdog)";     break;
            case ESP_RST_TASK_WDT: name = "TASK_WDT (task watchdog)";         break;
            case ESP_RST_WDT:      name = "WDT (other watchdog)";             break;
            case ESP_RST_BROWNOUT: name = "BROWNOUT (3V3/VBUS sag — ELECTRICAL, no coredump)"; break;
            case ESP_RST_DEEPSLEEP:name = "DEEPSLEEP";                        break;
            case ESP_RST_USB:      name = "USB (peripheral reset)";           break;
            default: break;
        }
        if (rr == ESP_RST_BROWNOUT || rr == ESP_RST_PANIC ||
            rr == ESP_RST_INT_WDT  || rr == ESP_RST_TASK_WDT) {
            SFX_LOG_WARN("[boot] *** last reset: %s (code=%d) ***", name, (int)rr);
        } else {
            SFX_LOG_INFO("[boot] last reset: %s (code=%d)", name, (int)rr);
        }
    }

    SFX_LOG_INFO("HubFX v%s build %u (PCB rev %d) — 8 PWM / 2 input / 10 servo-out + storage + audio + alerts + USB host + topology + input + landing + lightfx + gear + engine + gun",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER, (int)HUBFX_PCB_REV);

    // Boot baseline (Phase 4 polish 2026-05-27).  Capture free SRAM /
    // PSRAM / DMA-cap pools AFTER every policy + service has done its
    // allocations so we see the steady-state headroom.  Periodic
    // re-emission from loop() (every 30 s) flags drift.  + ground-truth
    // static-footprint dump so the .bss policy-pack cost is measured,
    // not estimated.
#if SFX_INSTRUMENTATION
    logMemoryHeapCaps("boot baseline");
    logStaticFootprint();

    // Asset-cache preload snapshot (rule-of-least-surprise: the
    // operator sees what's been queued for PSRAM residency before
    // play() requests start arriving).  Per-path lines may show
    // status=loading at this point — the loader keeps working in the
    // background and finished-load events log themselves.
    AudioAssetCache::instance().logPreloadStatus();
#endif
}

void loop() {
    // Rule 40 (instructions/22 §0.5): the effect clock is latched ONCE per
    // pass by the framework inside `board.process()`, BEFORE any policy/effect
    // ticks — so every effect reading `EffectClock::instance().nowMs()` this
    // pass sees the same value (ROF scheduler / fan puff / motion profile /
    // heater bang-bang / EngineFx state machine stay in lockstep).  No manual
    // latch here: the upload-exclusive path below returns without ticking
    // effects, so a clock that only advances when `process()` runs is correct.

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
            // Sync upload: drain ONLY the bus (read + dispatch the upload
            // chunks).  pumpBus() skips the policy tick, so roles/effects don't
            // run and no input/verbose broadcast is emitted while the transfer
            // is exclusive — matches the "drain only the storage server" intent
            // and stops broadcasts competing with the upload (Rule 28).
            board.pumpBus();
        }
        storage.checkUploadTimeout();
        // Yield to IDLE0 each iteration so the Task Watchdog Timer
        // doesn't panic.  The AudioAssetCache loader (Core 0, prio
        // configMAX_PRIORITIES-3 = 22) blocks on `sd.lock()` for the
        // duration of every upload; FreeRTOS's mutex priority
        // inheritance then boosts this loop task (prio 1) to 22.
        // Without the yield, IDLE0 (prio 0) is preempted indefinitely
        // and TWDT (5 s, CONFIG_ESP_TASK_WDT_TIMEOUT_S) reboots the
        // device — manifested as a hard reset at exactly the first
        // STREAM_SEGMENT_SIZE boundary into a stream upload, found and
        // verified on build 486 / 489 (2026-05-28).  vTaskDelay(1) at
        // FREERTOS_HZ=1000 = 1 ms per pass, well inside the TWDT
        // budget; throughput cost is sub-1 % vs the old 525 KB/s
        // ceiling.
        vTaskDelay(pdMS_TO_TICKS(1));
        return;
    }

    board.process();
    storage.checkUploadTimeout();
    board.pollSense();

    // Jeti EX expander telemetry — HubFX-own sensors on the local device
    // (joins the expander's built-in Uptime).  Version registers once; the
    // rev B rail monitors (probe pollSense: cached V/I @10 Hz + coulomb-
    // counted mAh) feed Batt/Exp sensors at kRailTelemetryMs.  Sensors
    // register ONLY for monitors that came up (canonical-ID gate) — a stock
    // un-reworked board simply shows no Exp rows.  Values reach both the
    // radio and Studio's Telemetry panel via the TelemetryHub.
    // (The undervoltage AlertSound::BatteryLow detector is still parked —
    // telemetry-only for now.)
    {
        auto& jexp = JetiEx::JetiExpander::instance();
        if (jexp.running()) {
            static bool jetiSensorsReg = false;
            if (!jetiSensorsReg) {
                jetiSensorsReg = true;
                int maj = 0, mnr = 0;
                sscanf(FIRMWARE_VERSION, "%d.%d", &maj, &mnr);   // "2.16.0-hubfx"
                jexp.setLocalSensor(3, "Version", "", 2,
                                    (int32_t)(maj * 100 + mnr), SFX_MILLIS());   // 216 → "2.16"
            }
#if HUBFX_PCB_REV >= 2
            // Rail telemetry — sensor ids 4..8 on the HubFx local device
            // (1 = Uptime, 3 = Version).  kInaAddrs[0] = battery,
            // kInaAddrs[1] = expander rail (see the pin/address map).
            static constexpr uint32_t kRailTelemetryMs = 500;
            static bool     railSensorsReg = false;
            static uint32_t railLastMs     = 0;
            const uint32_t  nowMs          = SFX_MILLIS();
            if (nowMs - railLastMs >= kRailTelemetryMs) {
                railLastMs = nowMs;
                const bool battUp = board.probe.inaUp(0);
                const bool expUp  = board.probe.inaUp(1);
                if (!railSensorsReg && (battUp || expUp)) {
                    railSensorsReg = true;
                    if (battUp) {
                        jexp.setLocalSensor(4, "Batt U",   "V",   2, 0, nowMs);
                        jexp.setLocalSensor(5, "Batt I",   "A",   2, 0, nowMs);
                        jexp.setLocalSensor(6, "Batt used","mAh", 0, 0, nowMs);
                    }
                    if (expUp) {
                        jexp.setLocalSensor(7, "Exp I",    "A",   2, 0, nowMs);
                        jexp.setLocalSensor(8, "Exp used", "mAh", 0, 0, nowMs);
                    }
                }
                if (battUp) {
                    // V·100 (12.60 V → 1260) and A·100 (3.45 A → 345) at 2
                    // decimals fit Int14 (±81.91); mAh integer in Int22.
                    jexp.setLocalValue(4, (int32_t)(board.probe.railVoltage_mV(0) / 10.0f), nowMs);
                    jexp.setLocalValue(5, (int32_t)(board.probe.railCurrent_mA(0) / 10.0f), nowMs);
                    jexp.setLocalValue(6, (int32_t)board.probe.railUsed_mAh(0), nowMs);
                }
                if (expUp) {
                    jexp.setLocalValue(7, (int32_t)(board.probe.railCurrent_mA(1) / 10.0f), nowMs);
                    jexp.setLocalValue(8, (int32_t)board.probe.railUsed_mAh(1), nowMs);
                }
            }
#endif
        }
    }

#if SFX_INSTRUMENTATION
    // Live-state telemetry — when /hubfx.yaml's `telemetry.inputs:` or
    // `telemetry.outputs:` is on, emit one aggregated snapshot line per
    // `interval_ms`.  No-op when both flags are off.
    static uint32_t telemetryLastMs = 0;
    hubfx::config::tickTelemetry(
        board,
        board.policy<InputDispatcherService>(),
        kHubFx.data(),
        telemetryLastMs);

    // Periodic memory snapshot (Phase 4 polish 2026-05-27).  Once per
    // 30 s we re-emit the heap-caps breakdown; an operator can grep
    // for `[mem]` and watch for SRAM creep over time (e.g. a slow
    // leak in a service policy or audio source).  Cheap — three
    // heap_caps queries.
    static uint32_t lastMemLogMs = 0;
    const uint32_t  nowMs = SFX_MILLIS();
    if (nowMs - lastMemLogMs >= 30000UL) {
        lastMemLogMs = nowMs;
        logMemoryHeapCaps("periodic");
    }
#endif // SFX_INSTRUMENTATION

    vTaskDelay(pdMS_TO_TICKS(1));
}

// ============================================================================
//  app_main — ESP-IDF native entry (replaces the Arduino setup/loop bridge).
// ============================================================================
//
//  framework = espidf (no Arduino).  IDF calls app_main on the "main" task;
//  we spawn a dedicated loop task that mirrors what Arduino-ESP32's loopTask
//  used to do: setup() once, then loop() forever, pinned to Core 0
//  (ARDUINO_RUNNING_CORE=0 → Core 1 belongs to the audio producer/consumer)
//  with the 16 KB stack the deep config-reload path needs (was
//  CONFIG_ARDUINO_LOOP_STACK_SIZE).  loop() already ends with vTaskDelay(1),
//  so IDLE0 / TWDT stay fed.
extern "C" void app_main(void) {
    xTaskCreatePinnedToCore(
        [](void*) {
            setup();
            for (;;) loop();
        },
        "loopTask", 16384, nullptr, 1, &loopTaskHandle, /*core=*/0);
}
