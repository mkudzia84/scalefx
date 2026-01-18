#ifndef STATUS_H
#define STATUS_H

// Forward declarations
typedef struct GunFX GunFX;
typedef struct EngineFX EngineFX;

// Status display controller
typedef struct StatusDisplay StatusDisplay;

/**
 * Create a new status display controller
 * @param gun GunFX handle (can be nullptr if not used)
 * @param engine EngineFX handle (can be nullptr if not used)
 * @param interval_ms Display refresh interval in milliseconds (default: 100)
 * @return StatusDisplay handle, or nullptr on failure
 */
StatusDisplay* status_display_create(GunFX *gun, EngineFX *engine, int interval_ms);

/**
 * Destroy status display controller
 * @param status StatusDisplay handle
 */
void status_display_destroy(StatusDisplay *status);

/**
 * Trigger an immediate status display (does not affect timer)
 * @param status StatusDisplay handle
 */
void status_display_print_now(StatusDisplay *status);

#endif // STATUS_H
