/*
 * sfx_servo.h — platform-selected hobby-servo driver.
 *
 * Resolves `sfx_peripherals::ServoDriver` to the concrete servo backend:
 *   - ESP32-S3 → EspServo (native MCPWM, no Arduino — see esp_servo.h)
 *   - Pico     → Arduino-Pico `Servo` (PIO-based); the native Pico SDK servo
 *                migration is tracked as P8 in 25-ARDUINO-REMOVAL.md.
 *
 * The backend exposes the Arduino-Servo subset MicroservoPort uses
 * (attach / attached / writeMicroseconds) so servo_port.h is backend-agnostic.
 */

#ifndef SFX_PERIPHERAL_SFX_SERVO_H
#define SFX_PERIPHERAL_SFX_SERVO_H

#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32
    #include "esp_servo.h"
    namespace sfx_peripherals { using ServoDriver = EspServo; }
#elif SFX_PLATFORM_PICO
    #include <Servo.h>   // Arduino-Pico PIO servo (native Pico = P8)
    namespace sfx_peripherals { using ServoDriver = ::Servo; }
#endif

#endif  // SFX_PERIPHERAL_SFX_SERVO_H
