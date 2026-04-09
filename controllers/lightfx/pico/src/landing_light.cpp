/*
 * Landing Light Sequencer - Implementation
 * 
 * See landing_light.h for class documentation and state machine diagram.
 */

#include "landing_light.h"

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

void LandingLight::configure(ServoControl* servo, LedControl* led,
                             uint8_t brightness) {
    _servo = servo;
    _led = led;
    _brightness = brightness;

    // Start in retracted state (close position)
    _led->off();
    _servo->setTarget(_servo->closePosition());
    _state = LandingLightState::RETRACTED;
}

void LandingLight::unconfigure() {
    if (_state == LandingLightState::UNCONFIGURED) return;
    _led->off();
    _servo = nullptr;
    _led = nullptr;
    _state = LandingLightState::UNCONFIGURED;
}

// ============================================================================
// Actions
// ============================================================================

void LandingLight::deploy() {
    if (_state == LandingLightState::UNCONFIGURED) return;
    if (_state == LandingLightState::DEPLOYED ||
        _state == LandingLightState::DEPLOYING) return;

    _servo->setTarget(_servo->openPosition());
    _state = LandingLightState::DEPLOYING;
    _emitProgress();  // Deploying phase started
}

void LandingLight::retract() {
    if (_state == LandingLightState::UNCONFIGURED) return;
    if (_state == LandingLightState::RETRACTED ||
        _state == LandingLightState::RETRACTING) return;

    // Light off BEFORE retraction starts
    _led->off();
    _servo->setTarget(_servo->closePosition());
    _state = LandingLightState::RETRACTING;
    _emitProgress();  // Retracting phase started
}

// ============================================================================
// Update
// ============================================================================

void LandingLight::update() {
    if (!_servo) return;

    switch (_state) {
        case LandingLightState::DEPLOYING:
            if (_servo->atTarget()) {
                _led->setBrightness(_brightness);
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
    _led->off();
    _state = LandingLightState::RETRACTED;
}
