/*
 * Landing Light Sequencer - Implementation
 * 
 * See landing_light.h for class documentation and state machine diagram.
 */

#include "landing_light.h"

// ============================================================================
// LED Helpers
// ============================================================================

void LandingLight::_ledsOn() {
    for (uint8_t i = 0; i < _ledCount; i++) {
        _leds[i]->setBrightness(_brightness);
    }
}

void LandingLight::_ledsOff() {
    for (uint8_t i = 0; i < _ledCount; i++) {
        _leds[i]->off();
    }
}

// ============================================================================
// Progress Emission
// ============================================================================

void LandingLight::_emitProgress(bool finished) {
    if (!_progressCb) return;

    LightFxLandingLightStatus status;
    status.slot = _slot;
    status.finished = finished;

    // Map internal state to wire-format phase constants
    switch (_state) {
        case LandingLightState::RETRACTED:  status.phase = LandingLightPhase::RETRACTED;  break;
        case LandingLightState::DEPLOYING:  status.phase = LandingLightPhase::DEPLOYING;  break;
        case LandingLightState::DEPLOYED:   status.phase = LandingLightPhase::DEPLOYED;   break;
        case LandingLightState::RETRACTING: status.phase = LandingLightPhase::RETRACTING; break;
        default:                            status.phase = LandingLightPhase::RETRACTED;  break;
    }

    _progressCb(status);
}

// ============================================================================
// Configuration
// ============================================================================

void LandingLight::configure(ServoControl* servo, LedControl** leds,
                             uint8_t ledCount, uint8_t brightness) {
    _servo = servo;  // May be nullptr (no servo group)
    _ledCount = (ledCount > MAX_LEDS) ? MAX_LEDS : ledCount;
    for (uint8_t i = 0; i < _ledCount; i++) {
        _leds[i] = leds[i];
    }
    _brightness = brightness;

    // Start in retracted state
    _ledsOff();
    if (_servo) {
        _servo->setTarget(_servo->closePosition());
    }
    _state = LandingLightState::RETRACTED;
}

void LandingLight::unconfigure() {
    if (_state == LandingLightState::UNCONFIGURED) return;
    _ledsOff();
    _servo = nullptr;
    _ledCount = 0;
    for (uint8_t i = 0; i < MAX_LEDS; i++) {
        _leds[i] = nullptr;
    }
    _state = LandingLightState::UNCONFIGURED;
}

// ============================================================================
// Actions
// ============================================================================

void LandingLight::deploy() {
    if (_state == LandingLightState::UNCONFIGURED) return;
    if (_state == LandingLightState::DEPLOYED ||
        _state == LandingLightState::DEPLOYING) return;

    if (_servo) {
        // Servo present: move to open, lights on when arrived (in update())
        _servo->setTarget(_servo->openPosition());
        _state = LandingLightState::DEPLOYING;
        _emitProgress();  // Deploying phase started
    } else {
        // No servo: lights on immediately
        _ledsOn();
        _state = LandingLightState::DEPLOYED;
        _emitProgress(true);  // Deploy complete (instant)
    }
}

void LandingLight::retract() {
    if (_state == LandingLightState::UNCONFIGURED) return;
    if (_state == LandingLightState::RETRACTED ||
        _state == LandingLightState::RETRACTING) return;

    // Lights off BEFORE retraction starts
    _ledsOff();

    if (_servo) {
        _servo->setTarget(_servo->closePosition());
        _state = LandingLightState::RETRACTING;
        _emitProgress();  // Retracting phase started
    } else {
        // No servo: instant retract
        _state = LandingLightState::RETRACTED;
        _emitProgress(true);  // Retract complete (instant)
    }
}

// ============================================================================
// Update
// ============================================================================

void LandingLight::update() {
    if (!_servo) return;  // No servo = no motion to track

    switch (_state) {
        case LandingLightState::DEPLOYING:
            if (_servo->atTarget()) {
                _ledsOn();
                _state = LandingLightState::DEPLOYED;
                _emitProgress(true);  // Deploy complete
            }
            break;

        case LandingLightState::RETRACTING:
            if (_servo->atTarget()) {
                _state = LandingLightState::RETRACTED;
                _emitProgress(true);  // Retract complete
            }
            break;

        default:
            break;
    }
}

void LandingLight::shutdown() {
    if (_state == LandingLightState::UNCONFIGURED) return;
    _ledsOff();
    _state = LandingLightState::RETRACTED;
}
