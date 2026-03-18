/*
 * LightFX Client — Binary Protocol Client
 *
 * Client-side LightFX serial communication (binary COBS protocol).
 * Used by HubFX to send commands to LightFX server boards over USB.
 * Extends BusClient with LightFX-specific commands.
 *
 * Included from sfx_boards library. Protocol types in <serial/lightfx/lightfx.h>.
 */

#ifndef BOARDS_LIGHTFX_CLIENT_H
#define BOARDS_LIGHTFX_CLIENT_H

#include <serial/lightfx/lightfx.h>
#include <serial/client/bus_client.h>

// ============================================================================
// LightFxClient Class (Binary Protocol)
// ============================================================================

/**
 * @brief Client-side LightFX serial communication (binary COBS protocol)
 * 
 * Used by HubFX to send commands to LightFX server boards over USB.
 * Extends BusClient with LightFX-specific commands.
 */
class LightFxClient : public BusClient {
public:
    // ========================================================================
    // LED Direct Control
    // ========================================================================
    
    CommandResult ledSet(uint8_t channel, uint8_t brightness);
    CommandResult ledOff(uint8_t channel = 0);

    // ========================================================================
    // LED Sequence Control
    // ========================================================================
    
    CommandResult ledSeqClear(uint8_t channel);
    CommandResult ledSeqAddOn(uint8_t channel, uint16_t durationMs, uint8_t brightness = 255);
    CommandResult ledSeqAddOff(uint8_t channel, uint16_t durationMs);
    CommandResult ledSeqAddFlash(uint8_t channel, uint16_t intervalMs, uint16_t durationMs,
                        uint8_t brightness = 255, uint8_t dutyPercent = 50);
    CommandResult ledSeqAddFadeIn(uint8_t channel, uint16_t durationMs, uint8_t brightness = 255);
    CommandResult ledSeqAddFadeOut(uint8_t channel, uint16_t durationMs, uint8_t brightness = 255);
    CommandResult ledSeqAddFading(uint8_t channel, uint16_t cycleMs, uint16_t durationMs = 0,
                         uint8_t minBrightness = 0, uint8_t maxBrightness = 255);
    CommandResult ledSeqStart(uint8_t channel);
    CommandResult ledSeqStop(uint8_t channel);
    CommandResult ledSeqRestart(uint8_t channel);
    CommandResult ledSeqStatus(uint8_t channel);
    CommandResult ledStatus();
    CommandResult ledMasterBrightness(uint8_t pct);

    // ========================================================================
    // Servo Control
    // ========================================================================
    
    CommandResult servoSet(uint8_t id, int16_t pulseUs);
    CommandResult servoSettings(uint8_t id, uint16_t minUs, uint16_t maxUs,
                       uint16_t speed, uint16_t accel, uint16_t decel);

    // ========================================================================
    // Landing Light Control
    // ========================================================================
    
    CommandResult landingLightBind(uint8_t slot, uint8_t servoId, uint8_t ledChannel,
                          uint16_t deployUs, uint16_t retractUs, uint8_t brightness = 255);
    CommandResult landingLightUnbind(uint8_t slot = 0);
    CommandResult landingLightDeploy(uint8_t slot = 0);
    CommandResult landingLightRetract(uint8_t slot = 0);

    // ========================================================================
    // Channel Management
    // ========================================================================

    CommandResult ledReset(uint8_t channel = 0);
    CommandResult ledEnable(uint8_t channel, bool enabled);

    // ========================================================================
    // Status
    // ========================================================================

    CommandResult requestStatus();

    // ========================================================================
    // Callbacks
    // ========================================================================
    
    void onSeqStatus(LightFxSeqStatusCallback cb) { _seqStatusCallback = cb; }
    void onChannelStatus(LightFxChannelStatusCallback cb) { _channelStatusCallback = cb; }
    void onSeqQueue(LightFxSeqQueueCallback cb) { _seqQueueCallback = cb; }
    void onLandingLightStatus(LightFxLandingLightStatusCallback cb) { _landingLightStatusCallback = cb; }

    /**
     * @brief Board information returned during init (alias for BusClientBoardInfo)
     */
    using BoardInfo = BusClientBoardInfo;

protected:
    void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override;
    const char* getModuleErrorMessage(uint8_t code) override { return LightFxError::getMessage(code); }

private:
    LightFxSeqStatusCallback _seqStatusCallback;
    LightFxChannelStatusCallback _channelStatusCallback;
    LightFxSeqQueueCallback _seqQueueCallback;
    LightFxLandingLightStatusCallback _landingLightStatusCallback;
};

#endif // BOARDS_LIGHTFX_CLIENT_H
