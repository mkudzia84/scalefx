/*
 * HubFX Audio Client — Client-Side Audio Serial Communication
 *
 * Used by any controller or app that sends audio playback commands
 * to a HubFX hub over USB. Extends BusClient with audio-specific
 * command methods and AUDIO_STATUS_RESP parsing.
 *
 * Shared library component (controllers/lib/sfx_audio/).
 * Depends on: sfx_serial (BusClient, HubFxPacket, HubFxAudio, HubFxError).
 */

#ifndef AUDIO_CLIENT_H
#define AUDIO_CLIENT_H

#include <protocol/audio_protocol.h>
#include <serial/client/bus_client.h>

// ============================================================================
// HubFxAudioClient — Client for Audio Commands
// ============================================================================

class HubFxAudioClient : public BusClient {
public:
    // ========================================================================
    // Playback Control
    // ========================================================================

    CommandResult play(uint8_t channel, const char* path, uint8_t volumePct = 100,
                       uint8_t outputChannels = AudioWire::OUTPUT_ALL,
                       uint8_t loopMode = AudioWire::LOOP_NONE,
                       uint16_t loopCount = 0);
    CommandResult stop(uint8_t channel = AudioWire::CH_ALL);
    CommandResult fade(uint8_t channel);

    // ========================================================================
    // Volume Control
    // ========================================================================

    CommandResult setVolume(uint8_t channel, uint8_t volumePct);
    CommandResult setMasterVolume(uint8_t volumePct);

    // ========================================================================
    // Queue Control
    // ========================================================================

    CommandResult queueSound(uint8_t channel, const char* path, uint8_t volumePct = 100,
                             uint16_t loopCount = 0,
                             uint8_t behavior = AudioWire::QUEUE_FINISH_LOOP);
    CommandResult queueClear(uint8_t channel = AudioWire::CH_ALL);

    // ========================================================================
    // Status
    // ========================================================================

    CommandResult requestStatus();

    // ========================================================================
    // Callbacks
    // ========================================================================

    void onAudioStatus(HubFxAudioStatusCallback cb) { _statusCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================

    const HubFxAudioStatus& lastStatus() const { return _lastStatus; }

protected:
    void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override;
    const char* getModuleErrorMessage(uint8_t code) override { return AudioError::getMessage(code); }

private:
    HubFxAudioStatus _lastStatus;
    HubFxAudioStatusCallback _statusCallback;
};

#endif // AUDIO_CLIENT_H
