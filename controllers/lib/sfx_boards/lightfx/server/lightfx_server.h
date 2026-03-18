/*
 * LightFX Server — Binary Protocol Command Handler
 *
 * Server-side LightFX serial communication (binary COBS protocol).
 * Used by LightFX Pico to receive commands from HubFX client.
 * Extends BusServer for use with SfxServer + CommandRouter.
 *
 * Included from sfx_boards library. Protocol types in <serial/lightfx/lightfx.h>.
 */

#ifndef BOARDS_LIGHTFX_SERVER_H
#define BOARDS_LIGHTFX_SERVER_H

#include <serial/lightfx/lightfx.h>
#include <serial/core/bus_server.h>

// ============================================================================
// LightFxServer Class (Binary Protocol)
// ============================================================================

/**
 * @brief Server-side LightFX serial communication (binary COBS protocol)
 * 
 * Used by LightFX Pico to receive commands from HubFX client.
 * Extends BusServer for use with SfxServer + CommandRouter.
 */
class LightFxServer : public BusServer {
public:
    const char* handlerName() const override { return "LightFxServer"; }

    // ========================================================================
    // Response Methods
    // ========================================================================
    
    int sendSeqStatus(const LightFxSeqStatus& status);
    int sendSeqQueue(const LightFxSeqQueue& queue);
    int sendChannelStatus(const LightFxChannelStatus* channels, uint8_t count);
    int sendLandingLightStatus(const LightFxLandingLightStatus& status);

    // ========================================================================
    // LED Callbacks
    // ========================================================================
    
    void onLedSet(LedSetCallback cb) { _ledSetCallback = cb; }
    void onLedOff(LedOffCallback cb) { _ledOffCallback = cb; }
    void onLedSeqClear(LedSeqClearCallback cb) { _ledSeqClearCallback = cb; }
    void onLedSeqAdd(LedSeqAddCallback cb) { _ledSeqAddCallback = cb; }
    void onLedSeqStart(LedSeqStartCallback cb) { _ledSeqStartCallback = cb; }
    void onLedSeqStop(LedSeqStopCallback cb) { _ledSeqStopCallback = cb; }
    void onLedSeqRestart(LedSeqRestartCallback cb) { _ledSeqRestartCallback = cb; }
    void onLedSeqStatus(LedSeqStatusCallback cb) { _ledSeqStatusCallback = cb; }
    void onLedSeqQueue(LedSeqQueueCallback cb) { _ledSeqQueueCallback = cb; }
    void onLedStatus(LedChannelStatusCallback cb) { _ledStatusCallback = cb; }
    void onLedMasterBrightness(LedMasterBrightnessCallback cb) { _ledMasterBrightnessCallback = cb; }
    void onLedReset(LedResetCallback cb) { _ledResetCallback = cb; }
    void onLedEnable(LedEnableCallback cb) { _ledEnableCallback = cb; }

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
    uint8_t moduleRangeLow() const override { return 0x40; }
    uint8_t moduleRangeHigh() const override { return 0x5F; }
    const char* getModuleErrorMessage(uint8_t code) override { return LightFxError::getMessage(code); }

private:
    LedSetCallback _ledSetCallback;
    LedOffCallback _ledOffCallback;
    LedSeqClearCallback _ledSeqClearCallback;
    LedSeqAddCallback _ledSeqAddCallback;
    LedSeqStartCallback _ledSeqStartCallback;
    LedSeqStopCallback _ledSeqStopCallback;
    LedSeqRestartCallback _ledSeqRestartCallback;
    LedSeqStatusCallback _ledSeqStatusCallback;
    LedSeqQueueCallback _ledSeqQueueCallback;
    LedChannelStatusCallback _ledStatusCallback;
    LedMasterBrightnessCallback _ledMasterBrightnessCallback;
    LedResetCallback _ledResetCallback;
    LedEnableCallback _ledEnableCallback;
    ServoSetCallback _servoSetCallback;
    ServoSettingsCallback _servoSettingsCallback;
    LandingLightBindCallback _landingLightBindCallback;
    LandingLightSlotCallback _landingLightUnbindCallback;
    LandingLightSlotCallback _landingLightDeployCallback;
    LandingLightSlotCallback _landingLightRetractCallback;
    uint8_t _landingLightTag[3] = {};  // Per-slot tag for deploy/retract progress
};

#endif // BOARDS_LIGHTFX_SERVER_H
