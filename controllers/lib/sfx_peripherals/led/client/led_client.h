/*
 * LED Protocol Client — Shared LED Command Builders
 *
 * Client-side LED serial communication (binary COBS protocol).
 * Provides command builder methods for all 13 LED commands and
 * response parsing for LED query responses.
 *
 * Designed to be either:
 *   - Used standalone (for HubFX → LED-only boards)
 *   - Extended by LightFxClient (which adds servo + landing light commands)
 *
 * Protocol types in <serial/lightfx/lightfx.h>.
 */

#ifndef LED_PROTOCOL_CLIENT_H
#define LED_PROTOCOL_CLIENT_H

#include <serial/lightfx/lightfx.h>
#include <serial/client/bus_client.h>

// ============================================================================
// LedProtocolClient Class
// ============================================================================

/**
 * @brief Shared LED client — command builders and response parsing for 13 LED commands.
 *
 * Extends BusClient. Module-specific clients (LightFxClient) extend this
 * to add servo, landing light, and other commands.
 */
class LedProtocolClient : public BusClient {
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
    CommandResult ledSeqAddOn(uint8_t channel, uint16_t durationMs, uint8_t brightness = 100,
                               uint8_t pwmDuty = 0);
    CommandResult ledSeqAddOff(uint8_t channel, uint16_t durationMs);
    CommandResult ledSeqAddFlash(uint8_t channel, uint16_t intervalMs, uint16_t durationMs,
                                 uint8_t brightness = 100, uint8_t dutyPercent = 50);
    CommandResult ledSeqAddFadeIn(uint8_t channel, uint16_t durationMs, uint8_t brightness = 100);
    CommandResult ledSeqAddFadeOut(uint8_t channel, uint16_t durationMs, uint8_t brightness = 100);
    CommandResult ledSeqAddFading(uint8_t channel, uint16_t cycleMs, uint16_t durationMs = 0,
                                  uint8_t minBrightness = 0, uint8_t maxBrightness = 100);
    CommandResult ledSeqAddBeacon(uint8_t channel, uint16_t cycleMs, uint16_t durationMs = 0,
                                  uint8_t flashPercent = 15, uint8_t maxBrightness = 100,
                                  uint8_t minBrightness = 0);
    CommandResult ledSeqStart(uint8_t channel);
    CommandResult ledSeqStop(uint8_t channel);
    CommandResult ledSeqRestart(uint8_t channel);
    CommandResult ledSeqStatus(uint8_t channel);
    CommandResult ledStatus();
    CommandResult ledMasterBrightness(uint8_t pct);

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
    // Callbacks (LED-specific responses)
    // ========================================================================

    void onSeqStatus(LightFxSeqStatusCallback cb) { _seqStatusCallback = cb; }
    void onChannelStatus(LightFxChannelStatusCallback cb) { _channelStatusCallback = cb; }
    void onSeqQueue(LightFxSeqQueueCallback cb) { _seqQueueCallback = cb; }

    /**
     * @brief Board information returned during init (alias for BusClientBoardInfo)
     */
    using BoardInfo = BusClientBoardInfo;

protected:
    /**
     * @brief Parse LED response packets.  Returns true if handled.
     *
     * Subclasses should call this from their onModulePacket() override
     * before handling their own packet types.
     */
    bool handleLedResponse(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len);

    void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override;

    const char* getModuleErrorMessage(uint8_t code) override {
        return LightFxError::getMessage(code);
    }

private:
    LightFxSeqStatusCallback _seqStatusCallback;
    LightFxChannelStatusCallback _channelStatusCallback;
    LightFxSeqQueueCallback _seqQueueCallback;
};

#endif // LED_PROTOCOL_CLIENT_H
