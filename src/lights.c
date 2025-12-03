#include "lights.h"
#include "gpio.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

struct Led {
    int pin;
    bool is_on;
    bool is_blinking;
    int blink_interval_ms;
    
    pthread_t blink_thread;
    bool blink_thread_running;
    pthread_mutex_t mutex;
};

// Blink thread function
static void* led_blink_thread(void *arg) {
    Led *led = (Led *)arg;
    
    while (led->blink_thread_running) {
        pthread_mutex_lock(&led->mutex);
        bool should_blink = led->is_blinking && led->is_on;
        int interval = led->blink_interval_ms;
        pthread_mutex_unlock(&led->mutex);
        
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
    
    return NULL;
}

Led* led_create(int pin) {
    if (pin < 0) {
        fprintf(stderr, "[LED] Error: Invalid pin number\n");
        return NULL;
    }
    
    Led *led = (Led *)calloc(1, sizeof(Led));
    if (!led) {
        fprintf(stderr, "[LED] Error: Cannot allocate memory for LED\n");
        return NULL;
    }
    
    led->pin = pin;
    led->is_on = false;
    led->is_blinking = false;
    led->blink_interval_ms = 1000; // Default 1 second
    led->blink_thread_running = false;
    pthread_mutex_init(&led->mutex, NULL);
    
    // Set pin as output and initialize to LOW
    if (gpio_set_mode(pin, GPIO_MODE_OUTPUT) < 0) {
        fprintf(stderr, "[LED] Error: Failed to set pin %d as output\n", pin);
        pthread_mutex_destroy(&led->mutex);
        free(led);
        return NULL;
    }
    
    gpio_write(pin, 0);
    
    // Start blink thread
    led->blink_thread_running = true;
    if (pthread_create(&led->blink_thread, NULL, led_blink_thread, led) != 0) {
        fprintf(stderr, "[LED] Error: Failed to create blink thread\n");
        led->blink_thread_running = false;
        pthread_mutex_destroy(&led->mutex);
        free(led);
        return NULL;
    }
    
    printf("[LED] LED created on pin %d\n", pin);
    return led;
}

void led_destroy(Led *led) {
    if (!led) return;
    
    // Stop blink thread
    if (led->blink_thread_running) {
        led->blink_thread_running = false;
        pthread_join(led->blink_thread, NULL);
    }
    
    // Turn off LED
    gpio_write(led->pin, 0);
    
    pthread_mutex_destroy(&led->mutex);
    free(led);
    
    printf("[LED] LED destroyed\n");
}

int led_on(Led *led) {
    if (!led) return -1;
    
    pthread_mutex_lock(&led->mutex);
    led->is_on = true;
    led->is_blinking = false;
    pthread_mutex_unlock(&led->mutex);
    
    // Set LED to solid on
    gpio_write(led->pin, 1);
    
    printf("[LED] LED on (solid)\n");
    return 0;
}

int led_off(Led *led) {
    if (!led) return -1;
    
    pthread_mutex_lock(&led->mutex);
    led->is_on = false;
    led->is_blinking = false;
    pthread_mutex_unlock(&led->mutex);
    
    // Turn LED off
    gpio_write(led->pin, 0);
    
    printf("[LED] LED off\n");
    return 0;
}

int led_blink(Led *led, int interval_ms) {
    if (!led || interval_ms <= 0) return -1;
    
    pthread_mutex_lock(&led->mutex);
    led->is_on = true;
    led->is_blinking = true;
    led->blink_interval_ms = interval_ms;
    pthread_mutex_unlock(&led->mutex);
    
    printf("[LED] LED blinking (interval: %d ms)\n", interval_ms);
    return 0;
}

bool led_is_on(Led *led) {
    if (!led) return false;
    
    pthread_mutex_lock(&led->mutex);
    bool on = led->is_on;
    pthread_mutex_unlock(&led->mutex);
    
    return on;
}

bool led_is_blinking(Led *led) {
    if (!led) return false;
    
    pthread_mutex_lock(&led->mutex);
    bool blinking = led->is_blinking;
    pthread_mutex_unlock(&led->mutex);
    
    return blinking;
}
