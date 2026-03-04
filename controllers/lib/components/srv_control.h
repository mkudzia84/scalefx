/*
 * Servo Control Library - Header
 * 
 * Object-oriented servo output control with motion profiling.
 * Implements smooth acceleration/deceleration and jerk offset effects.
 * 
 * Features:
 *   - Trapezoidal velocity motion profiling
 *   - Configurable acceleration and deceleration
 *   - Maximum speed limiting
 *   - Servo position limits with clamping
 *   - Jerk offset (adds temporary offset to position)
 *   - Async callbacks for position changes
 * 
 * Usage:
 *   ServoControl servo;
 *   servo.begin(1, 500, 2500);  // pin 1, limits 500-2500us
 *   servo.setTarget(1500);       // Set target position
 *   servo.update();              // Call regularly in loop
 */

#ifndef SRV_CONTROL_H
#define SRV_CONTROL_H

#include <Arduino.h>
#include <Servo.h>
#include <functional>

// ============================================================================
// Configuration Constants
// ============================================================================

namespace ServoControlConfig {
    // Default motion profile values
    constexpr int DEFAULT_MAX_SPEED = 4000;    // us per second
    constexpr int DEFAULT_ACCEL = 8000;        // us per second^2
    constexpr int DEFAULT_DECEL = 8000;        // us per second^2
    
    // Default position limits
    constexpr int DEFAULT_MIN_US = 500;        // Minimum pulse width
    constexpr int DEFAULT_MAX_US = 2500;       // Maximum pulse width
    constexpr int DEFAULT_CENTER_US = 1500;    // Center/neutral position
    
    // Absolute limits (hardware protection)
    constexpr int ABSOLUTE_MIN_US = 300;       // Never go below this
    constexpr int ABSOLUTE_MAX_US = 2700;      // Never go above this
    
    // Motion thresholds
    constexpr float POSITION_TOLERANCE = 0.5f; // Position snap threshold
    constexpr float VELOCITY_TOLERANCE = 1.0f; // Velocity zero threshold
}

// ============================================================================
// Forward Declaration
// ============================================================================

class ServoControl;

// ============================================================================
// Callback Types
// ============================================================================

/**
 * @brief Callback for position change events
 * @param servo Reference to the ServoControl
 * @param positionUs Current position in microseconds
 */
using ServoPositionCallback = std::function<void(ServoControl& servo, int positionUs)>;

/**
 * @brief Callback for target reached events
 * @param servo Reference to the ServoControl
 */
using ServoTargetReachedCallback = std::function<void(ServoControl& servo)>;

// ============================================================================
// ServoControl Class
// ============================================================================

/**
 * @brief Servo output with motion profiling and effects
 */
class ServoControl {
public:
    ServoControl() = default;
    ~ServoControl();
    
    // Non-copyable (due to Servo object)
    ServoControl(const ServoControl&) = delete;
    ServoControl& operator=(const ServoControl&) = delete;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize servo output
     * @param pin GPIO pin for servo signal
     * @param minUs Minimum pulse width limit (default: 500)
     * @param maxUs Maximum pulse width limit (default: 2500)
     * @param initialUs Initial position (default: center)
     * @return true if initialization succeeded
     */
    bool begin(int pin, int minUs = ServoControlConfig::DEFAULT_MIN_US,
               int maxUs = ServoControlConfig::DEFAULT_MAX_US,
               int initialUs = ServoControlConfig::DEFAULT_CENTER_US);

    /**
     * @brief Detach servo and release resources
     */
    void end();

    /**
     * @brief Check if servo is initialized
     */
    bool isAttached() const { return _attached; }

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Set position limits
     * @param minUs Minimum pulse width
     * @param maxUs Maximum pulse width
     */
    void setLimits(int minUs, int maxUs);

    /**
     * @brief Get minimum limit
     */
    int minLimit() const { return _minUs; }

    /**
     * @brief Get maximum limit
     */
    int maxLimit() const { return _maxUs; }

    /**
     * @brief Set motion profile parameters
     * @param maxSpeed Maximum speed in us/second
     * @param accel Acceleration in us/second^2
     * @param decel Deceleration in us/second^2
     */
    void setMotionProfile(int maxSpeed, int accel, int decel);

    /**
     * @brief Set maximum speed only
     * @param maxSpeed Maximum speed in us/second
     */
    void setMaxSpeed(int maxSpeed);

    /**
     * @brief Set acceleration only
     * @param accel Acceleration in us/second^2
     */
    void setAcceleration(int accel);

    /**
     * @brief Set deceleration only
     * @param decel Deceleration in us/second^2
     */
    void setDeceleration(int decel);

    /**
     * @brief Get current max speed setting
     */
    int maxSpeed() const { return _maxSpeed; }

    /**
     * @brief Get current acceleration setting
     */
    int acceleration() const { return _acceleration; }

    /**
     * @brief Get current deceleration setting
     */
    int deceleration() const { return _deceleration; }

    // ========================================================================
    // Jerk Offset
    // ========================================================================

    /**
     * @brief Apply a jerk offset to the servo position
     * 
     * Adds an offset in microseconds to the current position output.
     * The offset is clamped to stay within motion limits.
     * Use clearJerk() to remove the offset.
     * 
     * @param jerkUs Offset in microseconds (positive or negative)
     */
    void applyJerk(int jerkUs);

    /**
     * @brief Clear jerk offset
     * 
     * Removes any applied jerk offset, returning to normal position.
     */
    void clearJerk();

    /**
     * @brief Get current jerk offset
     * @return Current jerk offset in microseconds
     */
    int jerkOffset() const { return _jerkOffset; }

    // ========================================================================
    // Position Control
    // ========================================================================

    /**
     * @brief Set target position (with motion profiling)
     * 
     * The servo will move smoothly to the target position using
     * the configured acceleration, deceleration, and max speed.
     * 
     * @param targetUs Target position in microseconds
     */
    void setTarget(int targetUs);

    /**
     * @brief Set position immediately (no motion profiling)
     * 
     * Jumps directly to the specified position.
     * 
     * @param positionUs Position in microseconds
     */
    void setPositionImmediate(int positionUs);

    /**
     * @brief Get current target position
     */
    int target() const { return (int)_targetUs; }

    /**
     * @brief Get current actual position
     */
    int position() const { return (int)_positionUs; }

    /**
     * @brief Get current velocity
     */
    float velocity() const { return _velocityUsPerS; }

    /**
     * @brief Check if servo is moving
     */
    bool isMoving() const;

    /**
     * @brief Check if servo has reached target
     */
    bool atTarget() const;

    // ========================================================================
    // Update
    // ========================================================================

    /**
     * @brief Update servo motion (call regularly in loop)
     * 
     * Performs motion profiling calculations and updates servo output.
     * Should be called as frequently as possible for smooth motion.
     * 
     * @return Current position in microseconds
     */
    int update();

    /**
     * @brief Update with explicit timestamp
     * @param nowMs Current time in milliseconds
     * @return Current position in microseconds
     */
    int update(uint32_t nowMs);

    // ========================================================================
    // Event Callbacks
    // ========================================================================

    /**
     * @brief Set callback for position changes
     * @param callback Function called when position changes significantly
     */
    void onPositionChange(ServoPositionCallback callback) { _positionCallback = callback; }

    /**
     * @brief Set callback for target reached
     * @param callback Function called when target is reached
     */
    void onTargetReached(ServoTargetReachedCallback callback) { _targetReachedCallback = callback; }

    // ========================================================================
    // Utility
    // ========================================================================

    /**
     * @brief Get the GPIO pin
     */
    int pin() const { return _pin; }

    /**
     * @brief Get a user-defined ID (for multi-servo management)
     */
    uint8_t id() const { return _id; }

    /**
     * @brief Set a user-defined ID
     */
    void setId(uint8_t id) { _id = id; }

private:
    float approachZero(float value, float delta);
    void writeServo(int positionUs);

    // Hardware
    Servo _servo;
    int _pin = -1;
    uint8_t _id = 0;
    bool _attached = false;

    // Position limits
    int _minUs = ServoControlConfig::DEFAULT_MIN_US;
    int _maxUs = ServoControlConfig::DEFAULT_MAX_US;

    // Motion profile
    int _maxSpeed = ServoControlConfig::DEFAULT_MAX_SPEED;
    int _acceleration = ServoControlConfig::DEFAULT_ACCEL;
    int _deceleration = ServoControlConfig::DEFAULT_DECEL;

    // Motion state
    float _positionUs = ServoControlConfig::DEFAULT_CENTER_US;
    float _targetUs = ServoControlConfig::DEFAULT_CENTER_US;
    float _velocityUsPerS = 0.0f;
    uint32_t _lastUpdateMs = 0;
    bool _wasMoving = false;

    // Jerk offset
    int _jerkOffset = 0;

    // Callbacks
    ServoPositionCallback _positionCallback = nullptr;
    ServoTargetReachedCallback _targetReachedCallback = nullptr;
    int _lastCallbackPosition = 0;
};

#endif // SRV_CONTROL_H
