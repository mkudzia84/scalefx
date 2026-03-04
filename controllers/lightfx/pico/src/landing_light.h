/*
 * Landing Light Sequencer - Header
 * 
 * Coordinates a retract servo with a landing light LED channel.
 * The light activates when deployment is complete (servo at target)
 * and deactivates before retraction starts.
 * 
 * State Machine:
 *   UNCONFIGURED → (configure) → RETRACTED
 *   RETRACTED    → (deploy)    → DEPLOYING
 *   DEPLOYING    → (atTarget)  → DEPLOYED  [light ON]
 *   DEPLOYED     → (retract)   → RETRACTING [light OFF immediately]
 *   RETRACTING   → (atTarget)  → RETRACTED
 * 
 * Usage:
 *   LandingLight ll;
 *   ll.configure(&servo, &led, 2000, 1000, 100);  // deploy=2000µs, retract=1000µs
 *   ll.deploy();   // Servo moves to 2000µs, light on when arrived
 *   ll.retract();  // Light off immediately, servo moves to 1000µs
 *   ll.update();   // Call in loop()
 */

#ifndef LANDING_LIGHT_H
#define LANDING_LIGHT_H

#include <Arduino.h>
#include <led_control.h>
#include <srv_control.h>

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
 * @brief Coordinates a retract servo with a landing light LED channel
 * 
 * Binds one ServoControl and one LedControl to implement the sequence:
 * - Deploy: servo moves to deploy position, light turns on when target reached
 * - Retract: light turns off immediately, then servo moves to retract position
 */
class LandingLight {
public:
    LandingLight() = default;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Bind a servo and LED channel as a landing light pair
     * 
     * Sets the servo to the retract position and turns the light off.
     * 
     * @param servo Pointer to the retract servo
     * @param led Pointer to the landing light LED channel
     * @param deployUs Servo position when gear is deployed (µs)
     * @param retractUs Servo position when gear is retracted (µs)
     * @param brightness LED brightness when light is on (0-100, default 100)
     */
    void configure(ServoControl* servo, LedControl* led,
                   uint16_t deployUs, uint16_t retractUs,
                   uint8_t brightness = 100);

    /**
     * @brief Unbind the servo and LED channel
     * 
     * Turns the light off and resets to unconfigured state.
     * Does not move the servo.
     */
    void unconfigure();

    // ========================================================================
    // Actions
    // ========================================================================

    /**
     * @brief Start deployment sequence
     * 
     * Moves the servo to the deploy position. The light will turn on
     * automatically when the servo reaches the target (in update()).
     */
    void deploy();

    /**
     * @brief Start retraction sequence
     * 
     * Turns the light off IMMEDIATELY, then moves the servo to
     * the retract position. This ensures the light is off before
     * the gear mechanism begins folding.
     */
    void retract();

    // ========================================================================
    // Update
    // ========================================================================

    /**
     * @brief Update state machine (call in loop)
     * 
     * Monitors servo position and triggers light state transitions:
     * - DEPLOYING + atTarget → light ON, state = DEPLOYED
     * - RETRACTING + atTarget → state = RETRACTED
     */
    void update();

    /**
     * @brief Emergency shutdown - light off, state to retracted
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

    /** @brief Get deploy servo position (µs) */
    uint16_t deployUs() const { return _deployUs; }

    /** @brief Get retract servo position (µs) */
    uint16_t retractUs() const { return _retractUs; }

    /** @brief Get configured brightness */
    uint8_t brightness() const { return _brightness; }

private:
    ServoControl* _servo = nullptr;
    LedControl* _led = nullptr;
    uint16_t _deployUs = 0;
    uint16_t _retractUs = 0;
    uint8_t _brightness = 255;
    LandingLightState _state = LandingLightState::UNCONFIGURED;
};

#endif // LANDING_LIGHT_H
