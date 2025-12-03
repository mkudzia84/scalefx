#ifndef LIGHTS_H
#define LIGHTS_H

#include <stdbool.h>

// Forward declaration
typedef struct Led Led;

/**
 * Create a new LED controller
 * @param pin GPIO pin number for LED output
 * @return Led handle or NULL on error
 */
Led* led_create(int pin);

/**
 * Destroy LED controller and free resources
 * @param led Led handle
 */
void led_destroy(Led *led);

/**
 * Turn LED on (solid)
 * @param led Led handle
 * @return 0 on success, -1 on error
 */
int led_on(Led *led);

/**
 * Turn LED off
 * @param led Led handle
 * @return 0 on success, -1 on error
 */
int led_off(Led *led);

/**
 * Set LED to blink mode
 * @param led Led handle
 * @param interval_ms Blink interval in milliseconds (time for one complete on/off cycle)
 * @return 0 on success, -1 on error
 */
int led_blink(Led *led, int interval_ms);

/**
 * Check if LED is currently on (regardless of blink mode)
 * @param led Led handle
 * @return true if LED is on, false otherwise
 */
bool led_is_on(Led *led);

/**
 * Check if LED is in blink mode
 * @param led Led handle
 * @return true if blinking, false otherwise
 */
bool led_is_blinking(Led *led);

#endif // LIGHTS_H
