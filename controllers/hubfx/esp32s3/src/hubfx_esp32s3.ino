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
#define BUILD_NUMBER 9

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
}

/// I²C device addresses on Wire (SDA/SCL above).
namespace I2cAddr {
    constexpr uint8_t PCA9685    = 0x70;   ///< 8-channel PWM driver

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

// ── Board class ───────────────────────────────────────────────────────

class HubFxBoard : public sfx_core::BoardOf<HubFxBoard> {
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

void setup() {
    // I²C peripherals come up before the port registry's per-port
    // begin() runs.  RC pulse capture / UART mode are armed by the
    // role on attach — the port's own begin() only sanity-checks args.
    board.initHardware();

    board.begin(FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);

    board.setConnectionTimeoutEnabled(false);     // master — no upstream watchdog

    SFX_LOG_INFO("HubFX v%s build %u — 8 PWM / 1 input / 11 servo-out",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER);
}

void loop() {
    board.process();
    board.pollSense();
}
