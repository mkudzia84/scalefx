/*
 * Servo Control Library - Implementation
 * 
 * Object-oriented servo output control with motion profiling.
 */

#include "srv_control.h"
#include <math.h>

// ============================================================================
// Constructor / Destructor
// ============================================================================

ServoControl::~ServoControl() {
    end();
}

// ============================================================================
// Initialization
// ============================================================================

bool ServoControl::begin(int pin, int minUs, int maxUs, int initialUs) {
    if (pin < 0) {
        return false;
    }
    
    // Validate limits
    if (minUs < ServoControlConfig::ABSOLUTE_MIN_US) {
        minUs = ServoControlConfig::ABSOLUTE_MIN_US;
    }
    if (maxUs > ServoControlConfig::ABSOLUTE_MAX_US) {
        maxUs = ServoControlConfig::ABSOLUTE_MAX_US;
    }
    if (minUs >= maxUs) {
        return false;
    }
    
    // End any previous attachment
    end();
    
    _pin = pin;
    _minUs = minUs;
    _maxUs = maxUs;
    
    // Clamp initial position
    if (initialUs < _minUs) initialUs = _minUs;
    if (initialUs > _maxUs) initialUs = _maxUs;
    
    _positionUs = (float)initialUs;
    _targetUs = (float)initialUs;
    _velocityUsPerS = 0.0f;
    _lastUpdateMs = 0;
    _wasMoving = false;
    _jerkOffset = 0;
    _lastCallbackPosition = initialUs;
    
    // Attach servo
    _servo.attach(_pin, _minUs, _maxUs);
    _servo.writeMicroseconds(initialUs);
    _attached = true;
    
    return true;
}

void ServoControl::end() {
    if (_attached) {
        _servo.detach();
        _attached = false;
    }
    _pin = -1;
    _velocityUsPerS = 0.0f;
    _jerkOffset = 0;
    _positionCallback = nullptr;
    _targetReachedCallback = nullptr;
}

// ============================================================================
// Configuration
// ============================================================================

void ServoControl::setLimits(int minUs, int maxUs) {
    // Enforce absolute limits
    if (minUs < ServoControlConfig::ABSOLUTE_MIN_US) {
        minUs = ServoControlConfig::ABSOLUTE_MIN_US;
    }
    if (maxUs > ServoControlConfig::ABSOLUTE_MAX_US) {
        maxUs = ServoControlConfig::ABSOLUTE_MAX_US;
    }
    if (minUs >= maxUs) {
        return;  // Invalid
    }
    
    _minUs = minUs;
    _maxUs = maxUs;
    
    // Clamp current position and target to new limits
    if (_targetUs < _minUs) _targetUs = (float)_minUs;
    if (_targetUs > _maxUs) _targetUs = (float)_maxUs;
    if (_positionUs < _minUs) {
        _positionUs = (float)_minUs;
        _velocityUsPerS = 0.0f;
    }
    if (_positionUs > _maxUs) {
        _positionUs = (float)_maxUs;
        _velocityUsPerS = 0.0f;
    }
    
    if (_attached) {
        writeServo((int)_positionUs);
    }
}

void ServoControl::setMotionProfile(int maxSpeed, int accel, int decel) {
    if (maxSpeed > 0) _maxSpeed = maxSpeed;
    if (accel > 0) _acceleration = accel;
    if (decel > 0) _deceleration = decel;
}

void ServoControl::setMaxSpeed(int maxSpeed) {
    if (maxSpeed > 0) _maxSpeed = maxSpeed;
}

void ServoControl::setAcceleration(int accel) {
    if (accel > 0) _acceleration = accel;
}

void ServoControl::setDeceleration(int decel) {
    if (decel > 0) _deceleration = decel;
}

// ============================================================================
// Jerk Offset
// ============================================================================

void ServoControl::applyJerk(int jerkUs) {
    _jerkOffset = jerkUs;
}

void ServoControl::clearJerk() {
    _jerkOffset = 0;
}

// ============================================================================
// Position Control
// ============================================================================

void ServoControl::setTarget(int targetUs) {
    // Clamp to limits
    if (targetUs < _minUs) targetUs = _minUs;
    if (targetUs > _maxUs) targetUs = _maxUs;
    
    _targetUs = (float)targetUs;
}

void ServoControl::setPositionImmediate(int positionUs) {
    // Clamp to limits
    if (positionUs < _minUs) positionUs = _minUs;
    if (positionUs > _maxUs) positionUs = _maxUs;
    
    _positionUs = (float)positionUs;
    _targetUs = (float)positionUs;
    _velocityUsPerS = 0.0f;
    
    if (_attached) {
        writeServo(positionUs);
    }
}

bool ServoControl::isMoving() const {
    float dist = _targetUs - _positionUs;
    bool notAtTarget = (dist > ServoControlConfig::POSITION_TOLERANCE || 
                        dist < -ServoControlConfig::POSITION_TOLERANCE);
    bool hasVelocity = (fabs(_velocityUsPerS) >= ServoControlConfig::VELOCITY_TOLERANCE);
    return notAtTarget || hasVelocity;
}

bool ServoControl::atTarget() const {
    return !isMoving();
}

// ============================================================================
// Update
// ============================================================================

int ServoControl::update() {
    return update(millis());
}

int ServoControl::update(uint32_t nowMs) {
    if (!_attached) {
        return (int)_positionUs;
    }
    
    // Initialize timing on first call
    if (_lastUpdateMs == 0) {
        _lastUpdateMs = nowMs;
        return (int)_positionUs;
    }
    
    // Calculate delta time
    uint32_t dtMs = nowMs - _lastUpdateMs;
    if (dtMs == 0) {
        return (int)_positionUs;
    }
    _lastUpdateMs = nowMs;
    
    float dtS = dtMs / 1000.0f;
    float maxSpeed = (float)_maxSpeed;
    float accel = (float)_acceleration;
    float decel = (float)_deceleration;
    
    // Clamp target to limits continuously
    if (_targetUs < _minUs) _targetUs = (float)_minUs;
    if (_targetUs > _maxUs) _targetUs = (float)_maxUs;
    
    float dist = _targetUs - _positionUs;
    int dir = (dist > ServoControlConfig::POSITION_TOLERANCE) ? 1 : 
              (dist < -ServoControlConfig::POSITION_TOLERANCE ? -1 : 0);
    
    // If effectively at target and stopped, snap and exit
    if (dir == 0 && fabs(_velocityUsPerS) < ServoControlConfig::VELOCITY_TOLERANCE) {
        _positionUs = _targetUs;
        _velocityUsPerS = 0.0f;
        
        // Fire target reached callback if we just stopped
        if (_wasMoving) {
            _wasMoving = false;
            if (_targetReachedCallback) {
                _targetReachedCallback(*this);
            }
        }
        
        writeServo((int)_positionUs);
        return (int)_positionUs;
    }
    
    _wasMoving = true;
    
    // Calculate stopping distance
    float stopDist = (decel > 0.0f) ? 
                     ((_velocityUsPerS * _velocityUsPerS) / (2.0f * decel)) : 0.0f;
    bool movingToward = (_velocityUsPerS * dist) > 0.0f;
    
    if (_velocityUsPerS == 0.0f) {
        // Start accelerating toward target
        _velocityUsPerS += dir * accel * dtS;
    } else if (movingToward) {
        // If close enough that we must decelerate, do so; else accelerate up to max
        if (fabs(dist) <= stopDist) {
            _velocityUsPerS = approachZero(_velocityUsPerS, decel * dtS);
        } else {
            _velocityUsPerS += dir * accel * dtS;
        }
    } else {
        // Moving away from target: decelerate to zero first
        _velocityUsPerS = approachZero(_velocityUsPerS, decel * dtS);
    }
    
    // Cap speed
    if (_velocityUsPerS > maxSpeed) _velocityUsPerS = maxSpeed;
    if (_velocityUsPerS < -maxSpeed) _velocityUsPerS = -maxSpeed;
    
    // Integrate position
    _positionUs += _velocityUsPerS * dtS;
    
    // Prevent overshoot past target when moving toward it
    if (dir > 0 && _positionUs > _targetUs) {
        _positionUs = _targetUs;
        _velocityUsPerS = 0.0f;
    } else if (dir < 0 && _positionUs < _targetUs) {
        _positionUs = _targetUs;
        _velocityUsPerS = 0.0f;
    }
    
    // Enforce limits
    if (_positionUs < _minUs) {
        _positionUs = (float)_minUs;
        _velocityUsPerS = 0.0f;
    }
    if (_positionUs > _maxUs) {
        _positionUs = (float)_maxUs;
        _velocityUsPerS = 0.0f;
    }
    
    // Write servo output
    writeServo((int)_positionUs);
    
    return (int)_positionUs;
}

// ============================================================================
// Private Helpers
// ============================================================================

float ServoControl::approachZero(float value, float delta) {
    if (value > 0.0f) {
        value -= delta;
        if (value < 0.0f) value = 0.0f;
    } else if (value < 0.0f) {
        value += delta;
        if (value > 0.0f) value = 0.0f;
    }
    return value;
}

void ServoControl::writeServo(int positionUs) {
    // Apply jerk offset to final servo output
    int outputUs = positionUs + _jerkOffset;
    
    // Clamp output to servo limits
    if (outputUs < _minUs) outputUs = _minUs;
    if (outputUs > _maxUs) outputUs = _maxUs;
    
    _servo.writeMicroseconds(outputUs);
    
    // Fire position callback if position changed significantly
    if (_positionCallback) {
        int delta = outputUs - _lastCallbackPosition;
        if (delta < 0) delta = -delta;
        
        if (delta >= 5) {  // 5us threshold for position change callback
            _lastCallbackPosition = outputUs;
            _positionCallback(*this, outputUs);
        }
    }
}
