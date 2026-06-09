/*
 * LightFX Pico — generic-expander board (RP2040).
 *
 *   A THIN port + role expander.  All LED program sequencing, landing-
 *   light state machines, and RC program-select logic live on the HubFX
 *   master (`LightFxEffectServicePolicy` / `LandingLightServicePolicy`) —
 *   this board only exposes its physical ports and lets the hub attach
 *   roles + drive them over USB CDC.  (Architecture:
 *   instructions/15-GENERIC-EXPANDER-REFACTOR.md + 17-SYSTEM-SERVICES.md.)
 *
 *   Resurrected 2026-06-09 against the CURRENT BoardServer API — the prior
 *   standalone build was retired 2026-06-06 when the LightFx effects moved
 *   to the hub; this rebuild restores the physical expander board.  It now
 *   follows the gearcontrol-era shape: an explicit USB-CDC wire stream
 *   (Rule 55) + a `PortCapacity<>` that sizes the registry exactly.
 *
 *   Ports exposed:
 *     - 8 × PWM   (GP0..7, active-high LED channels) → LedAnimator role
 *                  (the hub plays LightFx programs through them; the
 *                   landing-light bulb is just one of these channels,
 *                   driven by the hub's LandingLightService).
 *     - 3 × Servo (GP8/9/10 headers)                 → ServoActuator role
 *                  (e.g. landing-light deploy servos, driven by the hub).
 *
 *   NO input port — RC reading is centralized on the hub, which drives
 *   program selection over the wire.  GP8 is a plain servo output
 *   (Rule 31 — port direction is fixed at declaration).
 *
 *   Battery: an ADC + resistor-divider sensor on GP29 (÷6.18 divider)
 *   wired through `BatteryServicePolicy` (ExtraPolicy on BoardOf<>) — it
 *   advertises CoreCapability::BATTERY, handles BATTERY_CONFIG (chemistry
 *   / cell count from the hub), and rides the periodic STATUS broadcast
 *   with a battery section (voltage / cells / low+critical flags).
 *
 *   The 2 board indicator LEDs (blue = connection, yellow = error) are
 *   driven by the auto-prepended IndicatorServicePolicy via board.begin().
 *
 *   Roles are NOT attached here — `RoleServicePolicy` (auto via BoardOf<>)
 *   accepts ROLE_ATTACH from the hub and emplaces the LedAnimator /
 *   ServoActuator variants at runtime.
 *
 *   PINOUT (RP2040 GPIO):
 *     GP0..7   : LED PWM channels (CH1..CH8)
 *     GP8/9/10 : servo headers (SRV1..SRV3)
 *     GP24/25  : indicator LEDs (connection / error)
 *     GP29     : battery voltage sense (ADC, ÷6.18 divider)
 */

#include <Arduino.h>
#include <type_traits>

#include <platform/sfx_platform.h>
#include <platform/pico_serial_stream.h>   // USB-CDC Serial → sfx::Stream wire adapter
#include <serial/diag_log.h>
#include <serial/core/core.h>              // StatusDataCallback layout
#include <server/board_of.h>

#include <ports/pwm_port.h>
#include <ports/servo_port.h>
#include <power/battery_monitor.h>         // AdcDividerBatteryT
#include <power/battery_server.h>          // BatteryServicePolicy

#define FIRMWARE_VERSION "1.0.0"
#define BUILD_NUMBER     1

// ════════════════════════════════════════════════════════════════════════
//  Board pin map (RP2040 GPIO)
// ════════════════════════════════════════════════════════════════════════

namespace Gpio {
    constexpr int LED_CONNECTION = 24;   // blue indicator
    constexpr int LED_ERROR      = 25;   // yellow indicator
    constexpr int VSENSE         = 29;   // battery ADC (÷6.18 divider)
}

/// USB-CDC wire type — `Serial`'s concrete class wrapped as sfx::Stream.
using LightWireStream       = sfx::PicoSerialStream<std::remove_reference_t<decltype(Serial)>>;

// Battery sensor type — ADC + ÷6.18 resistor divider (≈41k/8k), encoded in
// milli-units.  Aliased so both the board member and the policy pack agree.
using LightFxBattery        = AdcDividerBatteryT<6180>;
using LightFxBatteryService = BatteryServicePolicy<LightFxBattery>;

// ── Board class ──────────────────────────────────────────────────────
//
// BoardOf<TBoard, TStream, Caps, ExtraPolicies...> auto-prepends
// BoardServicePolicy + IndicatorServicePolicy + PortServicePolicy +
// RoleServicePolicy, then appends the user policies — here just the
// battery monitor.  Caps sizes the port registry EXACTLY to this board's
// hardware (3 servo / 8 pwm / 0 hbridge / 0 input).

class LightFxBoard : public sfx_core::BoardOf<LightFxBoard,
                                              LightWireStream,
                                              sfx_core::PortCapacity</*servo*/3, /*pwm*/8,
                                                                      /*hbridge*/0, /*input*/0>,
                                              LightFxBatteryService> {
public:
    // ── LED OUTPUT ports — 8 active-high PWM channels (GP0..7) ───────
    sfx_peripherals::NativePwmPort led[8] = {
        sfx_peripherals::NativePwmPort{0}, sfx_peripherals::NativePwmPort{1},
        sfx_peripherals::NativePwmPort{2}, sfx_peripherals::NativePwmPort{3},
        sfx_peripherals::NativePwmPort{4}, sfx_peripherals::NativePwmPort{5},
        sfx_peripherals::NativePwmPort{6}, sfx_peripherals::NativePwmPort{7},
    };

    // ── Servo OUTPUT ports — 3 headers (GP8/9/10) ────────────────────
    sfx_peripherals::MicroservoPort servoOut[3] = {
        {8}, {9}, {10},
    };

    // ── Battery sensor (ADC + divider on GP29) ───────────────────────
    LightFxBattery battery;

    // ── Port declarations (compile-time, consumed by BoardOf<>) ──────

    static constexpr auto kPwmPorts = sfx_core::ports::list(
        sfx_core::ports::pwm_array<&LightFxBoard::led, 8>());

    static constexpr auto kServoPorts = sfx_core::ports::list(
        sfx_core::ports::servo_array<&LightFxBoard::servoOut, 3>());

    static constexpr const char* kName = "LightFx";
};

LightFxBoard    board;
LightWireStream wireStream{Serial};   // USB-CDC wire to the hub (sfx::Stream adapter)

// Append a battery section to the periodic STATUS broadcast so the hub can
// read this expander's pack voltage.  Layout (Rule 11 append-only):
//   [present:u8][voltage_mV:u16LE][cellCount:u8][pct:u8][flags:u8]
//   flags: bit0 = low, bit1 = critical
static size_t appendBatteryStatus(uint8_t* buf, size_t maxLen) {
    if (maxLen < 6) return 0;
    const auto& b = board.battery;
    const uint16_t mv = b.voltage_mV();
    uint8_t flags = 0;
    if (b.isLow())      flags |= 0x01;
    if (b.isCritical()) flags |= 0x02;
    buf[0] = b.isPresent() ? 1 : 0;
    buf[1] = (uint8_t)(mv & 0xFF);
    buf[2] = (uint8_t)(mv >> 8);
    buf[3] = b.cellCount();
    buf[4] = b.percentage();
    buf[5] = flags;
    return 6;
}

void setup() {
    // Bring the USB-CDC wire up first — baud is ignored over USB, but this
    // brings the endpoint up before the framer starts reading it.
    Serial.begin(115200);

    // Battery ADC bring-up + bind BEFORE board.begin() so the policy's
    // begin() sees a bound sensor (and the BATTERY capability is live).
    board.battery.begin(Gpio::VSENSE);             // default LiPo, auto cell-detect
    board.policy<LightFxBatteryService>().bindBattery(board.battery);

    // Policy pack lifecycle — wire / DiagLog / indicator pins / port
    // registry binding / every policy's begin() / IDENTIFY capabilities.
    board.begin(wireStream, FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);

    board.core().onStatusData(appendBatteryStatus);

    // Stream INFO+ log lines onto the wire so the CLI / Studio console see
    // this expander's boot + role-attach trace.
    DiagLog::instance().setWireMinLevel(DiagLevel::INFO);

    SFX_LOG_INFO("LightFx expander v%s build %u — 8 pwm / 3 servo + ADC battery (GP%d)",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER, Gpio::VSENSE);
}

void loop() {
    board.process();
    board.battery.update();      // ADC read + state machine (throttled internally)
    busy_wait_ms(1);
}
