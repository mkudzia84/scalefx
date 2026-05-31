/*
 * native_gpio.h — platform-selected MCU GPIO / PWM provider.
 *
 * Resolves `NativeGpio` to the concrete native provider for the target
 * (EspNativeGpio on ESP-IDF, PicoNativeGpio on the Pico SDK).  Compile-time
 * dispatch only — no virtual, no RTTI, no Arduino.  Satisfies the PwmOutput
 * concept (pwm/pwm_output.h) so `LedControlT<NativeGpio>` /
 * `LedManager<N, NativeGpio>` work identically against MCU pins or an I²C PWM
 * chip — just change the template argument.
 *
 *   NativeGpio& gpio = NativeGpio::instance();
 *   gpio.setPinDirection(13, false);   // OUTPUT
 *   gpio.writePin(13, true);           // HIGH
 *   gpio.setLedBrightness(13, 128);    // 50% PWM (LEDC / RP2040 slice)
 */

#ifndef NATIVE_GPIO_H
#define NATIVE_GPIO_H

#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32
    #include "esp_native_gpio.h"
    using NativeGpio = EspNativeGpio;
#elif SFX_PLATFORM_PICO
    #include "pico_native_gpio.h"
    using NativeGpio = PicoNativeGpio;
#endif

// Informational pin count (kept for back-compat with callers that referenced it).
constexpr uint8_t NATIVE_GPIO_MAX_PINS = NativeGpio::NUM_PINS;

#endif  // NATIVE_GPIO_H
