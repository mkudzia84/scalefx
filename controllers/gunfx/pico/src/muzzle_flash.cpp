/*
 * Muzzle Flash Module - Implementation
 *
 * PWM flash pulse with fade-out and per-shot recoil jerk on servos.
 */

#include "muzzle_flash.h"

using namespace MuzzleFlashConfig;

// ============================================================================
// Initialization
// ============================================================================

void MuzzleFlash::begin(uint8_t pin) {
    _pin = pin;
    pinMode(_pin, OUTPUT);
    _setFlashPwm(0);
}

// ============================================================================
// Servo Registration
// ============================================================================

void MuzzleFlash::attachServo(uint8_t index, ServoControl* servo) {
    if (index < JERK_SERVO_COUNT) {
        _servos[index] = servo;
    }
}

// ============================================================================
// Firing Control
// ============================================================================

void MuzzleFlash::startFiring(uint16_t rpm) {
    if (rpm < RPM_MIN || rpm > RPM_MAX) return;
    _firing = true;
    _rpm = rpm;
    _shotInterval_ms = 60000UL / rpm;
    _nextShotTime_ms = millis();
}

void MuzzleFlash::stopFiring() {
    _firing = false;
    _rpm = 0;
    _setFlashPwm(0);
    _flashActive = false;
    _flashFading = false;
    _clearRecoilJerk();
}

// ============================================================================
// Update (call every loop)
// ============================================================================

void MuzzleFlash::update() {
    if (!_firing) {
        if (_flashActive || _flashFading) {
            _setFlashPwm(0);
            _flashActive = false;
            _flashFading = false;
            _clearRecoilJerk();
        }
        return;
    }

    uint32_t now = millis();

    if (_flashFading) {
        // Fade-out phase
        uint32_t fadeElapsed = now - _fadeStartTime_ms;
        if (fadeElapsed >= FADE_MS) {
            _setFlashPwm(0);
            _flashFading = false;
            _clearRecoilJerk();
        } else {
            uint16_t brightness = map(fadeElapsed, 0, FADE_MS, PWM_DUTY, 0);
            _setFlashPwm((uint8_t)brightness);
        }
    } else if (_flashActive) {
        // Full-brightness pulse phase
        if (now >= _flashOffTime_ms) {
            _flashActive = false;
            _flashFading = true;
            _fadeStartTime_ms = now;
        }
    } else if (now >= _nextShotTime_ms) {
        // New shot — trigger flash
        _setFlashPwm(PWM_DUTY);
        _flashActive = true;
        _flashOffTime_ms = now + PULSE_MS;
        _nextShotTime_ms = now + _shotInterval_ms;
        _applyRecoilJerk();
        _totalShots++;

        // Notify external listeners (e.g., smoke puff)
        if (_onShot) _onShot();
    }
}

// ============================================================================
// Recoil Jerk Configuration
// ============================================================================

void MuzzleFlash::setRecoilJerk(uint8_t index, int16_t jerk_us, int16_t variance_us) {
    if (index < JERK_SERVO_COUNT) {
        _jerkConfigs[index].jerk_us = jerk_us;
        _jerkConfigs[index].variance_us = variance_us;
    }
}

// ============================================================================
// Internal Helpers
// ============================================================================

void MuzzleFlash::_setFlashPwm(uint8_t duty) {
    analogWrite(_pin, duty);
}

void MuzzleFlash::_applyRecoilJerk() {
    for (uint8_t i = 0; i < JERK_SERVO_COUNT; i++) {
        if (!_servos[i]) continue;
        RecoilJerkConfig* jerk = &_jerkConfigs[i];
        if (jerk->jerk_us == 0) {
            _servos[i]->clearJerk();
        } else {
            int direction = (random(2) == 0) ? 1 : -1;
            int variance = jerk->variance_us > 0 ? random(jerk->variance_us + 1) : 0;
            _servos[i]->applyJerk(direction * (jerk->jerk_us + variance));
        }
    }
}

void MuzzleFlash::_clearRecoilJerk() {
    for (uint8_t i = 0; i < JERK_SERVO_COUNT; i++) {
        if (_servos[i]) _servos[i]->clearJerk();
    }
}
