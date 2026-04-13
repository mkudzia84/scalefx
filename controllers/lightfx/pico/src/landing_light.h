/*
 * Landing Light Sequencer - Header
 * 
 * Coordinates a retract servo with one or more landing light LED channels.
 * The lights activate when deployment is complete (servo at target)
 * and deactivate before retraction starts.
 * 
 * State Machine:
 *   UNCONFIGURED → (configure) → RETRACTED
 *   RETRACTED    → (deploy)    → DEPLOYING
 *   DEPLOYING    → (atTarget)  → DEPLOYED  [lights ON]
 *   DEPLOYED     → (retract)   → RETRACTING [lights OFF immediately]
 *   RETRACTING   → (atTarget)  → RETRACTED
 * 
 * Servo-optional:
 *   If no servo is bound, deploy/retract complete immediately
 *   (LEDs toggle on/off without waiting for servo motion).
 * 
 * Usage:
 *   LandingLight ll;
 *   LedControl* leds[] = { &ledManager.channel(4), &ledManager.channel(5) };
 *   ll.configure(&servo, leds, 2, 100);  // brightness=100%
 *   ll.deploy();   // Servo moves to open, lights on when arrived
 *   ll.retract();  // Lights off immediately, servo closes
 *   ll.update();   // Call in loop()
 */

#ifndef LANDING_LIGHT_H
#define LANDING_LIGHT_H

#include <Arduino.h>
#include <led/led_control.h>
#include <servo/srv_control.h>
#include <serial/lightfx/lightfx.h>  // LightFxLandingLightStatus, LandingLightPhase

// ============================================================================
// Landing Light State
// ============================================================================

enum class LandingLightState : uint8_t {
    UNCONFIGURED = 0,  // Not bound to servo/LED
    RETRACTED    = 1,  // Servo at retract position, light off
    DEPLOYING    = 2,  // Servo moving to deploy position, light off
    DEPLOYED     = 3,  // Servo at deploy position, light on
    RETRACTING   = 4   // Light off, servo moving to retract position
};

// ============================================================================
// LandingLight Class
// ============================================================================

/**
 * @brief Coordinates a retract servo with one or more landing light LEDs
 * 
 * Binds an optional ServoControl and 1-8 LedControl channels.
 * - Deploy: servo moves to open position (if present), lights on when target reached
 * - Retract: lights off immediately, servo moves to close (if present)
 *
 * If no servo is bound, deploy/retract complete immediately (LEDs toggle).
 * Open/close positions are determined by the servo's limits and direction flag.
 */
class LandingLight {
public:
    static constexpr uint8_t MAX_LEDS = 8;

    LandingLight() = default;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Bind LED channels (and optional servo) as a landing light group
     * 
     * Sets the servo to close (retract) position (if present) and turns
     * all LEDs off. Open/close positions are derived from servo limits.
     * 
     * @param servo Pointer to the retract servo (nullptr = no servo)
     * @param leds Array of LED channel pointers (must not be nullptr)
     * @param ledCount Number of LED channels (1-MAX_LEDS)
     * @param brightness LED brightness when light is on (0-100, default 100)
     */
    void configure(ServoControl* servo, LedControl** leds, uint8_t ledCount,
                   uint8_t brightness = 100);

    /**
     * @brief Unbind the servo and LED channels
     * 
     * Turns all lights off and resets to unconfigured state.
     * Does not move the servo.
     */
    void unconfigure();

    // ========================================================================
    // Actions
    // ========================================================================

    /**
     * @brief Start deployment sequence
     * 
     * Moves the servo to the deploy position (if present). The lights
     * will turn on automatically when the servo reaches the target.
     * If no servo, lights turn on immediately.
     */
    void deploy();

    /**
     * @brief Start retraction sequence
     * 
     * Turns all lights off IMMEDIATELY, then moves the servo to
     * the retract position (if present). If no servo, completes immediately.
     */
    void retract();

    // ========================================================================
    // Update
    // ========================================================================

    /**
     * @brief Update state machine (call in loop)
     * 
     * Monitors servo position and triggers light state transitions:
     * - DEPLOYING + atTarget → lights ON, state = DEPLOYED
     * - RETRACTING + atTarget → state = RETRACTED
     * If no servo is bound, transitions happen immediately in deploy()/retract().
     */
    void update();

    /**
     * @brief Emergency shutdown - all lights off, state to retracted
     * 
     * Does not move the servo (servo shutdown handled separately).
     */
    void shutdown();

    // ========================================================================
    // Accessors
    // ========================================================================

    /** @brief Check if this slot is configured */
    bool isConfigured() const { return _state != LandingLightState::UNCONFIGURED; }

    /** @brief Get current state */
    LandingLightState state() const { return _state; }

    /** @brief Get number of bound LED channels */
    uint8_t ledCount() const { return _ledCount; }

    /** @brief Check if a servo is bound */
    bool hasServo() const { return _servo != nullptr; }

    // ========================================================================
    // Progress Callback
    // ========================================================================

    /**
     * @brief Callback for deploy/retract progress updates
     *
     * Called on each state transition during deploy/retract operations.
     * Receives a LightFxLandingLightStatus struct matching the
     * LANDING_LIGHT_STATUS wire format.
     */
    using ProgressCallback = std::function<void(const LightFxLandingLightStatus&)>;

    /**
     * @brief Register callback for deploy/retract progress
     * @param cb Callback receiving LightFxLandingLightStatus
     */
    void onProgress(ProgressCallback cb) { _progressCb = cb; }

    /**
     * @brief Set the slot ID for progress reporting
     * @param slot Slot index (1-3)
     */
    void setSlot(uint8_t slot) { _slot = slot; }

private:
    ServoControl* _servo = nullptr;         // Optional, nullptr = no servo
    LedControl* _leds[MAX_LEDS] = {};       // LED channels (up to 8)
    uint8_t _ledCount = 0;                  // Number of bound LEDs
    uint8_t _brightness = 255;
    uint8_t _slot = 0;  // Slot ID for progress reporting (1-3)
    LandingLightState _state = LandingLightState::UNCONFIGURED;
    ProgressCallback _progressCb;

    // LED helpers
    void _ledsOn();
    void _ledsOff();

    // Emit progress to registered callback
    void _emitProgress(bool finished = false);
};

#endif // LANDING_LIGHT_H
