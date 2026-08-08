/*
 * PortExpander Pico — generic-expander board (RP2040).
 *
 *   A THIN port + role expander.  All control logic and sequencing live on
 *   the HubFX master — this board only exposes its physical ports and lets
 *   the hub attach roles + drive them over USB CDC.
 *
 *   Ports exposed (cross-referenced against instructions/schematics/
 *   expander.tel — see PINOUT below):
 *     - 8 × Servo    (Servo1-8 headers)              → ServoActuator role
 *     - 5 × HBridge  (PWM+DIR motor drivers U3/U5-U8) → BiDcMotor role,
 *                     each with an INA226 current sensor for diagnostics
 *
 *   Board LEDs: LED1 (GP25) + LED2 (GP24) are the two indicator LEDs
 *   driven by the auto-prepended IndicatorServicePolicy via board.begin().
 *   L_CH1 / L_CH2 hang PASSIVELY off the CH1/CH2 input nets and POWER
 *   hangs off VCC — none are firmware-driven (no per-motor status LEDs
 *   on this board, unlike GearControl).
 *
 *   NO input port yet — CH1 (GP0) / CH2 (GP1) are reserved for future
 *   input channels; the hub commands all outputs over the wire.
 *
 *   Roles are NOT attached here — `RoleServicePolicy` (auto via
 *   BoardOf<>) accepts ROLE_ATTACH from the hub and emplaces the
 *   ServoActuator / BiDcMotor variants at runtime.
 *
 *   PINOUT (RP2040 GPIO — authoritative; decoded from the expander.tel
 *   netlist, QFN-56 package pin → GPIO):
 *     GP14..GP21      : servo headers (SERVO1-8, U1.17/18/27..32)
 *     GP4  / GP5      : I²C0 SDA / SCL (5× INA226)
 *     GP12/13, GP10/11, GP2/3, GP6/7, GP8/9 :
 *                       motor PWM/DIR pairs (MOT1..MOT5 → U3/U5/U6/U7/U8)
 *     GP25 / GP24     : indicator LEDs (LED1 / LED2)
 *     GP0  / GP1      : CH1 / CH2 reserved input channels (UART0-capable)
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
#include <i2c/sfx_i2c.h>                   // sfx_peripherals::SfxI2cBus (= PicoI2cBus)
#include <power/ina226.h>
#include <power/ina226_sensor.h>

#define FIRMWARE_VERSION "1.0.0-rc2"
#define BUILD_NUMBER     6

// ════════════════════════════════════════════════════════════════════════
//  Board pin / address map (cross-ref: instructions/schematics/expander.tel)
// ════════════════════════════════════════════════════════════════════════

namespace Gpio {
    // I²C0 bus — 5× INA226 motor-current monitors (SDA=U1.6, SCL=U1.7).
    constexpr int I2C_SDA        =  4;
    constexpr int I2C_SCL        =  5;

    // Indicator LEDs (IndicatorServicePolicy).  LED1=U1.37, LED2=U1.36.
    constexpr int LED_CONNECTION = 25;   // LED1
    constexpr int LED_ERROR      = 24;   // LED2

    // Reserved input channels (CH1=U1.2, CH2=U1.3) — future use.
    constexpr int CH1_INPUT      =  0;
    constexpr int CH2_INPUT      =  1;
}

namespace I2cAddr {
    // Strap-selected INA226 addresses, one per motor, decoded from the
    // netlist (A1/A0 pins 1/2): addr = 0x40 + A1*4 + A0 with
    // GND=0, VS=1, SDA=2, SCL=3.
    //   MOT1 → U43 (A1=GND, A0=GND) → 0x40
    //   MOT2 → U13 (A1=VS,  A0=VS ) → 0x45
    //   MOT3 → U14 (A1=VS,  A0=GND) → 0x44
    //   MOT4 → U15 (A1=GND, A0=VS ) → 0x41
    //   MOT5 → U16 (A1=GND, A0=SDA) → 0x42
    constexpr uint8_t INA226[5] = { 0x40, 0x45, 0x44, 0x41, 0x42 };
}

namespace Sense {
    // Shunts R83/R8/R9/R10/R11 — value NOT in the netlist; 5 mΩ assumed
    // (same as GearControl).  VERIFY against the BOM before trusting
    // absolute current readings.
    constexpr float SHUNT_OHMS = 0.005f;   // 5 mΩ shunt (≈16 A INA226 ceiling)
    constexpr float MAX_AMPS   = 10.0f;    // 10 A expected peak
}

// ── Board class ──────────────────────────────────────────────────────
//
// BoardOf<TBoard, TStream, Caps, …> auto-prepends BoardServicePolicy +
// IndicatorServicePolicy + PortServicePolicy + RoleServicePolicy.  A thin
// expander needs no user policies — the hub drives everything through the
// role surface.  TStream is the USB-CDC `Serial` wrapped as an sfx::Stream
// (Rule 55 — the Pico wire adapter); Caps sizes the port registry EXACTLY
// to this board's hardware (8 servo / 0 pwm / 5 hbridge / 0 input).

/// USB-CDC wire type — `Serial`'s concrete class wrapped as sfx::Stream.
using PortExpanderWireStream = sfx::PicoSerialStream<std::remove_reference_t<decltype(Serial)>>;

class PortExpanderBoard : public sfx_core::BoardOf<PortExpanderBoard,
                                                   PortExpanderWireStream,
                                                   sfx_core::PortCapacity</*servo*/8, /*pwm*/0,
                                                                          /*hbridge*/5, /*input*/0>> {
public:
    // ── Servo OUTPUT ports — Servo1-8 headers (GP14..GP21) ───────────
    sfx_peripherals::MicroservoPort servoOut[8] = {
        {14}, {15}, {16}, {17}, {18}, {19}, {20}, {21},
    };

    // ── Motor H-bridges — 5 PWM+DIR driver pairs ─────────────────────
    // The drivers (U3/U5/U6/U7/U8) take one PWM magnitude + one GPIO
    // direction pin → PwmDirHBridgePort (brake falls back to coast).
    // Declaration order matters: the PwmPorts must exist before the
    // PwmDirHBridgePort instances reference them.
    sfx_peripherals::NativePwmPort motorPwm[5] = {
        sfx_peripherals::NativePwmPort{12},   // PWM1 (MOT1)
        sfx_peripherals::NativePwmPort{10},   // PWM2 (MOT2)
        sfx_peripherals::NativePwmPort{2},    // PWM3 (MOT3)
        sfx_peripherals::NativePwmPort{6},    // PWM4 (MOT4)
        sfx_peripherals::NativePwmPort{8},    // PWM5 (MOT5)
    };
    sfx_peripherals::PwmDirHBridgePort motor[5] = {
        {motorPwm[0], /*dir*/13},   // MOT1: PWM=GP12 DIR=GP13
        {motorPwm[1], /*dir*/11},   // MOT2: PWM=GP10 DIR=GP11
        {motorPwm[2], /*dir*/ 3},   // MOT3: PWM=GP2  DIR=GP3
        {motorPwm[3], /*dir*/ 7},   // MOT4: PWM=GP6  DIR=GP7
        {motorPwm[4], /*dir*/ 9},   // MOT5: PWM=GP8  DIR=GP9
    };

    // ── Per-motor current monitors (diagnostics) ─────────────────────
    // Native Pico-SDK I²C bus (Rule 55 — no Arduino Wire); the INA226
    // driver takes the bus by reference and is bus-type-agnostic.
    sfx_peripherals::SfxI2cBus i2cBus;
    INA226 ina[5];
    // Each INA226 measures BOTH motor current AND bus voltage — wire both
    // so the BiDcMotor role reports real current_mA + voltage_mV.
    sfx_peripherals::Ina226CurrentSensor iSense[5] = {
        {ina[0]}, {ina[1]}, {ina[2]}, {ina[3]}, {ina[4]},
    };
    sfx_peripherals::Ina226VoltageSensor vSense[5] = {
        {ina[0]}, {ina[1]}, {ina[2]}, {ina[3]}, {ina[4]},
    };

    /// Motor PWM carrier — ultrasonic + within H-bridge driver spec (the
    /// platform default clk_sys/256 ≈ 490 kHz free-run whines and delivers
    /// mushy torque at partial duty; see the GearControl bench note).
    static constexpr uint16_t kMotorPwmHz = 20000;

    /// I²C + INA226 bring-up — runs BEFORE board.begin() so the H-bridge
    /// current sensors are live when the registry walks port begin().
    void initHardware() {
        i2cBus.begin(Gpio::I2C_SDA, Gpio::I2C_SCL, 400000);
        for (uint8_t k = 0; k < 5; ++k) {
            ina[k].begin(i2cBus, I2cAddr::INA226[k], Sense::SHUNT_OHMS, Sense::MAX_AMPS);
        }
        for (uint8_t m = 0; m < 5; ++m) {
            motor[m].setFrequencyHz(kMotorPwmHz);   // magnitude PWM pin
        }
    }

    /// Per-loop maintenance — refresh INA226 cached readings so the
    /// BiDcMotor role's diagnostics see fresh current.
    void pollSense() {
        for (uint8_t k = 0; k < 5; ++k) ina[k].update();
    }

    // ── Port declarations (compile-time, consumed by BoardOf<>) ──────

    static constexpr auto kServoPorts = sfx_core::ports::list(
        sfx_core::ports::servo_array<&PortExpanderBoard::servoOut, 8>());

    static constexpr auto kHBridgePorts = sfx_core::ports::list(
        sfx_core::ports::hbridge_array<&PortExpanderBoard::motor, 5>()
            .with_iSense_array<&PortExpanderBoard::iSense>()
            .with_vSense_array<&PortExpanderBoard::vSense>());

    static constexpr const char* kName = "PortExp";
};

PortExpanderBoard      board;
PortExpanderWireStream wireStream{Serial};   // USB-CDC wire to the hub

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

    // Stream INFO+ log lines live on the wire so the CLI / Studio console
    // see diagnostics (default is ring-buffer only, no wire emission).
    DiagLog::instance().setWireMinLevel(DiagLevel::INFO);

    SFX_LOG_INFO("PortExp expander v%s build %u — 8 servo / 5 hbridge",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER);
}

void loop() {
    board.process();
    board.pollSense();
    busy_wait_ms(1);
}
