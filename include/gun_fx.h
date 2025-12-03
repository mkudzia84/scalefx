#ifndef GUN_FX_H
#define GUN_FX_H

#include <stdbool.h>

// Forward declarations
typedef struct AudioMixer AudioMixer;
typedef struct Sound Sound;
typedef struct Led Led;
typedef struct SmokeGenerator SmokeGenerator;

// Gun FX controller
typedef struct GunFX GunFX;

// Rate of fire configuration
typedef struct {
    int rounds_per_minute;      // Firing rate (RPM)
    Sound *sound;               // Sound to play for this rate (optional)
    int pwm_threshold_us;       // PWM threshold for this rate (microseconds)
} RateOfFire;

/**
 * Create a new gun FX controller
 * @param mixer Audio mixer handle (can be NULL if no audio)
 * @param audio_channel Audio channel to use for gun sounds
 * @param trigger_pwm_pin GPIO pin for PWM trigger input
 * @param smoke_heater_toggle_pin GPIO pin for smoke heater on/off toggle input (-1 to disable)
 * @param nozzle_flash_pin GPIO pin for nozzle flash LED output (-1 to disable)
 * @param smoke_fan_pin GPIO pin for smoke fan output (-1 to disable)
 * @param smoke_heater_pin GPIO pin for smoke heater output (-1 to disable)
 * @return GunFX handle, or NULL on failure
 */
GunFX* gun_fx_create(AudioMixer *mixer, int audio_channel,
                     int trigger_pwm_pin,
                     int smoke_heater_toggle_pin,
                     int nozzle_flash_pin,
                     int smoke_fan_pin,
                     int smoke_heater_pin);

/**
 * Destroy gun FX controller
 * @param gun GunFX handle
 */
void gun_fx_destroy(GunFX *gun);

/**
 * Set rates of fire (replaces all existing rates)
 * @param gun GunFX handle
 * @param rates Array of RateOfFire configurations
 * @param count Number of rates in array
 * @return 0 on success, -1 on failure
 */
int gun_fx_set_rates_of_fire(GunFX *gun, const RateOfFire *rates, int count);

/**
 * Get current firing rate (rounds per minute)
 * @param gun GunFX handle
 * @return Current RPM, or 0 if not firing
 */
int gun_fx_get_current_rpm(GunFX *gun);

/**
 * Check if gun is currently firing
 * @param gun GunFX handle
 * @return true if firing, false otherwise
 */
bool gun_fx_is_firing(GunFX *gun);

/**
 * Set smoke fan off delay
 * @param gun GunFX handle
 * @param delay_ms Delay in milliseconds before turning smoke fan off after firing stops (0 = immediate)
 */
void gun_fx_set_smoke_fan_off_delay(GunFX *gun, int delay_ms);

#endif // GUN_FX_H
