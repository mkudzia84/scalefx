/*
 * LightFX Client — Binary Protocol Client
 *
 * Client-side LightFX serial communication (binary COBS protocol).
 * Used by HubFX to send commands to LightFX server boards over USB.
 *
 * Extends LedProtocolClient (shared LED commands) with LightFX-specific
 * servo and landing light commands.
 *
 * Included from sfx_boards library. Protocol types in <serial/lightfx/lightfx.h>.
 */

#ifndef BOARDS_LIGHTFX_CLIENT_H
#define BOARDS_LIGHTFX_CLIENT_H

#include <serial/lightfx/lightfx.h>
#include <led/client/led_client.h>

// ============================================================================
// LightFxClient Class (Binary Protocol)
// ============================================================================

/**
 * @brief Client-side LightFX serial communication (binary COBS protocol)
 *
 * Extends LedProtocolClient (13 LED commands + LED response parsing)
 * with LightFX-specific servo and landing light commands.
 */
class LightFxClient : public LedProtocolClient {
public:
    // ========================================================================
    // Servo Control
    // ========================================================================

    CommandResult servoSet(uint8_t id, int16_t pulseUs);
    CommandResult servoSettings(uint8_t id, uint16_t minUs, uint16_t maxUs,
                       uint16_t speed, uint16_t accel, uint16_t decel,
                       bool reversed = false);

    // ========================================================================
    // Landing Light Control
    // ========================================================================

    CommandResult landingLightBind(uint8_t slot, uint8_t servoId, uint8_t channelMask,
                          uint8_t brightness = 255);
    CommandResult landingLightUnbind(uint8_t slot = 0);
    CommandResult landingLightDeploy(uint8_t slot = 0);
    CommandResult landingLightRetract(uint8_t slot = 0);

    // ========================================================================
    // Light Program Runtime Control
    // ========================================================================

    /// Activate a program by 0-based index. The program (and all servo /
    /// landing-group bindings it touches) must already be loaded on the slave
    /// from /lightfx.yaml.
    CommandResult lightProgramSelect(uint8_t programIndex);

    /// Stop all sequences, retract all landing groups, leave no active program.
    CommandResult lightProgramReset();

    // ========================================================================
    // Callbacks (LightFX-specific)
    // ========================================================================

    void onLandingLightStatus(LightFxLandingLightStatusCallback cb) { _landingLightStatusCallback = cb; }

protected:
    void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override;

private:
    LightFxLandingLightStatusCallback _landingLightStatusCallback;
};

#endif // BOARDS_LIGHTFX_CLIENT_H
