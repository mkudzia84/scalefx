/*
 * GearControl Pico — generic-expander board (RP2040).
 *
 *   A THIN port + role expander.  All gear / door sequencing, stall
 *   detection, and timeout logic live on the HubFX master's
 *   `GearControlServicePolicy` — this board only exposes its physical
 *   ports and lets the hub attach roles + drive them over USB CDC.
 *   (Architecture: instructions/15-GENERIC-EXPANDER-REFACTOR.md +
 *   17-SYSTEM-SERVICES.md.  Replaces ~4 000 lines of on-board
 *   sequencer code that was retired with this rebuild.)
 *
 *   Ports exposed (cross-referenced against instructions/schematics/
 *   gearcontrol.tel — see PINOUT below):
 *     - 7 × Servo    (door + yaw headers)        → ServoActuator role
 *     - 3 × HBridge  (gear motors, fwd/rev)       → BiDcMotor role,
 *                     each with an INA226 current sensor for stall
 *                     detection (MOTOR_STALL_EVENT → hub gear FSM)
 *
 *   The 6 small per-motor status LEDs are NOT exposed as ports.  They
 *   are plain GPIO indicators driven LOCALLY (`GearStatusLeds`) from the
 *   H-bridge drive state — the hub does not command them:
 *     forward (signed duty > 0) → CW LED blinks
 *     reverse (signed duty < 0) → CCW LED blinks
 *     idle (duty == 0)          → last-direction LED solid (position hint)
 *     over-current fault        → both LEDs blink fast
 *
 *   NO input port — the legacy RC PWM deploy/retract input on GP0 is
 *   retired; the hub commands gear state over the wire.
 *
 *   The 2 board indicator LEDs (blue = connection, yellow = error) are
 *   driven by the auto-prepended IndicatorServicePolicy via board.begin().
 *
 *   Roles are NOT attached here — `RoleServicePolicy` (auto via
 *   BoardOf<>) accepts ROLE_ATTACH from the hub and emplaces the
 *   ServoActuator / BiDcMotor variants at runtime.
 *
 *   PINOUT (RP2040 GPIO — authoritative; netlist confirms topology):
 *     GP1,2,3,6,7,8,9 : servo headers (SERVO1-7)
 *     GP4 / GP5       : I²C SDA / SCL (3× INA226)
 *     GP13 / GP14     : indicator LEDs (BLUE_LED / YELLOW_LED)
 *     GP15/16,17/18,19/20 : motor H-bridge fwd/rev (MOTOR1-3 / MOTORREV1-3)
 *     GP21..26        : per-motor status LEDs (STATUS1-6, CW/CCW pairs)
 *     GP29            : battery voltage sense (ADC) — reserved, not yet
 *                       exposed (no ADC battery sensor wired this pass)
 */

#include <Arduino.h>
#include <type_traits>
#include <variant>

#include <platform/sfx_platform.h>
#include <platform/pico_serial_stream.h>   // USB-CDC Serial → sfx::Stream wire adapter
#include <serial/diag_log.h>
#include <server/board_of.h>

#include <ports/servo_port.h>
#include <ports/pwm_port.h>
#include <ports/hbridge_port.h>
#include <i2c/sfx_i2c.h>                    // sfx_peripherals::SfxI2cBus (= PicoI2cBus)
#include <power/ina226.h>
#include <power/ina226_sensor.h>

#define FIRMWARE_VERSION "1.0.0"
#define BUILD_NUMBER     30

// ════════════════════════════════════════════════════════════════════════
//  Board pin / address map (cross-ref: instructions/schematics/gearcontrol.tel)
// ════════════════════════════════════════════════════════════════════════

namespace Gpio {
    // I²C bus — 3× INA226 motor-current monitors.
    constexpr int I2C_SDA        =  4;
    constexpr int I2C_SCL        =  5;

    // Indicator LEDs (IndicatorServicePolicy).
    constexpr int LED_CONNECTION = 13;   // BLUE_LED
    constexpr int LED_ERROR      = 14;   // YELLOW_LED

    // Per-motor status LEDs (STATUS1-6) — small GPIO indicators, driven
    // locally by GearStatusLeds.  CW = forward/deploy, CCW = reverse/retract.
    constexpr uint8_t STATUS_CW [3] = { 21, 23, 25 };   // STATUS1/3/5
    constexpr uint8_t STATUS_CCW[3] = { 22, 24, 26 };   // STATUS2/4/6
}

namespace I2cAddr {
    // Strap-selected INA226 addresses, one per motor (from legacy map):
    //   motor 0 (nose)       GND/GND → 0x40
    //   motor 1 (left main)  VS/GND  → 0x44
    //   motor 2 (right main) GND/VS  → 0x41
    constexpr uint8_t INA226[3] = { 0x40, 0x44, 0x41 };
}

namespace Sense {
    constexpr float SHUNT_OHMS = 0.005f;   // 5 mΩ shunt (≈16 A INA226 ceiling)
    constexpr float MAX_AMPS   = 10.0f;    // 10 A expected peak

    // Hard over-current limit for the LOCAL status-LED fault blink.  Set
    // well above the normal endpoint stall current (which is the gear's
    // expected mechanical signal, NOT a fault) so only a genuine short /
    // jam lights the fault pattern.
    constexpr int16_t FAULT_mA = 9000;
}

// ── Status-LED driver — hardwired direction indicators ────────────────
//
// Drives the 6 small per-motor status LEDs purely from the LOCAL H-bridge
// drive state — no hub involvement.  Each motor has a CW (forward/deploy)
// and CCW (reverse/retract) LED:
//   forward (duty > 0)  → CW LED blinks @250 ms
//   reverse (duty < 0)  → CCW LED blinks @250 ms
//   idle (duty == 0)    → last-direction LED solid (position hint)
//   over-current fault  → both LEDs blink fast @100 ms
class GearStatusLeds {
public:
    void begin(const uint8_t cw[3], const uint8_t ccw[3]) {
        for (uint8_t m = 0; m < 3; ++m) {
            _cw[m]  = cw[m];
            _ccw[m] = ccw[m];
            pinMode(_cw[m],  OUTPUT);
            pinMode(_ccw[m], OUTPUT);
            digitalWrite(_cw[m],  LOW);
            digitalWrite(_ccw[m], LOW);
            _lastDir[m] = 0;
        }
    }

    /// Tick once per loop with each motor's commanded signed duty, the
    /// measured current (mA) for the over-current fault check, and a
    /// per-motor error flag (e.g. a latched endstop-seek timeout from the
    /// BiDcMotor role) that also blinks the pair fast.
    void update(const int16_t signedDuty[3], const int16_t current_mA[3],
                const bool error[3]) {
        const uint32_t now       = millis();
        const bool     blinkSlow = ((now / 250) & 1) != 0;   // 250 ms half-period
        const bool     blinkFast = ((now / 100) & 1) != 0;   // 100 ms (fault)
        for (uint8_t m = 0; m < 3; ++m) {
            bool cw = false, ccw = false;
            const bool overcurrent = signedDuty[m] != 0 &&
                               (current_mA[m] > Sense::FAULT_mA ||
                                current_mA[m] < -Sense::FAULT_mA);
            const bool fault = overcurrent || error[m];
            if (fault) {
                cw = ccw = blinkFast;
            } else if (signedDuty[m] > 0) {
                cw = blinkSlow;  _lastDir[m] = +1;
            } else if (signedDuty[m] < 0) {
                ccw = blinkSlow; _lastDir[m] = -1;
            } else {
                cw  = (_lastDir[m] > 0);   // idle → hold last direction solid
                ccw = (_lastDir[m] < 0);
            }
            digitalWrite(_cw[m],  cw  ? HIGH : LOW);
            digitalWrite(_ccw[m], ccw ? HIGH : LOW);
        }
    }

private:
    uint8_t _cw[3]      = {0, 0, 0};
    uint8_t _ccw[3]     = {0, 0, 0};
    int8_t  _lastDir[3] = {0, 0, 0};
};

// ── Board class ──────────────────────────────────────────────────────
//
// BoardOf<TBoard, TStream, Caps, …> auto-prepends BoardServicePolicy +
// IndicatorServicePolicy + PortServicePolicy + RoleServicePolicy.  A thin
// expander needs no user policies — the hub drives everything through the
// role surface.  TStream is the USB-CDC `Serial` wrapped as an sfx::Stream
// (Rule 55 — the Pico wire adapter); Caps sizes the port registry EXACTLY
// to this board's hub-local hardware (7 servo / 0 pwm / 3 hbridge / 0 input).

/// USB-CDC wire type — `Serial`'s concrete class wrapped as sfx::Stream.
using GearWireStream = sfx::PicoSerialStream<std::remove_reference_t<decltype(Serial)>>;

class GearControlBoard : public sfx_core::BoardOf<GearControlBoard,
                                                  GearWireStream,
                                                  sfx_core::PortCapacity</*servo*/7, /*pwm*/0,
                                                                          /*hbridge*/3, /*input*/0>> {
public:
    // ── Servo OUTPUT ports — door + yaw headers (SERVO1-7) ───────────
    sfx_peripherals::MicroservoPort servoOut[7] = {
        {1}, {2}, {3}, {6}, {7}, {8}, {9},
    };

    // ── Motor H-bridges — 6 native PWM pins → 3 dual-PWM bridges ─────
    // Declaration order matters: the half-bridge PwmPorts must exist
    // before the DualPwmHBridgePort instances reference them.
    // NativePwmPort's ctor is `explicit`, so direct-list-init each elem.
    sfx_peripherals::NativePwmPort motorPwm[6] = {
        sfx_peripherals::NativePwmPort{15}, sfx_peripherals::NativePwmPort{16},
        sfx_peripherals::NativePwmPort{17}, sfx_peripherals::NativePwmPort{18},
        sfx_peripherals::NativePwmPort{19}, sfx_peripherals::NativePwmPort{20},
    };
    sfx_peripherals::DualPwmHBridgePort motor[3] = {
        {motorPwm[0], motorPwm[1]},   // motor 0: fwd=GP15 rev=GP16
        {motorPwm[2], motorPwm[3]},   // motor 1: fwd=GP17 rev=GP18
        {motorPwm[4], motorPwm[5]},   // motor 2: fwd=GP19 rev=GP20
    };

    // ── Per-motor current monitors (stall detection) ─────────────────
    // Native Pico-SDK I²C bus (Rule 55 — no Arduino Wire); the INA226 driver
    // takes the bus by reference and is bus-type-agnostic via SfxI2cBus.
    sfx_peripherals::SfxI2cBus i2cBus;
    INA226 ina[3];
    // Each INA226 measures BOTH motor current (stall detection) AND bus voltage
    // — wire both so the BiDcMotor role reports real current_mA + voltage_mV.
    sfx_peripherals::Ina226CurrentSensor iSense[3] = {
        {ina[0]}, {ina[1]}, {ina[2]},
    };
    sfx_peripherals::Ina226VoltageSensor vSense[3] = {
        {ina[0]}, {ina[1]}, {ina[2]},
    };

    /// I²C + INA226 bring-up — runs BEFORE board.begin() so the H-bridge
    /// current sensors are live when the registry walks port begin().
    void initHardware() {
        i2cBus.begin(Gpio::I2C_SDA, Gpio::I2C_SCL, 400000);
        for (uint8_t k = 0; k < 3; ++k) {
            ina[k].begin(i2cBus, I2cAddr::INA226[k], Sense::SHUNT_OHMS, Sense::MAX_AMPS);
        }
    }

    /// Per-loop maintenance — refresh INA226 cached readings so the
    /// BiDcMotor role's stall detector sees fresh current.
    void pollSense() {
        for (uint8_t k = 0; k < 3; ++k) ina[k].update();
    }

    // ── Port declarations (compile-time, consumed by BoardOf<>) ──────

    static constexpr auto kServoPorts = sfx_core::ports::list(
        sfx_core::ports::servo_array<&GearControlBoard::servoOut, 7>());

    static constexpr auto kHBridgePorts = sfx_core::ports::list(
        sfx_core::ports::hbridge_array<&GearControlBoard::motor, 3>()
            .with_iSense_array<&GearControlBoard::iSense>()
            .with_vSense_array<&GearControlBoard::vSense>());

    static constexpr const char* kName = "GearCtrl";
};

GearControlBoard board;
GearWireStream   wireStream{Serial};   // USB-CDC wire to the hub (sfx::Stream adapter)
GearStatusLeds   statusLeds;           // local H-bridge → status-LED driver

void setup() {
    // Bring the USB-CDC wire up first — baud is ignored over USB, but this
    // brings the endpoint up before the framer starts reading it.
    Serial.begin(115200);

    // I²C + INA226 bring-up before the registry begins each port.
    board.initHardware();

    // Policy pack lifecycle — wire / DiagLog / indicator pins / port
    // registry binding / every policy's begin() / IDENTIFY capabilities.
    board.begin(wireStream, FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);

    // Local status-LED driver — direction indicators per H-bridge.
    statusLeds.begin(Gpio::STATUS_CW, Gpio::STATUS_CCW);

    // Stream INFO+ log lines live on the wire so the CLI / Studio console see
    // the seek/stall trace (default is ring-buffer only, no wire emission).
    DiagLog::instance().setWireMinLevel(DiagLevel::INFO);

    SFX_LOG_INFO("GearCtrl expander v%s build %u — 7 servo / 3 hbridge + local status LEDs",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER);
}

void loop() {
    board.process();
    board.pollSense();

    // Drive the per-motor status LEDs purely from local H-bridge state.
    // The error flag is the BiDcMotor role's latched endstop-seek timeout
    // (read from the registry) — the seek + its timeout run locally on
    // this board, so the error indication is local too.
    int16_t duty[3], cur[3];
    bool    err[3];
    for (uint8_t m = 0; m < 3; ++m) {
        duty[m] = board.motor[m].signedDuty();
        cur[m]  = board.iSense[m].current_mA();
        err[m]  = false;
        auto* binding = board.registry().hbridgeAt(m);
        if (binding) {
            auto* role = std::get_if<sfx_core::BiDcMotorRole>(&binding->role);
            err[m] = role &&
                     role->seekState() == sfx_core::BiDcMotorRole::SeekState::TimedOut;
        }
    }
    statusLeds.update(duty, cur, err);

    busy_wait_ms(1);
}
