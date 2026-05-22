/*
 * NoOp Pico — minimal generic-expander board / template (RP2040).
 *
 *   The smallest possible ScaleFX expander: 7 servo ports and nothing
 *   else.  Useful as a protocol-test target (serial / IDENTIFY / port +
 *   role surface), a servo-bring-up rig on GearControl hardware, and the
 *   copy-paste template for a new board.  All effect logic lives on the
 *   HubFX master — this board just exposes its ports and lets the hub
 *   attach `ServoActuator` roles and drive them over USB CDC.
 *   (Architecture: instructions/15-GENERIC-EXPANDER-REFACTOR.md +
 *   17-SYSTEM-SERVICES.md.)
 *
 *   Ports exposed:
 *     - 7 × Servo (GP1,2,3,6,7,8,9) → ServoActuator role
 *
 *   The 2 board indicator LEDs (GP13 connection, GP14 error) are driven
 *   by the auto-prepended IndicatorServicePolicy via board.begin().  An
 *   I²C bus scan is enabled for bench diagnostics (no expected devices).
 *
 *   Roles are NOT attached here — `RoleServicePolicy` (auto via BoardOf<>)
 *   accepts ROLE_ATTACH from the hub and emplaces the ServoActuator
 *   variants at runtime.
 *
 *   PINOUT (RP2040 GPIO — matches GearControl servo headers):
 *     GP1,2,3,6,7,8,9 : servo headers (SERVO0..6)
 *     GP4 / GP5       : I²C SDA / SCL (bus scan only)
 *     GP13 / GP14     : indicator LEDs (connection / error)
 */

#include <Arduino.h>
#include <Wire.h>

#include <platform/sfx_platform.h>
#include <serial/diag_log.h>
#include <server/board_of.h>

#include <ports/servo_port.h>

#define FIRMWARE_VERSION "1.0.0"
#define BUILD_NUMBER     2

namespace Gpio {
    constexpr int I2C_SDA        =  4;
    constexpr int I2C_SCL        =  5;
    constexpr int LED_CONNECTION = 13;
    constexpr int LED_ERROR      = 14;
}

class NoOpBoard : public sfx_core::BoardOf<NoOpBoard> {
public:
    // ── Servo OUTPUT ports — 7 headers (GearControl pin map) ─────────
    sfx_peripherals::MicroservoPort servoOut[7] = {
        {1}, {2}, {3}, {6}, {7}, {8}, {9},
    };

    static constexpr auto kServoPorts = sfx_core::ports::list(
        sfx_core::ports::servo_array<&NoOpBoard::servoOut, 7>());

    static constexpr const char* kName = "NoOp";
};

NoOpBoard board;

void setup() {
    Wire.setSDA(Gpio::I2C_SDA);
    Wire.setSCL(Gpio::I2C_SCL);
    Wire.begin();
    Wire.setClock(400000);

    board.begin(FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);
    board.enableI2CScan(Wire);   // bench diagnostics — no expected devices

    SFX_LOG_INFO("NoOp expander v%s build %u — 7 servo (test board / template)",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER);
}

void loop() {
    board.process();
    busy_wait_ms(1);
}
