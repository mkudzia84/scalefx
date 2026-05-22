/*
 * GunFX Pico — generic-expander board (RP2040).
 *
 *   A THIN port + role expander.  All trigger timing, muzzle-flash
 *   pulse/fade, recoil jerk, and smoke heater/fan sequencing live on the
 *   HubFX master (`GunFxServicePolicy`) — this board only exposes its
 *   physical ports and lets the hub attach roles + drive them over USB
 *   CDC.  (Architecture: instructions/15-GENERIC-EXPANDER-REFACTOR.md +
 *   17-SYSTEM-SERVICES.md.  Replaces ~400 lines of on-board muzzle /
 *   smoke / trigger-capture code retired with this rebuild.)
 *
 *   Ports exposed (idx order is the hub's PortRef addressing — keep in
 *   sync with /hubfx.yaml's gunfx wiring):
 *     - 3 × PWM   pwm[0]=GP25 muzzle flash → LedAnimator role
 *                 pwm[1]=GP16 smoke fan    → DcMotor role
 *                 pwm[2]=GP17 smoke heater → Heater role
 *     - 3 × Servo GP1/2/3 (recoil headers) → ServoActuator role
 *
 *   NO input port — the legacy GP0 RC PWM trigger capture is retired; the
 *   hub reads the gun-trigger channel on its own input and drives the
 *   muzzle / recoil / smoke outputs over the wire (matches GearControl).
 *   Port direction is fixed at declaration (Rule 31); GP0 is unused.
 *
 *   Battery: an ADC + resistor-divider sensor on GP29 (÷6 divider) wired
 *   through `BatteryServicePolicy` (ExtraPolicy on BoardOf<>) — advertises
 *   CoreCapability::BATTERY, handles BATTERY_CONFIG (chemistry / cell
 *   count from the hub), and rides the periodic STATUS broadcast with a
 *   battery section (voltage / cells / low+critical flags).
 *
 *   The 2 board indicator LEDs (blue = connection, yellow = error) are
 *   driven by the auto-prepended IndicatorServicePolicy via board.begin().
 *
 *   Roles are NOT attached here — `RoleServicePolicy` (auto via BoardOf<>)
 *   accepts ROLE_ATTACH from the hub and emplaces the role variants.
 *
 *   PINOUT (RP2040 GPIO):
 *     GP1/2/3 : recoil servo headers (SRV1..SRV3)
 *     GP16    : smoke fan motor (PWM)
 *     GP17    : smoke heater relay (PWM/on-off)
 *     GP25    : muzzle flash LED (PWM)
 *     GP13/14 : indicator LEDs (connection / error)
 *     GP29    : battery voltage sense (ADC, ÷6 divider)
 */

#include <Arduino.h>

#include <platform/sfx_platform.h>
#include <serial/diag_log.h>
#include <serial/core/core.h>            // StatusDataCallback layout
#include <server/board_of.h>

#include <ports/pwm_port.h>
#include <ports/servo_port.h>
#include <power/battery_monitor.h>       // AdcDividerBatteryT
#include <power/battery_server.h>        // BatteryServicePolicy

#define FIRMWARE_VERSION "1.1.0"
#define BUILD_NUMBER     4

// ════════════════════════════════════════════════════════════════════════
//  Board pin map (RP2040 GPIO)
// ════════════════════════════════════════════════════════════════════════

namespace Gpio {
    constexpr int LED_CONNECTION = 13;   // blue indicator
    constexpr int LED_ERROR      = 14;   // yellow indicator
    constexpr int VSENSE         = 29;   // battery ADC (÷6 divider)
}

// Battery sensor type — ADC + ÷6 resistor divider (≈50k/10k), encoded in
// milli-units.  Aliased so both the board member and the policy pack agree.
using GunFxBattery        = AdcDividerBatteryT<6000>;
using GunFxBatteryService = BatteryServicePolicy<GunFxBattery>;

// ── Board class ──────────────────────────────────────────────────────
//
// BoardOf<TBoard, ExtraPolicies...> auto-prepends BoardServicePolicy +
// IndicatorServicePolicy + PortServicePolicy + RoleServicePolicy, then
// appends the user policies — here just the battery monitor.

class GunFxBoard : public sfx_core::BoardOf<GunFxBoard, GunFxBatteryService> {
public:
    // ── PWM OUTPUT ports — muzzle flash + smoke fan + smoke heater ───
    // idx order is the hub's addressing: 0=muzzle, 1=fan, 2=heater.
    sfx_peripherals::NativePwmPort pwm[3] = {
        sfx_peripherals::NativePwmPort{25},   // pwm[0] muzzle flash
        sfx_peripherals::NativePwmPort{16},   // pwm[1] smoke fan
        sfx_peripherals::NativePwmPort{17},   // pwm[2] smoke heater
    };

    // ── Servo OUTPUT ports — 3 recoil headers (GP1/2/3) ──────────────
    sfx_peripherals::MicroservoPort servoOut[3] = {
        {1}, {2}, {3},
    };

    // ── Battery sensor (ADC + divider on GP29) ───────────────────────
    GunFxBattery battery;

    // ── Port declarations (compile-time, consumed by BoardOf<>) ──────

    static constexpr auto kPwmPorts = sfx_core::ports::list(
        sfx_core::ports::pwm_array<&GunFxBoard::pwm, 3>());

    static constexpr auto kServoPorts = sfx_core::ports::list(
        sfx_core::ports::servo_array<&GunFxBoard::servoOut, 3>());

    static constexpr const char* kName = "GunFx";
};

GunFxBoard board;

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
    // Battery ADC bring-up + bind BEFORE board.begin() so the policy's
    // begin() sees a bound sensor (and the BATTERY capability is live).
    board.battery.begin(Gpio::VSENSE);             // default LiPo, auto cell-detect
    board.policy<GunFxBatteryService>().bindBattery(board.battery);

    // Policy pack lifecycle — Serial / DiagLog / indicator pins / port
    // registry binding / every policy's begin() / IDENTIFY capabilities.
    board.begin(FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);

    board.core().onStatusData(appendBatteryStatus);

    SFX_LOG_INFO("GunFx expander v%s build %u — 3 pwm / 3 servo + ADC battery (GP%d)",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER, Gpio::VSENSE);
}

void loop() {
    board.process();
    board.battery.update();      // ADC read + state machine (throttled internally)
    busy_wait_ms(1);
}
