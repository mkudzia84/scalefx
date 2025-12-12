#include "gpio.h"
#include <pigpio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <threads.h>
#include "logging.h"

static bool initialized = false;

int gpio_init(void) {
    if (initialized) {
        LOG_WARN(LOG_GPIO, "GPIO already initialized");
        return 0;
    }
    
    // Initialize pigpio library
    int status = gpioInitialise();
    if (status < 0) {
        LOG_ERROR(LOG_GPIO, "gpioInitialise failed with code %d", status);
        return -1;
    }
    
    initialized = true;
    LOG_INFO(LOG_GPIO, "GPIO subsystem initialized (pigpio version %d)", status);
    return 0;
}

void gpio_cleanup(void) {
    if (!initialized) return;
    
    gpioTerminate();
    initialized = false;
    LOG_INFO(LOG_GPIO, "GPIO subsystem cleaned up");
}

int gpio_set_mode(int pin, GPIOMode mode) {
    if (!initialized) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return -1;
    }
    
    int result;
    if (mode == GPIO_MODE_INPUT) {
        result = gpioSetMode(pin, PI_INPUT);
    } else {
        result = gpioSetMode(pin, PI_OUTPUT);
    }
    
    if (result < 0) {
        LOG_ERROR(LOG_GPIO, "gpioSetMode failed for pin %d: %d", pin, result);
        return -1;
    }
    
    return 0;
}

int gpio_set_pull(int pin, GPIOPull pull) {
    if (!initialized) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return -1;
    }
    
    int pigpio_pull;
    switch (pull) {
        case GPIO_PULL_OFF:
            pigpio_pull = PI_PUD_OFF;
            break;
        case GPIO_PULL_DOWN:
            pigpio_pull = PI_PUD_DOWN;
            break;
        case GPIO_PULL_UP:
            pigpio_pull = PI_PUD_UP;
            break;
        default:
            LOG_ERROR(LOG_GPIO, "Invalid pull mode %d", pull);
            return -1;
    }
    
    int result = gpioSetPullUpDown(pin, pigpio_pull);
    if (result < 0) {
        LOG_ERROR(LOG_GPIO, "gpioSetPullUpDown failed for pin %d: %d", pin, result);
        return -1;
    }
    
    return 0;
}

int gpio_write(int pin, bool value) {
    if (!initialized) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return -1;
    }
    
    int result = gpioWrite(pin, value ? 1 : 0);
    if (result < 0) {
        LOG_ERROR(LOG_GPIO, "gpioWrite failed for pin %d: %d", pin, result);
        return -1;
    }
    
    return 0;
}

bool gpio_read(int pin) {
    if (!initialized) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return false;
    }
    
    int result = gpioRead(pin);
    if (result < 0) {
        LOG_ERROR(LOG_GPIO, "gpioRead failed for pin %d: %d", pin, result);
        return false;
    }
    
    return result != 0;
}

// ============================================================================
// ASYNC PWM MONITOR IMPLEMENTATION (using pigpio alerts)
// ============================================================================

struct PWMMonitor {
    int pin;
    char *feature_name;  // Feature name for logging (e.g., "Trigger", "Pitch Servo")
    mtx_t mutex;
    
    bool running;
    bool has_new_reading;
    bool first_signal_received;
    
    PWMReading current_reading;
    
    PWMCallback callback;
    void *user_data;
    
    // Track rising edge for pulse width calculation
    uint32_t rise_tick;
    bool waiting_for_fall;

    // Averaging window and ring buffer for recent readings
    int avg_window_ms;           // Default 200ms
    #define PWM_AVG_MAX_SAMPLES 128
    struct {
        int duration_us;
        struct timespec ts;
    } samples[PWM_AVG_MAX_SAMPLES];
    int sample_head;             // next write index
};

// pigpio alert callback - called on every edge
static void pwm_alert_callback(int gpio, int level, uint32_t tick, void *userdata) {
    PWMMonitor *monitor = (PWMMonitor *)userdata;
    if (!monitor || !monitor->running) return;
    
    if (level == 1) {
        // Rising edge - start of pulse
        monitor->rise_tick = tick;
        monitor->waiting_for_fall = true;
    } else if (level == 0 && monitor->waiting_for_fall) {
        // Falling edge - end of pulse
        uint32_t pulse_width;
        
        // Handle tick wrap-around (ticks wrap at 2^32 microseconds)
        if (tick >= monitor->rise_tick) {
            pulse_width = tick - monitor->rise_tick;
        } else {
            pulse_width = (0xFFFFFFFF - monitor->rise_tick) + tick + 1;
        }
        
        // Sanity check: typical RC PWM is 1000-2000µs, allow 500-3000µs
        if (pulse_width >= 500 && pulse_width <= 3000) {
            if (!monitor->first_signal_received) {
                LOG_INFO(LOG_GPIO, "First PWM signal received on [%s] pin %d: %u µs",
                         monitor->feature_name ?: "Unknown", gpio, pulse_width);
                monitor->first_signal_received = true;
            }
            
            PWMReading reading;
            reading.pin = gpio;
            reading.duration_us = (int)pulse_width;
            
            mtx_lock(&monitor->mutex);
            
            // Update current reading
            monitor->current_reading = reading;
            monitor->has_new_reading = true;
            
            // Append to averaging buffer
            clock_gettime(CLOCK_MONOTONIC, &monitor->samples[monitor->sample_head].ts);
            monitor->samples[monitor->sample_head].duration_us = (int)pulse_width;
            monitor->sample_head = (monitor->sample_head + 1) % PWM_AVG_MAX_SAMPLES;
            
            mtx_unlock(&monitor->mutex);
            
            // Call user callback if set
            if (monitor->callback) {
                monitor->callback(reading, monitor->user_data);
            }
        }
        
        monitor->waiting_for_fall = false;
    }
}

PWMMonitor* pwm_monitor_create(int pin, PWMCallback callback, void *user_data) {
    return pwm_monitor_create_with_name(pin, nullptr, callback, user_data);
}

PWMMonitor* pwm_monitor_create_with_name(int pin, const char *feature_name, PWMCallback callback, void *user_data) {
    if (!initialized) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return nullptr;
    }
    
    if (pin < 0 || pin > 27) {
        LOG_ERROR(LOG_GPIO, "Invalid pin number %d (must be 0-27)", pin);
        return nullptr;
    }
    
    PWMMonitor *monitor = calloc(1, sizeof(PWMMonitor));
    if (!monitor) {
        LOG_ERROR(LOG_GPIO, "Cannot allocate memory for PWM monitor");
        return nullptr;
    }
    
    monitor->pin = pin;
    monitor->feature_name = feature_name ? strdup(feature_name) : nullptr;
    monitor->callback = callback;
    monitor->user_data = user_data;
    monitor->running = false;
    monitor->has_new_reading = false;
    monitor->first_signal_received = false;
    monitor->rise_tick = 0;
    monitor->waiting_for_fall = false;
    monitor->avg_window_ms = 200;  // default averaging window
    monitor->sample_head = 0;
    
    // Clear sample buffer
    memset(monitor->samples, 0, sizeof(monitor->samples));
    
    mtx_init(&monitor->mutex, mtx_plain);
    
    // Set pin as input
    if (gpio_set_mode(pin, GPIO_MODE_INPUT) < 0) {
        LOG_ERROR(LOG_GPIO, "Failed to set pin %d as input", pin);
        mtx_destroy(&monitor->mutex);
        free(monitor->feature_name);
        free(monitor);
        return nullptr;
    }
    
    LOG_INFO(LOG_GPIO, "PWM monitor created for [%s] pin %d", 
            feature_name ?: "Unknown", pin);
    return monitor;
}

void pwm_monitor_destroy(PWMMonitor *monitor) {
    if (!monitor) return;
    
    if (monitor->running) {
        pwm_monitor_stop(monitor);
    }
    
    mtx_destroy(&monitor->mutex);
    
    if (monitor->feature_name) {
        free(monitor->feature_name);
    }
    
    free(monitor);
    
    LOG_INFO(LOG_GPIO, "PWM monitor destroyed");
}

int pwm_monitor_start(PWMMonitor *monitor) {
    if (!monitor) return -1;
    
    if (monitor->running) {
        LOG_WARN(LOG_GPIO, "PWM monitor already running");
        return 0;
    }
    
    // Register pigpio alert callback
    int result = gpioSetAlertFuncEx(monitor->pin, pwm_alert_callback, monitor);
    if (result < 0) {
        LOG_ERROR(LOG_GPIO, "Failed to set alert function for pin %d: %d", monitor->pin, result);
        return -1;
    }
    
    monitor->running = true;
    
    LOG_INFO(LOG_GPIO, "PWM monitor started for [%s] pin %d (using pigpio alerts)", 
            monitor->feature_name ?: "Unknown", monitor->pin);
    return 0;
}

int pwm_monitor_stop(PWMMonitor *monitor) {
    if (!monitor) return -1;
    
    if (!monitor->running) {
        return 0;
    }
    
    // Unregister pigpio alert callback
    gpioSetAlertFuncEx(monitor->pin, nullptr, nullptr);
    
    monitor->running = false;
    
    LOG_INFO(LOG_GPIO, "PWM monitor stopped for [%s]", 
            monitor->feature_name ?: "Unknown");
    return 0;
}

bool pwm_monitor_get_reading(PWMMonitor *monitor, PWMReading *reading) {
    if (!monitor || !reading) return false;
    
    mtx_lock(&monitor->mutex);
    
    bool has_reading = monitor->has_new_reading;
    if (has_reading) {
        *reading = monitor->current_reading;
        monitor->has_new_reading = false;
    }
    
    mtx_unlock(&monitor->mutex);
    
    return has_reading;
}

bool pwm_monitor_wait_reading(PWMMonitor *monitor, PWMReading *reading, int timeout_ms) {
    if (!monitor || !reading) return false;
    
    // With pigpio alerts, we don't have blocking wait anymore
    // This function now polls with timeout
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    while (1) {
        mtx_lock(&monitor->mutex);
        bool has_reading = monitor->has_new_reading;
        if (has_reading) {
            *reading = monitor->current_reading;
            monitor->has_new_reading = false;
            mtx_unlock(&monitor->mutex);
            return true;
        }
        mtx_unlock(&monitor->mutex);
        
        if (timeout_ms >= 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + 
                             (now.tv_nsec - start.tv_nsec) / 1000000;
            if (elapsed_ms >= timeout_ms) {
                return false; // Timeout
            }
        }
        
        usleep(1000); // Sleep 1ms between checks
    }
}

bool pwm_monitor_is_running(PWMMonitor *monitor) {
    if (!monitor) return false;
    return monitor->running;
}

void pwm_monitor_set_avg_window_ms(PWMMonitor *monitor, int window_ms) {
    if (!monitor) return;
    if (window_ms < 10) window_ms = 10;
    if (window_ms > 5000) window_ms = 5000;
    mtx_lock(&monitor->mutex);
    monitor->avg_window_ms = window_ms;
    mtx_unlock(&monitor->mutex);
}

bool pwm_monitor_get_average(PWMMonitor *monitor, int *avg_us) {
    if (!monitor || !avg_us) return false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long window_ns;
    mtx_lock(&monitor->mutex);
    window_ns = (long long)monitor->avg_window_ms * 1000000LL;
    long long sum = 0;
    int count = 0;
    for (int i = 0; i < PWM_AVG_MAX_SAMPLES; i++) {
        struct timespec ts = monitor->samples[i].ts;
        if (ts.tv_sec == 0 && ts.tv_nsec == 0) continue;
        long long age_ns = ((long long)(now.tv_sec - ts.tv_sec) * 1000000000LL) + (now.tv_nsec - ts.tv_nsec);
        if (age_ns >= 0 && age_ns <= window_ns) {
            sum += monitor->samples[i].duration_us;
            count++;
        }
    }
    if (count == 0) {
        mtx_unlock(&monitor->mutex);
        return false;
    }
    *avg_us = (int)(sum / count);
    mtx_unlock(&monitor->mutex);
    return true;
}

bool gpio_is_initialized(void) {
    return initialized;
}
