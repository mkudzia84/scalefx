/*
 * pico_native_gpio.h — native Pico SDK MCU GPIO + PWM (no Arduino).
 *
 * Concrete PwmOutput provider for RP2040/RP2350: digital I/O via
 * `hardware/gpio`, brightness via `hardware/pwm` (8-bit wrap on the pin's
 * slice).  Same surface as EspNativeGpio.  Selected via `native_gpio.h`.
 */

#ifndef SFX_PICO_NATIVE_GPIO_H
#define SFX_PICO_NATIVE_GPIO_H

#include <platform/sfx_platform.h>
#if SFX_PLATFORM_PICO

#include <cstdint>
#include <hardware/gpio.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>

class PicoNativeGpio {
public:
    static PicoNativeGpio& instance() {
        static PicoNativeGpio inst;
        return inst;
    }

    static constexpr uint8_t NUM_PINS   = 30;    // RP2040 GP0..29
    static constexpr bool    HAS_HW_PWM = true;

    bool begin()              { return true; }
    bool isAvailable() const  { return true; }

    bool setPinDirection(uint8_t pin, bool isInput) {
        gpio_init((uint)pin);
        gpio_set_dir((uint)pin, isInput ? GPIO_IN : GPIO_OUT);
        return true;
    }
    bool writePin(uint8_t pin, bool high) {
        gpio_put((uint)pin, high);
        return true;
    }
    bool readPin(uint8_t pin) {
        return gpio_get((uint)pin);
    }

    /// 8-bit brightness via the pin's PWM slice (wrap=255).
    bool setLedBrightness(uint8_t pin, uint8_t brightness) {
        gpio_set_function((uint)pin, GPIO_FUNC_PWM);
        const uint slice = pwm_gpio_to_slice_num((uint)pin);
        pwm_set_wrap(slice, 255);
        pwm_set_gpio_level((uint)pin, brightness);
        pwm_set_enabled(slice, true);
        return true;
    }

    /// Set the pin's PWM carrier frequency (slice clock divider; wrap stays
    /// 255 → duty resolution unchanged).  With NO divider a slice free-runs
    /// at clk_sys/256 ≈ 490 kHz — fine for an LED, far beyond what H-bridge
    /// driver ICs switch cleanly (whine + mushy torque at partial duty).
    /// Motor ports set ~20 kHz (ultrasonic, in-spec).  NOTE: frequency is
    /// per SLICE — the pin's slice-partner changes with it (harmless for
    /// LEDs, which don't care about carrier frequency).
    bool setPinPwmFrequencyHz(uint8_t pin, uint32_t hz) {
        if (hz == 0) return false;
        const uint slice = pwm_gpio_to_slice_num((uint)pin);
        float div = (float)clock_get_hz(clk_sys) / (256.0f * (float)hz);
        if (div < 1.0f)   div = 1.0f;     // ceiling: clk_sys/256
        if (div > 255.9f) div = 255.9f;   // floor: ~1.9 kHz @125 MHz
        pwm_set_clkdiv(slice, div);
        return true;
    }
};

#endif  // SFX_PLATFORM_PICO
#endif  // SFX_PICO_NATIVE_GPIO_H
