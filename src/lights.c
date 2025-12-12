#include "lights.h"
#include "gpio.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <threads.h>
#include <unistd.h>
#include <sys/time.h>

struct Led {
    int pin;
    atomic_bool is_on;
    atomic_bool is_blinking;
    atomic_int blink_interval_ms;
    
    thrd_t blink_thread;
    atomic_bool blink_thread_running;
};

// Blink thread function
static int led_blink_thread(void *arg) {
    Led *led = (Led *)arg;
    
    while (atomic_load(&led->blink_thread_running)) {
        bool should_blink = atomic_load(&led->is_blinking) && atomic_load(&led->is_on);
        int interval = atomic_load(&led->blink_interval_ms);
        
        if (!should_blink) {
            usleep(10000); // 10ms sleep when not blinking
            continue;
        }
        
        // Calculate half interval (on time and off time)
        int half_interval_us = (interval * 1000) / 2;
        
        // Turn LED on (high)
        gpio_write(led->pin, 1);
        usleep(half_interval_us);
        
        // Turn LED off (low)
        gpio_write(led->pin, 0);
        usleep(half_interval_us);
    }
    
    return thrd_success;
}

Led* led_create(int pin) {
    if (pin < 0) {
        LOG_ERROR(LOG_LIGHTS, "Invalid pin number");
        return nullptr;
    }
    
    Led *led = calloc(1, sizeof(Led));
    if (!led) {
        LOG_ERROR(LOG_LIGHTS, "Cannot allocate memory for LED");
        return nullptr;
    }
    
    led->pin = pin;
    atomic_init(&led->is_on, false);
    atomic_init(&led->is_blinking, false);
    atomic_init(&led->blink_interval_ms, 1000); // Default 1 second
    atomic_init(&led->blink_thread_running, false);
    
    // Set pin as output and initialize to LOW
    if (gpio_set_mode(pin, GPIO_MODE_OUTPUT) < 0) {
        LOG_ERROR(LOG_LIGHTS, "Failed to set pin %d as output", pin);
        free(led);
        return nullptr;
    }
    
    gpio_write(pin, 0);
    
    // Start blink thread
    atomic_store(&led->blink_thread_running, true);
    if (thrd_create(&led->blink_thread, led_blink_thread, led) != thrd_success) {
        LOG_ERROR(LOG_LIGHTS, "Failed to create blink thread");
        atomic_store(&led->blink_thread_running, false);
        free(led);
        return nullptr;
    }
    
    LOG_INFO(LOG_LIGHTS, "LED created on pin %d", pin);
    return led;
}

void led_destroy(Led *led) {
    if (!led) return;
    
    // Stop blink thread
    if (atomic_load(&led->blink_thread_running)) {
        atomic_store(&led->blink_thread_running, false);
        thrd_join(led->blink_thread, nullptr);
    }
    
    // Turn off LED
    gpio_write(led->pin, 0);
    
    free(led);
    
    LOG_INFO(LOG_LIGHTS, "LED destroyed");
}

int led_on(Led *led) {
    if (!led) return -1;
    
    atomic_store(&led->is_on, true);
    atomic_store(&led->is_blinking, false);
    
    // Set LED to solid on
    gpio_write(led->pin, 1);
    
    LOG_INFO(LOG_LIGHTS, "LED on (solid)");
    return 0;
}

int led_off(Led *led) {
    if (!led) return -1;
    
    atomic_store(&led->is_on, false);
    atomic_store(&led->is_blinking, false);
    
    // Turn LED off
    gpio_write(led->pin, 0);
    
    LOG_INFO(LOG_LIGHTS, "LED off");
    return 0;
}

int led_blink(Led *led, int interval_ms) {
    if (!led || interval_ms <= 0) return -1;
    
    atomic_store(&led->is_on, true);
    atomic_store(&led->is_blinking, true);
    atomic_store(&led->blink_interval_ms, interval_ms);
    
    LOG_INFO(LOG_LIGHTS, "LED blinking (interval: %d ms)", interval_ms);
    return 0;
}

bool led_is_on(Led *led) {
    if (!led) return false;
    
    return atomic_load(&led->is_on);
}

bool led_is_blinking(Led *led) {
    if (!led) return false;
    
    return atomic_load(&led->is_blinking);
}
