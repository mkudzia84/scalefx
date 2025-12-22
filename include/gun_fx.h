#ifndef GUN_FX_H
#define GUN_FX_H

#include <stdbool.h>

// Forward declarations
typedef struct AudioMixer AudioMixer;
typedef struct Sound Sound;
typedef struct GunFXConfig GunFXConfig;

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
 * @param mixer Audio mixer handle (can be nullptr if no audio)
 * @param audio_channel Audio channel to use for gun sounds
 * @param config Gun FX configuration
 * @return GunFX handle, or nullptr on failure
 */
GunFX* gun_fx_create(AudioMixer *mixer, int audio_channel,
                     const GunFXConfig *config);

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
 * Get current selected rate index
 * @param gun GunFX handle
 * @return Selected rate index (0-based), or -1 if no rate selected
 */
int gun_fx_get_current_rate_index(GunFX *gun);

/**
 * Check if gun is currently firing
 * @param gun GunFX handle
 * @return true if firing, false otherwise
 */
bool gun_fx_is_firing(GunFX *gun);

// Getter functions for status display
int gun_fx_get_trigger_pwm(GunFX *gun);
int gun_fx_get_trigger_pin(GunFX *gun);
int gun_fx_get_heater_toggle_pwm(GunFX *gun);
int gun_fx_get_heater_toggle_pin(GunFX *gun);
bool gun_fx_get_heater_state(GunFX *gun);
int gun_fx_get_pitch_pwm(GunFX *gun);
int gun_fx_get_pitch_pin(GunFX *gun);
int gun_fx_get_yaw_pwm(GunFX *gun);
int gun_fx_get_yaw_pin(GunFX *gun);

// Recoil jerk settings getters
int gun_fx_get_pitch_recoil_jerk(GunFX *gun);
int gun_fx_get_pitch_recoil_jerk_variance(GunFX *gun);
int gun_fx_get_yaw_recoil_jerk(GunFX *gun);
int gun_fx_get_yaw_recoil_jerk_variance(GunFX *gun);

#endif // GUN_FX_H
