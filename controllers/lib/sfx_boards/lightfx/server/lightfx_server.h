/*
 * LightFX Server — Binary Protocol Command Handler
 *
 * Server-side LightFX serial communication (binary COBS protocol).
 * Used by LightFX Pico to receive commands from HubFX client.
 *
 * Extends LedProtocolServer (which handles the 13 LED commands) and adds
 * LightFX-specific commands: servos (0x50-0x51) and landing lights (0x52-0x55).
 *
 * Included from sfx_boards library. Protocol types in <serial/lightfx/lightfx.h>.
 */

#ifndef BOARDS_LIGHTFX_SERVER_H
#define BOARDS_LIGHTFX_SERVER_H

#include <serial/lightfx/lightfx.h>
#include <led/server/led_server.h>

// ============================================================================
// LightFxServer Class (Binary Protocol)
// ============================================================================

/**
 * @brief Server-side LightFX serial communication (binary COBS protocol)
 *
 * Extends LedProtocolServer (all 13 LED commands handled automatically)
 * and adds servo + landing light command handling.
 *
 * Usage:
 *   LedManager<8> ledManager;
 *   LightFxServer lightfxServer;
 *   lightfxServer.setLedManager(&ledManager);
 *   lightfxServer.begin(&Serial);
 *   lightfxServer.onServoSet([](uint8_t id, int pulseUs) -> uint8_t { ... });
 *   // ... register servo/landing light callbacks
 *   router.addHandler(&lightfxServer);
 */
class LightFxServer : public LedProtocolServer {
public:
    const char* handlerName() const override { return "LightFxServer"; }

    // ========================================================================
    // Landing Light Response
    // ========================================================================

    int sendLandingLightStatus(const LightFxLandingLightStatus& status);

    // ========================================================================
    // Servo Callbacks
    // ========================================================================

    void onServoSet(ServoSetCallback cb) { _servoSetCallback = cb; }
    void onServoSettings(ServoSettingsCallback cb) { _servoSettingsCallback = cb; }

    // ========================================================================
    // Landing Light Callbacks
    // ========================================================================

    void onLandingLightBind(LandingLightBindCallback cb) { _landingLightBindCallback = cb; }
    void onLandingLightUnbind(LandingLightSlotCallback cb) { _landingLightUnbindCallback = cb; }
    void onLandingLightDeploy(LandingLightSlotCallback cb) { _landingLightDeployCallback = cb; }
    void onLandingLightRetract(LandingLightSlotCallback cb) { _landingLightRetractCallback = cb; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;

    // Extend range to include servos (0x50-0x51) and landing lights (0x52-0x56)
    uint8_t moduleRangeHigh() const override { return 0x5F; }

private:
    ServoSetCallback _servoSetCallback;
    ServoSettingsCallback _servoSettingsCallback;
    LandingLightBindCallback _landingLightBindCallback;
    LandingLightSlotCallback _landingLightUnbindCallback;
    LandingLightSlotCallback _landingLightDeployCallback;
    LandingLightSlotCallback _landingLightRetractCallback;
    uint8_t _landingLightTag[3] = {};  // Per-slot tag for deploy/retract progress
};

#endif // BOARDS_LIGHTFX_SERVER_H
