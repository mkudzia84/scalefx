#include "gpio.h"
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <threads.h>
#include <stdatomic.h>
#include <poll.h>
#include "logging.h"

static bool initialized = false;
static struct gpiod_chip *chip = NULL;

// Single monitoring thread for all PWM pins
static thrd_t pwm_monitoring_thread;
static atomic_bool pwm_thread_running = false;
static mtx_t pwm_monitors_mutex;

#define MAX_PWM_MONITORS 8
static PWMMonitor *active_monitors[MAX_PWM_MONITORS] = {0};
static int active_monitor_count = 0;

// Audio HAT Reserved Pins - DO NOT USE
// Supports both WM8960 Audio HAT and Raspberry Pi DigiAMP+
#define AUDIO_I2C_SDA     2   // I2C Data (both HATs)
#define AUDIO_I2C_SCL     3   // I2C Clock (both HATs)
#define AUDIO_I2S_BCK     18  // I2S Bit Clock (both HATs)
#define AUDIO_I2S_LRCK    19  // I2S Left/Right Clock (both HATs)
#define AUDIO_I2S_DIN     20  // I2S Data In (both HATs)
#define AUDIO_I2S_DOUT    21  // I2S Data Out (both HATs)
#define AUDIO_SHUTDOWN    22  // DigiAMP+ shutdown control (optional)

// Check if a pin is reserved by audio HATs
// Protects pins used by WM8960 Audio HAT and Raspberry Pi DigiAMP+
static bool is_audio_hat_pin(int pin) {
    return (pin == AUDIO_I2C_SDA || pin == AUDIO_I2C_SCL ||
            pin == AUDIO_I2S_BCK || pin == AUDIO_I2S_LRCK ||
            pin == AUDIO_I2S_DIN || pin == AUDIO_I2S_DOUT ||
            pin == AUDIO_SHUTDOWN);
}

// Track GPIO line requests we've made (libgpiod v2.x uses requests)
#define MAX_LINES 32
static struct gpiod_line_request *line_requests[MAX_LINES] = {0};

int gpio_init(void) {
    if (initialized) {
        LOG_WARN(LOG_GPIO, "GPIO already initialized");
        return 0;
    }
    
    // Open GPIO chip (gpiochip0 on Raspberry Pi)
    chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) {
        LOG_ERROR(LOG_GPIO, "Failed to open GPIO chip: %s", strerror(errno));
        LOG_ERROR(LOG_GPIO, "Make sure you have permission to access /dev/gpiochip0");
        return -1;
    }
    
    // Initialize PWM monitoring mutex
    mtx_init(&pwm_monitors_mutex, mtx_plain);
    
    initialized = true;
    LOG_INFO(LOG_GPIO, "GPIO subsystem initialized using libgpiod v2.x");
    LOG_INFO(LOG_GPIO, "WM8960 Audio HAT pins (2,3,18-21) will not be used");
    return 0;
}

void gpio_cleanup(void) {
    if (!initialized) return;
    
    // Stop PWM emitting thread if running
    if (atomic_load(&emitter_thread_running)) {
        atomic_store(&emitter_thread_running, false);
        thrd_join(pwm_emitting_thread, NULL);
        LOG_INFO(LOG_GPIO, "PWM emitting thread stopped");
    }
    
    // Stop PWM monitoring thread if running
    if (atomic_load(&pwm_thread_running)) {
        atomic_store(&pwm_thread_running, false);
        thrd_join(pwm_monitoring_thread, NULL);
    }
    
    // Release all GPIO line requests
    for (int i = 0; i < MAX_LINES; i++) {
        if (line_requests[i]) {
            gpiod_line_request_release(line_requests[i]);
            line_requests[i] = NULL;
        }
    }
    
    // Destroy PWM monitoring mutex
    mtx_destroy(&pwm_monitors_mutex);
    
    // Close GPIO chip
    if (chip) {
        gpiod_chip_close(chip);
        chip = NULL;
    }
    
    initialized = false;
    LOG_INFO(LOG_GPIO, "GPIO subsystem cleaned up");
}

int gpio_set_mode(int pin, GPIOMode mode) {
    if (!initialized || !chip) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return -1;
    }
    
    // Prevent using audio HAT pins (WM8960 / DigiAMP+)
    if (is_audio_hat_pin(pin)) {
        LOG_ERROR(LOG_GPIO, "Cannot use GPIO %d - reserved for audio HAT (WM8960/DigiAMP+)!", pin);
        return -1;
    }
    
    // Release any existing request for this pin
    if (line_requests[pin]) {
        gpiod_line_request_release(line_requests[pin]);
        line_requests[pin] = NULL;
    }
    
    // Create line settings
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    if (!settings) {
        LOG_ERROR(LOG_GPIO, "Failed to create line settings");
        return -1;
    }
    
    // Set direction
    int ret = gpiod_line_settings_set_direction(settings, 
        mode == GPIO_MODE_INPUT ? GPIOD_LINE_DIRECTION_INPUT : GPIOD_LINE_DIRECTION_OUTPUT);
    if (ret < 0) {
        gpiod_line_settings_free(settings);
        LOG_ERROR(LOG_GPIO, "Failed to set direction");
        return -1;
    }
    
    // Set initial output value to low if output
    if (mode == GPIO_MODE_OUTPUT) {
        gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);
    }
    
    // Create request config
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    if (!req_cfg) {
        gpiod_line_settings_free(settings);
        LOG_ERROR(LOG_GPIO, "Failed to create request config");
        return -1;
    }
    gpiod_request_config_set_consumer(req_cfg, "helifx");
    
    // Create line config
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        gpiod_request_config_free(req_cfg);
        gpiod_line_settings_free(settings);
        LOG_ERROR(LOG_GPIO, "Failed to create line config");
        return -1;
    }
    
    unsigned int offset = (unsigned int)pin;
    ret = gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
    if (ret < 0) {
        gpiod_line_config_free(line_cfg);
        gpiod_request_config_free(req_cfg);
        gpiod_line_settings_free(settings);
        LOG_ERROR(LOG_GPIO, "Failed to add line settings");
        return -1;
    }
    
    // Request the line
    struct gpiod_line_request *request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    
    // Cleanup temporary objects
    gpiod_line_config_free(line_cfg);
    gpiod_request_config_free(req_cfg);
    gpiod_line_settings_free(settings);
    
    if (!request) {
        LOG_ERROR(LOG_GPIO, "Failed to request GPIO line %d: %s", pin, strerror(errno));
        return -1;
    }
    
    // Store the request
    line_requests[pin] = request;
    
    LOG_INFO(LOG_GPIO, "GPIO %d configured as %s", pin, 
            mode == GPIO_MODE_INPUT ? "INPUT" : "OUTPUT");
    return 0;
}

int gpio_set_pull(int pin, GPIOPull pull) {
    if (!initialized) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return -1;
    }
    
    if (is_audio_hat_pin(pin)) {
        LOG_ERROR(LOG_GPIO, "Cannot use GPIO %d - reserved for WM8960 Audio HAT!", pin);
        return -1;
    }
    
    // Note: libgpiod v2.x requires pull configuration via bias flags on request
    // For now, we'll just log a warning if not GPIO_PULL_OFF
    if (pull != GPIO_PULL_OFF) {
        LOG_WARN(LOG_GPIO, "Pull-up/down configuration not supported in libgpiod v2.x");
        LOG_WARN(LOG_GPIO, "Configure pull resistors in device tree if needed");
    }
    
    return 0;
}

int gpio_write(int pin, bool value) {
    if (!initialized) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return -1;
    }
    
    if (is_audio_hat_pin(pin)) {
        LOG_ERROR(LOG_GPIO, "Cannot use GPIO %d - reserved for WM8960 Audio HAT!", pin);
        return -1;
    }
    
    struct gpiod_line_request *request = line_requests[pin];
    if (!request) {
        LOG_ERROR(LOG_GPIO, "GPIO %d not configured (call gpio_set_mode first)", pin);
        return -1;
    }
    
    int result = gpiod_line_request_set_value(request, pin, value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
    if (result < 0) {
        LOG_ERROR(LOG_GPIO, "Failed to write GPIO %d: %s", pin, strerror(errno));
        return -1;
    }
    
    return 0;
}

bool gpio_read(int pin) {
    if (!initialized) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return false;
    }
    
    if (is_audio_hat_pin(pin)) {
        LOG_ERROR(LOG_GPIO, "Cannot use GPIO %d - reserved for WM8960 Audio HAT!", pin);
        return false;
    }
    
    struct gpiod_line_request *request = line_requests[pin];
    if (!request) {
        LOG_ERROR(LOG_GPIO, "GPIO %d not configured (call gpio_set_mode first)", pin);
        return false;
    }
    
    enum gpiod_line_value value = gpiod_line_request_get_value(request, (unsigned int)pin);
    if (value == GPIOD_LINE_VALUE_ERROR) {
        LOG_ERROR(LOG_GPIO, "Failed to read GPIO %d: %s", pin, strerror(errno));
        return false;
    }
    
    return value == GPIOD_LINE_VALUE_ACTIVE;
}

// ============================================================================
// ASYNC PWM MONITOR IMPLEMENTATION (using libgpiod edge detection)
// ============================================================================

struct PWMMonitor {
    int pin;
    char *feature_name;
    struct gpiod_line_request *line_request;
    
    atomic_bool active;
    atomic_bool has_new_reading;
    atomic_bool first_signal_received;
    
    // Current reading - written by monitoring thread, read by API
    atomic_int current_duration_us;
    atomic_int current_pin;
    
    PWMCallback callback;
    void *user_data;
    
    // Track rising edge for pulse width calculation
    struct timespec rise_time;
    atomic_bool waiting_for_fall;

    // Averaging window and ring buffer for recent readings
    atomic_int avg_window_ms;
    #define PWM_AVG_MAX_SAMPLES 128
    struct {
        atomic_int duration_us;
        struct timespec ts;
    } samples[PWM_AVG_MAX_SAMPLES];
    atomic_int sample_head;
};

// Get microseconds from timespec
static int64_t timespec_to_us(const struct timespec *ts) {
    return (int64_t)ts->tv_sec * 1000000 + ts->tv_nsec / 1000;
}

// Process edge event for a specific monitor
static void process_pwm_event(PWMMonitor *monitor, struct gpiod_edge_event *event) {
    if (gpiod_edge_event_get_event_type(event) == GPIOD_EDGE_EVENT_RISING_EDGE) {
        // Rising edge - start of pulse
        uint64_t ns = gpiod_edge_event_get_timestamp_ns(event);
        monitor->rise_time.tv_sec = ns / 1000000000;
        monitor->rise_time.tv_nsec = ns % 1000000000;
        atomic_store(&monitor->waiting_for_fall, true);
    } else if (gpiod_edge_event_get_event_type(event) == GPIOD_EDGE_EVENT_FALLING_EDGE && atomic_load(&monitor->waiting_for_fall)) {
        // Falling edge - end of pulse, calculate pulse width
        uint64_t ns = gpiod_edge_event_get_timestamp_ns(event);
        struct timespec fall_time = {
            .tv_sec = ns / 1000000000,
            .tv_nsec = ns % 1000000000
        };
        
        int64_t rise_us = timespec_to_us(&monitor->rise_time);
        int64_t fall_us = timespec_to_us(&fall_time);
        int pulse_width = (int)(fall_us - rise_us);
        
        // Sanity check: typical RC PWM is 1000-2000µs, allow 500-3000µs
        if (pulse_width >= 500 && pulse_width <= 3000) {
            if (!atomic_load(&monitor->first_signal_received)) {
                LOG_INFO(LOG_GPIO, "First PWM signal received on [%s] pin %d: %d µs",
                         monitor->feature_name ?: "Unknown", monitor->pin, pulse_width);
                atomic_store(&monitor->first_signal_received, true);
            }
            
            // Update current reading atomically
            atomic_store(&monitor->current_pin, monitor->pin);
            atomic_store(&monitor->current_duration_us, pulse_width);
            atomic_store(&monitor->has_new_reading, true);
            
            // Append to averaging buffer
            int head = atomic_load(&monitor->sample_head);
            clock_gettime(CLOCK_MONOTONIC, &monitor->samples[head].ts);
            atomic_store(&monitor->samples[head].duration_us, pulse_width);
            atomic_store(&monitor->sample_head, (head + 1) % PWM_AVG_MAX_SAMPLES);
            
            // Call user callback if set
            if (monitor->callback) {
                PWMReading reading;
                reading.pin = monitor->pin;
                reading.duration_us = pulse_width;
                monitor->callback(reading, monitor->user_data);
            }
        }
        atomic_store(&monitor->waiting_for_fall, false);
    }
}

// Single PWM monitoring thread - polls all active monitors using poll()
static int pwm_monitoring_thread_func(void *arg) {
    (void)arg;  // Unused
    
    struct pollfd fds[MAX_PWM_MONITORS];
    
    LOG_INFO(LOG_GPIO, "PWM monitoring thread started");
    
    while (atomic_load(&pwm_thread_running)) {
        // Build pollfd array from active monitors
        int nfds = 0;
        PWMMonitor *monitor_map[MAX_PWM_MONITORS] = {0};  // Map fd index to monitor
        
        mtx_lock(&pwm_monitors_mutex);
        for (int i = 0; i < MAX_PWM_MONITORS; i++) {
            if (active_monitors[i] && atomic_load(&active_monitors[i]->active)) {
                int fd = gpiod_line_request_get_fd(active_monitors[i]->line_request);
                if (fd >= 0) {
                    fds[nfds].fd = fd;
                    fds[nfds].events = POLLIN;
                    fds[nfds].revents = 0;
                    monitor_map[nfds] = active_monitors[i];
                    nfds++;
                }
            }
        }
        mtx_unlock(&pwm_monitors_mutex);
        
        if (nfds == 0) {
            // No active monitors, sleep and retry
            usleep(100000);  // 100ms
            continue;
        }
        
        // Poll with timeout
        int ret = poll(fds, nfds, 1000);  // 1 second timeout
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR(LOG_GPIO, "poll() error: %s", strerror(errno));
            break;
        }
        
        if (ret == 0) continue;  // Timeout
        
        // Check which monitors have events
        for (int i = 0; i < nfds; i++) {
            if (fds[i].revents & POLLIN) {
                PWMMonitor *monitor = monitor_map[i];
                if (!monitor) continue;
                
                // Read all pending events for this monitor
                struct gpiod_edge_event_buffer *buffer = gpiod_edge_event_buffer_new(16);
                if (!buffer) continue;
                
                int ret = gpiod_line_request_read_edge_events(monitor->line_request, buffer, 16);
                if (ret > 0) {
                    for (int j = 0; j < ret; j++) {
                        struct gpiod_edge_event *event = gpiod_edge_event_buffer_get_event(buffer, j);
                        if (event) {
                            process_pwm_event(monitor, event);
                        }
                    }
                }
                gpiod_edge_event_buffer_free(buffer);
            }
        }
    }
    
    LOG_INFO(LOG_GPIO, "PWM monitoring thread stopped");
    return 0;
}

PWMMonitor* pwm_monitor_create(int pin, PWMCallback callback, void *user_data) {
    return pwm_monitor_create_with_name(pin, nullptr, callback, user_data);
}

PWMMonitor* pwm_monitor_create_with_name(int pin, const char *feature_name, PWMCallback callback, void *user_data) {
    if (!initialized || !chip) {
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
    atomic_init(&monitor->active, false);
    atomic_init(&monitor->has_new_reading, false);
    atomic_init(&monitor->first_signal_received, false);
    atomic_init(&monitor->waiting_for_fall, false);
    atomic_init(&monitor->avg_window_ms, 200);
    atomic_init(&monitor->sample_head, 0);
    atomic_init(&monitor->current_pin, 0);
    atomic_init(&monitor->current_duration_us, 0);
    
    // Clear sample buffer and initialize atomics
    for (int i = 0; i < PWM_AVG_MAX_SAMPLES; i++) {
        atomic_init(&monitor->samples[i].duration_us, 0);
        monitor->samples[i].ts.tv_sec = 0;
        monitor->samples[i].ts.tv_nsec = 0;
    }
    
    // Create line settings for edge detection
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    if (!settings) {
        LOG_ERROR(LOG_GPIO, "Failed to create line settings");
        free(monitor->feature_name);
        free(monitor);
        return nullptr;
    }
    
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH);
    
    // Create request config
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    if (!req_cfg) {
        gpiod_line_settings_free(settings);
        free(monitor->feature_name);
        free(monitor);
        return nullptr;
    }
    gpiod_request_config_set_consumer(req_cfg, "helifx-pwm");
    
    // Create line config
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        gpiod_request_config_free(req_cfg);
        gpiod_line_settings_free(settings);
        free(monitor->feature_name);
        free(monitor);
        return nullptr;
    }
    
    unsigned int offset = (unsigned int)pin;
    int ret = gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
    if (ret < 0) {
        gpiod_line_config_free(line_cfg);
        gpiod_request_config_free(req_cfg);
        gpiod_line_settings_free(settings);
        free(monitor->feature_name);
        free(monitor);
        return nullptr;
    }
    
    // Request the line
    struct gpiod_line_request *request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    
    // Cleanup temporary objects
    gpiod_line_config_free(line_cfg);
    gpiod_request_config_free(req_cfg);
    gpiod_line_settings_free(settings);
    
    if (!request) {
        LOG_ERROR(LOG_GPIO, "Failed to request edge events for pin %d: %s", pin, strerror(errno));
        free(monitor->feature_name);
        free(monitor);
        return nullptr;
    }
    
    monitor->line_request = request;
    
    LOG_INFO(LOG_GPIO, "PWM monitor created for [%s] pin %d", 
            feature_name ?: "Unknown", pin);
    return monitor;
}

void pwm_monitor_destroy(PWMMonitor *monitor) {
    if (!monitor) return;
    
    if (atomic_load(&monitor->active)) {
        pwm_monitor_stop(monitor);
    }
    
    if (monitor->line_request) {
        gpiod_line_request_release(monitor->line_request);
    }
    
    if (monitor->feature_name) {
        free(monitor->feature_name);
    }
    
    free(monitor);
    
    LOG_INFO(LOG_GPIO, "PWM monitor destroyed");
}

int pwm_monitor_start(PWMMonitor *monitor) {
    if (!monitor) return -1;
    
    if (atomic_load(&monitor->active)) {
        LOG_WARN(LOG_GPIO, "PWM monitor already running");
        return 0;
    }
    
    // Add monitor to active list
    mtx_lock(&pwm_monitors_mutex);
    
    int slot = -1;
    for (int i = 0; i < MAX_PWM_MONITORS; i++) {
        if (!active_monitors[i]) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        mtx_unlock(&pwm_monitors_mutex);
        LOG_ERROR(LOG_GPIO, "Maximum number of PWM monitors (%d) reached", MAX_PWM_MONITORS);
        return -1;
    }
    
    active_monitors[slot] = monitor;
    active_monitor_count++;
    atomic_store(&monitor->active, true);
    
    // Start shared monitoring thread if not running
    if (!atomic_load(&pwm_thread_running)) {
        atomic_store(&pwm_thread_running, true);
        if (thrd_create(&pwm_monitoring_thread, pwm_monitoring_thread_func, NULL) != thrd_success) {
            LOG_ERROR(LOG_GPIO, "Failed to create PWM monitoring thread");
            active_monitors[slot] = NULL;
            active_monitor_count--;
            atomic_store(&monitor->active, false);
            atomic_store(&pwm_thread_running, false);
            mtx_unlock(&pwm_monitors_mutex);
            return -1;
        }
    }
    
    mtx_unlock(&pwm_monitors_mutex);
    
    LOG_INFO(LOG_GPIO, "PWM monitor started for [%s] pin %d (shared thread, %d active)", 
            monitor->feature_name ?: "Unknown", monitor->pin, active_monitor_count);
    return 0;
}

int pwm_monitor_stop(PWMMonitor *monitor) {
    if (!monitor) return -1;
    
    if (!atomic_load(&monitor->active)) {
        return 0;
    }
    
    // Remove monitor from active list
    mtx_lock(&pwm_monitors_mutex);
    
    for (int i = 0; i < MAX_PWM_MONITORS; i++) {
        if (active_monitors[i] == monitor) {
            active_monitors[i] = NULL;
            active_monitor_count--;
            break;
        }
    }
    
    atomic_store(&monitor->active, false);
    
    // If no more active monitors, stop the shared thread
    if (active_monitor_count == 0 && atomic_load(&pwm_thread_running)) {
        atomic_store(&pwm_thread_running, false);
        mtx_unlock(&pwm_monitors_mutex);
        thrd_join(pwm_monitoring_thread, NULL);
        mtx_lock(&pwm_monitors_mutex);
    }
    
    mtx_unlock(&pwm_monitors_mutex);
    
    LOG_INFO(LOG_GPIO, "PWM monitor stopped for [%s] (%d active)", 
            monitor->feature_name ?: "Unknown", active_monitor_count);
    return 0;
}

bool pwm_monitor_get_reading(PWMMonitor *monitor, PWMReading *reading) {
    if (!monitor || !reading) return false;
    
    bool has_reading = atomic_exchange(&monitor->has_new_reading, false);
    if (has_reading) {
        reading->pin = atomic_load(&monitor->current_pin);
        reading->duration_us = atomic_load(&monitor->current_duration_us);
    }
    
    return has_reading;
}

bool pwm_monitor_wait_reading(PWMMonitor *monitor, PWMReading *reading, int timeout_ms) {
    if (!monitor || !reading) return false;
    
    // With pigpio alerts, we don't have blocking wait anymore
    // This function now polls with timeout
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    while (1) {
        bool has_reading = atomic_exchange(&monitor->has_new_reading, false);
        if (has_reading) {
            reading->pin = atomic_load(&monitor->current_pin);
            reading->duration_us = atomic_load(&monitor->current_duration_us);
            return true;
        }
        
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
    return atomic_load(&monitor->active);
}

void pwm_monitor_set_avg_window_ms(PWMMonitor *monitor, int window_ms) {
    if (!monitor) return;
    if (window_ms < 10) window_ms = 10;
    if (window_ms > 5000) window_ms = 5000;
    atomic_store(&monitor->avg_window_ms, window_ms);
}

bool pwm_monitor_get_average(PWMMonitor *monitor, int *avg_us) {
    if (!monitor || !avg_us) return false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    long long window_ns = (long long)atomic_load(&monitor->avg_window_ms) * 1000000LL;
    long long sum = 0;
    int count = 0;
    
    for (int i = 0; i < PWM_AVG_MAX_SAMPLES; i++) {
        struct timespec ts = monitor->samples[i].ts;
        if (ts.tv_sec == 0 && ts.tv_nsec == 0) continue;
        long long age_ns = ((long long)(now.tv_sec - ts.tv_sec) * 1000000000LL) + (now.tv_nsec - ts.tv_nsec);
        if (age_ns >= 0 && age_ns <= window_ns) {
            sum += atomic_load(&monitor->samples[i].duration_us);
            count++;
        }
    }
    
    if (count == 0) {
        return false;
    }
    
    *avg_us = (int)(sum / count);
    return true;
}

bool gpio_is_initialized(void) {
    return initialized;
}

// ============================================================================
// SOFTWARE PWM EMITTER IMPLEMENTATION
// ============================================================================

#define MAX_PWM_EMITTERS 8
#define PWM_PERIOD_US 20000  // 50Hz = 20ms period

struct PWMEmitter {
    int pin;
    char *feature_name;
    atomic_int value_us;  // Pulse width in microseconds
    atomic_bool active;
};

// Global emitter tracking
static PWMEmitter *active_emitters[MAX_PWM_EMITTERS] = {0};
static int active_emitter_count = 0;
static mtx_t emitters_mutex;
static thrd_t pwm_emitting_thread;
static atomic_bool emitter_thread_running = false;

// PWM emitting thread - generates PWM signals for all emitters
static int pwm_emitting_thread_func(void *arg) {
    (void)arg;
    
    LOG_INFO(LOG_GPIO, "PWM emitting thread started");
    
    struct timespec cycle_start, now;
    
    while (atomic_load(&emitter_thread_running)) {
        clock_gettime(CLOCK_MONOTONIC, &cycle_start);
        
        // Phase 1: Set all active pins HIGH
        mtx_lock(&emitters_mutex);
        for (int i = 0; i < active_emitter_count; i++) {
            PWMEmitter *emitter = active_emitters[i];
            if (emitter && atomic_load(&emitter->active)) {
                int value = atomic_load(&emitter->value_us);
                if (value > 0) {
                    gpio_write(emitter->pin, 1);
                }
            }
        }
        mtx_unlock(&emitters_mutex);
        
        // Phase 2: Wait and turn off pins as their pulse width expires
        // We'll sample at 5us intervals for smooth resolution (200 positions)
        for (int elapsed_us = 0; elapsed_us < PWM_PERIOD_US; elapsed_us += 5) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t actual_elapsed_us = ((now.tv_sec - cycle_start.tv_sec) * 1000000LL) +
                                        ((now.tv_nsec - cycle_start.tv_nsec) / 1000LL);
            
            // Turn off pins whose pulse width has expired
            mtx_lock(&emitters_mutex);
            for (int i = 0; i < active_emitter_count; i++) {
                PWMEmitter *emitter = active_emitters[i];
                if (emitter && atomic_load(&emitter->active)) {
                    int value = atomic_load(&emitter->value_us);
                    if (value > 0 && actual_elapsed_us >= value) {
                        gpio_write(emitter->pin, 0);
                    }
                }
            }
            mtx_unlock(&emitters_mutex);
            
            // Sleep for 5us
            struct timespec sleep_time = {0, 5000};  // 5us
            nanosleep(&sleep_time, NULL);
        }
        
        // Ensure we complete the full PWM period
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_us = ((now.tv_sec - cycle_start.tv_sec) * 1000000LL) +
                            ((now.tv_nsec - cycle_start.tv_nsec) / 1000LL);
        
        if (elapsed_us < PWM_PERIOD_US) {
            struct timespec remaining = {
                .tv_sec = 0,
                .tv_nsec = (PWM_PERIOD_US - elapsed_us) * 1000LL
            };
            nanosleep(&remaining, NULL);
        }
    }
    
    LOG_INFO(LOG_GPIO, "PWM emitting thread stopped");
    return 0;
}

PWMEmitter* pwm_emitter_create(int pin, const char *feature_name) {
    if (!initialized) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return NULL;
    }
    
    if (pin < 0 || pin >= 28) {
        LOG_ERROR(LOG_GPIO, "Invalid GPIO pin %d for PWM emitter", pin);
        return NULL;
    }
    
    if (is_audio_hat_pin(pin)) {
        LOG_ERROR(LOG_GPIO, "Cannot use audio HAT pin %d for PWM emitter", pin);
        return NULL;
    }
    
    // Set pin as output
    if (gpio_set_mode(pin, GPIO_MODE_OUTPUT) < 0) {
        LOG_ERROR(LOG_GPIO, "Failed to set pin %d as output for PWM emitter", pin);
        return NULL;
    }
    
    // Initialize pin to LOW
    gpio_write(pin, 0);
    
    PWMEmitter *emitter = calloc(1, sizeof(PWMEmitter));
    if (!emitter) {
        LOG_ERROR(LOG_GPIO, "Failed to allocate PWM emitter");
        return NULL;
    }
    
    emitter->pin = pin;
    emitter->feature_name = strdup(feature_name ? feature_name : "unknown");
    atomic_init(&emitter->value_us, 0);
    atomic_init(&emitter->active, true);
    
    // Add to active emitters
    static bool emitters_mutex_initialized = false;
    if (!emitters_mutex_initialized) {
        mtx_init(&emitters_mutex, mtx_plain);
        emitters_mutex_initialized = true;
    }
    
    mtx_lock(&emitters_mutex);
    
    if (active_emitter_count >= MAX_PWM_EMITTERS) {
        mtx_unlock(&emitters_mutex);
        LOG_ERROR(LOG_GPIO, "Maximum number of PWM emitters reached");
        free(emitter->feature_name);
        free(emitter);
        return NULL;
    }
    
    active_emitters[active_emitter_count++] = emitter;
    
    // Start emitting thread if not already running
    if (!atomic_load(&emitter_thread_running)) {
        atomic_store(&emitter_thread_running, true);
        if (thrd_create(&pwm_emitting_thread, pwm_emitting_thread_func, NULL) != thrd_success) {
            LOG_ERROR(LOG_GPIO, "Failed to create PWM emitting thread");
            active_emitter_count--;
            atomic_store(&emitter_thread_running, false);
            mtx_unlock(&emitters_mutex);
            free(emitter->feature_name);
            free(emitter);
            return NULL;
        }
        LOG_INFO(LOG_GPIO, "PWM emitting thread created");
    }
    
    mtx_unlock(&emitters_mutex);
    
    LOG_INFO(LOG_GPIO, "Created PWM emitter '%s' on pin %d", feature_name, pin);
    return emitter;
}

void pwm_emitter_destroy(PWMEmitter *emitter) {
    if (!emitter) return;
    
    atomic_store(&emitter->active, false);
    
    // Set pin LOW
    gpio_write(emitter->pin, 0);
    
    // Remove from active emitters
    mtx_lock(&emitters_mutex);
    
    for (int i = 0; i < active_emitter_count; i++) {
        if (active_emitters[i] == emitter) {
            // Shift remaining emitters
            for (int j = i; j < active_emitter_count - 1; j++) {
                active_emitters[j] = active_emitters[j + 1];
            }
            active_emitters[--active_emitter_count] = NULL;
            break;
        }
    }
    
    // Stop emitting thread if no more emitters
    if (active_emitter_count == 0 && atomic_load(&emitter_thread_running)) {
        atomic_store(&emitter_thread_running, false);
        mtx_unlock(&emitters_mutex);
        thrd_join(pwm_emitting_thread, NULL);
        LOG_INFO(LOG_GPIO, "PWM emitting thread stopped (no active emitters)");
    } else {
        mtx_unlock(&emitters_mutex);
    }
    
    LOG_INFO(LOG_GPIO, "Destroyed PWM emitter '%s' on pin %d", emitter->feature_name, emitter->pin);
    
    free(emitter->feature_name);
    free(emitter);
}

int pwm_emitter_set_value(PWMEmitter *emitter, int value_us) {
    if (!emitter) return -1;
    
    if (value_us < 0 || value_us > PWM_PERIOD_US) {
        LOG_WARN(LOG_GPIO, "PWM value %d us out of range (0-%d)", value_us, PWM_PERIOD_US);
        value_us = value_us < 0 ? 0 : PWM_PERIOD_US;
    }
    
    atomic_store(&emitter->value_us, value_us);
    return 0;
}

int pwm_emitter_get_value(PWMEmitter *emitter) {
    if (!emitter) return -1;
    return atomic_load(&emitter->value_us);
}
