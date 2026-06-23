/*
 * esp_native_gpio.h — native ESP-IDF MCU GPIO + LEDC PWM (no Arduino).
 *
 * Concrete (no virtual / RTTI) PwmOutput provider for MCU-direct pins on the
 * ESP32-S3: digital I/O via `driver/gpio`, brightness via `driver/ledc`
 * (lazily binding a LEDC channel to a pin on first setLedBrightness).  Same
 * surface as the I²C PWM chips so `LedControlT<NativeGpio>` is bus-agnostic.
 * Selected via `native_gpio.h`.
 */

#ifndef SFX_ESP_NATIVE_GPIO_H
#define SFX_ESP_NATIVE_GPIO_H

#include <platform/sfx_platform.h>
#if SFX_PLATFORM_ESP32

#include <cstdint>
#include <driver/gpio.h>
#include <driver/ledc.h>

class EspNativeGpio {
public:
    static EspNativeGpio& instance() {
        static EspNativeGpio inst;          // C++11 thread-safe static local
        return inst;
    }

    static constexpr uint8_t NUM_PINS   = 48;    // ESP32-S3 GPIO 0..48
    static constexpr bool    HAS_HW_PWM = true;

    bool begin()              { return true; }
    bool isAvailable() const  { return true; }

    bool setPinDirection(uint8_t pin, bool isInput) {
        gpio_reset_pin((gpio_num_t)pin);
        gpio_set_direction((gpio_num_t)pin,
                           isInput ? GPIO_MODE_INPUT : GPIO_MODE_OUTPUT);
        return true;
    }
    bool writePin(uint8_t pin, bool high) {
        gpio_set_level((gpio_num_t)pin, high ? 1 : 0);
        return true;
    }
    bool readPin(uint8_t pin) {
        return gpio_get_level((gpio_num_t)pin) != 0;
    }

    /// 8-bit brightness via LEDC (lazy per-pin channel; ~5 kHz, low-speed).
    bool setLedBrightness(uint8_t pin, uint8_t brightness) {
        const int ch = channelForPin(pin);
        if (ch < 0) return false;
        // 8-bit timer: full-scale duty is 2^8 = 256, not 255 — so brightness 255
        // maps to 256 for a true 100% (always-on) duty, not 255/256.
        const uint32_t duty = (brightness == 255) ? 256u : (uint32_t)brightness;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch);
        return true;
    }

private:
    static constexpr int kMaxLedc = 8;   // LEDC low-speed channels on the S3
    struct Slot { uint8_t pin = 0; bool used = false; };
    Slot _ledc[kMaxLedc] = {};
    bool _timerReady = false;

    void ensureTimer() {
        if (_timerReady) return;
        ledc_timer_config_t t = {};
        t.speed_mode      = LEDC_LOW_SPEED_MODE;
        t.timer_num       = LEDC_TIMER_0;
        t.duty_resolution = LEDC_TIMER_8_BIT;
        t.freq_hz         = 5000;
        t.clk_cfg         = LEDC_AUTO_CLK;
        ledc_timer_config(&t);
        _timerReady = true;
    }

    int channelForPin(uint8_t pin) {
        for (int i = 0; i < kMaxLedc; ++i)
            if (_ledc[i].used && _ledc[i].pin == pin) return i;
        for (int i = 0; i < kMaxLedc; ++i) {
            if (_ledc[i].used) continue;
            ensureTimer();
            ledc_channel_config_t ch = {};
            ch.gpio_num   = pin;
            ch.speed_mode = LEDC_LOW_SPEED_MODE;
            ch.channel    = (ledc_channel_t)i;
            ch.timer_sel  = LEDC_TIMER_0;
            ch.duty       = 0;
            ch.hpoint     = 0;
            ch.intr_type  = LEDC_INTR_DISABLE;
            if (ledc_channel_config(&ch) != ESP_OK) return -1;
            _ledc[i] = { pin, true };
            return i;
        }
        return -1;   // all channels taken
    }
};

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_ESP_NATIVE_GPIO_H
