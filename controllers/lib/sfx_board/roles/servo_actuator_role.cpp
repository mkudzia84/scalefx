/*
 * ServoActuatorRole implementation.
 */

#include "servo_actuator_role.h"

namespace sfx_core {

void ServoActuatorRole::setLimits(uint16_t minUs, uint16_t maxUs) {
    if (minUs >= maxUs) return;
    if (_port) {
        const uint16_t hwMin = _port->minMicroseconds();
        const uint16_t hwMax = _port->maxMicroseconds();
        if (minUs < hwMin) minUs = hwMin;
        if (maxUs > hwMax) maxUs = hwMax;
    }
    _minUs = minUs;
    _maxUs = maxUs;
    if (_target < _minUs)    _target = _minUs;
    if (_target > _maxUs)    _target = _maxUs;
    if (_commanded < _minUs) _commanded = _minUs;
    if (_commanded > _maxUs) _commanded = _maxUs;
}

void ServoActuatorRole::setTarget(uint16_t target_us) {
    if (target_us < _minUs) target_us = _minUs;
    if (target_us > _maxUs) target_us = _maxUs;
    _target = target_us;
    if (_target != _commanded) _wasAtTarget = false;
}

void ServoActuatorRole::setPositionImmediate(uint16_t pos_us) {
    if (pos_us < _minUs) pos_us = _minUs;
    if (pos_us > _maxUs) pos_us = _maxUs;
    _commanded = pos_us;
    _target    = pos_us;
    _velocity_us_per_s = 0;
    _wasAtTarget = true;
    if (_port) _port->writeMicroseconds(pos_us);
}

void ServoActuatorRole::tick() {
    if (!_port) return;
    const uint32_t now = millis();
    if (_lastTickMs == 0) { _lastTickMs = now; }
    const uint32_t dt_ms = now - _lastTickMs;
    _lastTickMs = now;

    if (_commanded == _target) {
        _velocity_us_per_s = 0;
        if (!_wasAtTarget) {
            _wasAtTarget = true;
            if (_onTargetReached) _onTargetReached(_commanded);
        }
        return;
    }

    // Linear step toward target, clamped by max velocity.
    const int32_t maxStep = (int32_t)_maxVelocity_us_per_s * (int32_t)dt_ms / 1000;
    int32_t delta = (int32_t)_target - (int32_t)_commanded;
    if (delta >  maxStep) delta =  maxStep;
    if (delta < -maxStep) delta = -maxStep;

    _commanded = (uint16_t)((int32_t)_commanded + delta);
    _velocity_us_per_s = (dt_ms > 0)
        ? (int16_t)((delta * 1000) / (int32_t)dt_ms)
        : (int16_t)0;
    _port->writeMicroseconds(_commanded);
}

}  // namespace sfx_core
