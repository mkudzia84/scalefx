#ifndef SMOKE_GENERATOR_H
#define SMOKE_GENERATOR_H

#include <stdbool.h>

// Forward declaration
typedef struct SmokeGenerator SmokeGenerator;

/**
 * Create a new smoke generator controller
 * @param heater_pin GPIO pin number for heater control
 * @param fan_pin GPIO pin number for fan control
 * @return SmokeGenerator handle or NULL on error
 */
SmokeGenerator* smoke_generator_create(int heater_pin, int fan_pin);

/**
 * Destroy smoke generator controller and free resources
 * @param smoke SmokeGenerator handle
 */
void smoke_generator_destroy(SmokeGenerator *smoke);

/**
 * Turn heater on
 * @param smoke SmokeGenerator handle
 * @return 0 on success, -1 on error
 */
int smoke_generator_heater_on(SmokeGenerator *smoke);

/**
 * Turn heater off
 * @param smoke SmokeGenerator handle
 * @return 0 on success, -1 on error
 */
int smoke_generator_heater_off(SmokeGenerator *smoke);

/**
 * Turn fan on
 * @param smoke SmokeGenerator handle
 * @return 0 on success, -1 on error
 */
int smoke_generator_fan_on(SmokeGenerator *smoke);

/**
 * Turn fan off
 * @param smoke SmokeGenerator handle
 * @return 0 on success, -1 on error
 */
int smoke_generator_fan_off(SmokeGenerator *smoke);

/**
 * Check if heater is on
 * @param smoke SmokeGenerator handle
 * @return true if heater is on, false otherwise
 */
bool smoke_generator_is_heater_on(SmokeGenerator *smoke);

/**
 * Check if fan is on
 * @param smoke SmokeGenerator handle
 * @return true if fan is on, false otherwise
 */
bool smoke_generator_is_fan_on(SmokeGenerator *smoke);

#endif // SMOKE_GENERATOR_H
