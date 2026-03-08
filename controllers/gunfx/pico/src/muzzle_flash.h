/*
 * Muzzle Flash Module - Header
 *
 * Encapsulates the muzzle flash LED effect with PWM pulse and fade-out.
 * Each shot produces a full-brightness pulse followed by a smooth fade.
 *
 * The module also manages per-shot recoil jerk on attached servos —
 * jerk is applied on flash trigger and cleared after fade completes.
 *
 * Usage:
 *   MuzzleFlash flash;
 *   flash.begin(PIN_NOZZLE_FLASH);
 *   flash.setRecoilJerk(0, 50, 25);  // servo 0: ±50-75µs per shot
 *
 *   // Start/stop firing:
 *   flash.startFiring(600);   // 600 RPM
 *   flash.stopFiring();
 *
 *   // In loop:
 *   flash.update();
 */

#ifndef MUZZLE_FLASH_H
#define MUZZLE_FLASH_H

#include <Arduino.h>
#include <srv_control.h>
#include <functional>

// ============================================================================
// Constants
// ============================================================================

namespace MuzzleFlashConfig {
    constexpr uint8_t  PWM_DUTY       = 255;    // Full brightness
    constexpr uint16_t PULSE_MS       = 30;     // Duration at full brightness
    constexpr uint16_t FADE_MS        = 80;     // Fade-out duration
    constexpr uint8_t  JERK_SERVO_COUNT = 3;    // Max servos for recoil jerk
    constexpr uint16_t RPM_MIN        = 1;
    constexpr uint16_t RPM_MAX        = 3000;
}

// ============================================================================
// Recoil Jerk Configuration
// ============================================================================

struct RecoilJerkConfig {
    int16_t jerk_us     = 0;   // Base jerk displacement in µs
    int16_t variance_us = 0;   // Random variance added to jerk in µs
};

// ============================================================================
// MuzzleFlash Class
// ============================================================================

/**
 * @brief Muzzle flash LED controller with recoil jerk effect.
 *
 * Manages the firing cycle: flash pulse → fade → wait → next shot.
 * On each shot, applies random recoil jerk to attached servos and
 * fires the onShot callback for external coordination (e.g., smoke puff).
 */
class MuzzleFlash {
public:
    // Shot fired callback — called on each flash trigger
    using ShotCallback = std::function<void()>;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize the muzzle flash module.
     * @param pin       GPIO pin for the flash LED (PWM capable)
     */
    void begin(uint8_t pin);

    // ========================================================================
    // Servo Registration
    // ========================================================================

    /**
     * @brief Attach a servo for recoil jerk effects.
     * @param index     Servo slot (0-2)
     * @param servo     Pointer to the ServoControl instance
     */
    void attachServo(uint8_t index, ServoControl* servo);

    // ========================================================================
    // Firing Control
    // ========================================================================

    /**
     * @brief Start firing at the specified RPM.
     * @param rpm       Firing rate in rounds per minute (1-3000)
     */
    void startFiring(uint16_t rpm);

    /**
     * @brief Stop firing immediately (flash off, jerk cleared).
     */
    void stopFiring();

    /**
     * @brief Update flash state (call every loop iteration).
     */
    void update();

    // ========================================================================
    // Recoil Jerk Configuration
    // ========================================================================

    /**
     * @brief Configure recoil jerk for a servo slot.
     * @param index        Servo slot (0-2)
     * @param jerk_us      Base jerk displacement in µs
     * @param variance_us  Random variance added to jerk in µs
     */
    void setRecoilJerk(uint8_t index, int16_t jerk_us, int16_t variance_us);

    // ========================================================================
    // Callbacks
    // ========================================================================

    /**
     * @brief Register callback fired on each shot (for smoke puff sync, etc.).
     */
    void onShot(ShotCallback cb) { _onShot = cb; }

    // ========================================================================
    // State Queries
    // ========================================================================

    bool isFiring()     const { return _firing; }
    bool isFlashOn()    const { return _flashActive; }
    bool isFading()     const { return _flashFading; }
    uint16_t rpm()      const { return _rpm; }
    uint32_t shotsFired() const { return _totalShots; }

    const RecoilJerkConfig& recoilJerk(uint8_t index) const {
        return _jerkConfigs[index < MuzzleFlashConfig::JERK_SERVO_COUNT ? index : 0];
    }

private:
    // Hardware
    uint8_t _pin = 0;

    // Servo references for recoil jerk
    ServoControl* _servos[MuzzleFlashConfig::JERK_SERVO_COUNT] = { nullptr };
    RecoilJerkConfig _jerkConfigs[MuzzleFlashConfig::JERK_SERVO_COUNT];

    // Firing state
    bool     _firing       = false;
    uint16_t _rpm          = 0;
    uint32_t _shotInterval_ms = 0;
    uint32_t _nextShotTime_ms = 0;

    // Flash state
    bool     _flashActive  = false;
    bool     _flashFading  = false;
    uint32_t _flashOffTime_ms  = 0;
    uint32_t _fadeStartTime_ms = 0;

    // Metrics
    uint32_t _totalShots = 0;

    // Callbacks
    ShotCallback _onShot;

    // Internal helpers
    void _setFlashPwm(uint8_t duty);
    void _applyRecoilJerk();
    void _clearRecoilJerk();
};

#endif // MUZZLE_FLASH_H
