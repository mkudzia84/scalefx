#ifndef GPIO_H
#define GPIO_H

#include <stdbool.h>
#include <threads.h>

// Forward declarations
typedef struct PWMMonitor PWMMonitor;

// PWM reading structure
typedef struct {
    int pin;                // GPIO pin number
    int duration_us;        // Pulse duration in microseconds
} PWMReading;

// Callback function type for PWM readings
typedef void (*PWMCallback)(PWMReading reading, void *user_data);

// GPIO pin modes
typedef enum {
    GPIO_MODE_INPUT,
    GPIO_MODE_OUTPUT
} GPIOMode;

// GPIO pull-up/pull-down resistor configuration
typedef enum {
    GPIO_PULL_OFF,      // No pull-up or pull-down
    GPIO_PULL_DOWN,     // Enable pull-down resistor
    GPIO_PULL_UP        // Enable pull-up resistor
} GPIOPull;

/**
 * Initialize GPIO subsystem
 * @return 0 on success, -1 on error
 */
int gpio_init(void);

/**
 * Cleanup GPIO subsystem
 */
void gpio_cleanup(void);

/**
 * Set GPIO pin mode
 * @param pin GPIO pin number (BCM numbering)
 * @param mode Pin mode (input or output)
 * @return 0 on success, -1 on error
 */
int gpio_set_mode(int pin, GPIOMode mode);

/**
 * Set pull-up/pull-down resistor configuration
 * @param pin GPIO pin number
 * @param pull Pull configuration
 * @return 0 on success, -1 on error
 */
int gpio_set_pull(int pin, GPIOPull pull);

/**
 * Write digital value to GPIO pin
 * @param pin GPIO pin number
 * @param value true for HIGH, false for LOW
 * @return 0 on success, -1 on error
 */
int gpio_write(int pin, bool value);

/**
 * Read digital value from GPIO pin
 * @param pin GPIO pin number
 * @return true for HIGH, false for LOW, or false on error
 */
bool gpio_read(int pin);

// ============================================================================
// ASYNC PWM MONITOR API
// ============================================================================

/**
 * Create a PWM monitor for asynchronous pulse width monitoring
 * @param pin GPIO pin number to monitor
 * @param callback Callback function called for each pulse (optional, can be nullptr)
 * @param user_data User data passed to callback
 * @return PWMMonitor handle or nullptr on error
 */
PWMMonitor* pwm_monitor_create(int pin, PWMCallback callback, void *user_data);

/**
 * Create a PWM monitor with feature name for informative logging
 * @param pin GPIO pin number to monitor
 * @param feature_name Name of the feature (e.g., "Trigger", "Pitch Servo") for logging
 * @param callback Callback function called for each pulse (optional, can be nullptr)
 * @param user_data User data passed to callback
 * @return PWMMonitor handle or nullptr on error
 */
PWMMonitor* pwm_monitor_create_with_name(int pin, const char *feature_name, PWMCallback callback, void *user_data);

/**
 * Destroy PWM monitor and stop monitoring thread
 * @param monitor PWM monitor handle
 */
void pwm_monitor_destroy(PWMMonitor *monitor);

/**
 * Start PWM monitoring
 * @param monitor PWM monitor handle
 * @return 0 on success, -1 on error
 */
int pwm_monitor_start(PWMMonitor *monitor);

/**
 * Stop PWM monitoring
 * @param monitor PWM monitor handle
 * @return 0 on success, -1 on error
 */
int pwm_monitor_stop(PWMMonitor *monitor);

/**
 * Get the latest PWM reading (non-blocking)
 * @param monitor PWM monitor handle
 * @param reading Pointer to PWMReading structure to fill
 * @return true if reading available, false if no new reading
 */
bool pwm_monitor_get_reading(PWMMonitor *monitor, PWMReading *reading);

/**
 * Wait for next PWM reading (blocking)
 * @param monitor PWM monitor handle
 * @param reading Pointer to PWMReading structure to fill
 * @param timeout_ms Timeout in milliseconds (-1 for infinite)
 * @return true if reading received, false if timeout
 */
bool pwm_monitor_wait_reading(PWMMonitor *monitor, PWMReading *reading, int timeout_ms);

/**
 * Check if PWM monitor is currently running
 * @param monitor PWM monitor handle
 * @return true if running, false otherwise
 */
bool pwm_monitor_is_running(PWMMonitor *monitor);

/**
 * Check if GPIO subsystem is initialized
 * @return true if initialized, false otherwise
 */
bool gpio_is_initialized(void);

/**
 * Set averaging window for PWM readings used by average APIs.
 * @param monitor PWM monitor handle
 * @param window_ms Averaging window in milliseconds (default 200ms)
 */
void pwm_monitor_set_avg_window_ms(PWMMonitor *monitor, int window_ms);

/**
 * Get average PWM duration over the monitor's configured window.
 * @param monitor PWM monitor handle
 * @param avg_us Out parameter for average in microseconds
 * @return true if average computed from at least 1 sample, false otherwise
 */
bool pwm_monitor_get_average(PWMMonitor *monitor, int *avg_us);


#endif // GPIO_H
