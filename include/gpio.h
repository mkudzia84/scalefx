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

// GPIO edge detection for interrupt handling
typedef enum {
    GPIO_EDGE_NONE,     // No edge detection
    GPIO_EDGE_RISING,   // Detect rising edge (0->1)
    GPIO_EDGE_FALLING,  // Detect falling edge (1->0)
    GPIO_EDGE_BOTH      // Detect both edges
} GPIOEdge;

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

/**
 * Toggle GPIO pin state
 * @param pin GPIO pin number
 * @return 0 on success, -1 on error
 */
int gpio_toggle(int pin);

/**
 * Set edge detection for interrupt handling
 * @param pin GPIO pin number
 * @param edge Edge type to detect
 * @return 0 on success, -1 on error
 */
int gpio_set_edge(int pin, GPIOEdge edge);

/**
 * Wait for edge detection on a GPIO pin (blocking)
 * @param pin GPIO pin number
 * @param timeout_ms Timeout in milliseconds (-1 for infinite)
 * @return 1 if edge detected, 0 if timeout, -1 on error
 */
int gpio_wait_for_edge(int pin, int timeout_ms);

/**
 * Read PWM signal duration (pulse width) from a GPIO pin
 * Measures the time the signal stays HIGH
 * @param pin GPIO pin number
 * @param timeout_us Timeout in microseconds for pulse detection
 * @return Pulse duration in microseconds, or -1 on error/timeout
 */
int gpio_read_pwm_duration(int pin, int timeout_us);

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

#endif // GPIO_H
