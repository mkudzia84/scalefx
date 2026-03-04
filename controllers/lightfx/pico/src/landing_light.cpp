/*
 * Landing Light Sequencer - Implementation
 * 
 * See landing_light.h for class documentation and state machine diagram.
 */

#include "landing_light.h"

// ============================================================================
// Configuration
// ============================================================================

void LandingLight::configure(ServoControl* servo, LedControl* led,
                             uint16_t deployUs, uint16_t retractUs,
                             uint8_t brightness) {
    _servo = servo;
    _led = led;
    _deployUs = deployUs;
    _retractUs = retractUs;
    _brightness = brightness;

    // Start in retracted state
    _led->off();
    _servo->setTarget(_retractUs);
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

    _servo->setTarget(_deployUs);
    _state = LandingLightState::DEPLOYING;
}

void LandingLight::retract() {
    if (_state == LandingLightState::UNCONFIGURED) return;
    if (_state == LandingLightState::RETRACTED ||
        _state == LandingLightState::RETRACTING) return;

    // Light off BEFORE retraction starts
    _led->off();
    _servo->setTarget(_retractUs);
    _state = LandingLightState::RETRACTING;
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
            }
            break;

        case LandingLightState::RETRACTING:
            if (_servo->atTarget()) {
                _state = LandingLightState::RETRACTED;
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
