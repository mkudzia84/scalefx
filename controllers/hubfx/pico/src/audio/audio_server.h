/*
 * Audio Server — Command Handler for Audio Playback Control
 *
 * Handles audio-specific packets (0x84-0x8B):
 *   - Play, stop, volume, fade
 *   - Queue management
 *   - Audio mixer status query
 *
 * Registered as a separate handler in PicoServer's CommandRouter,
 * keeping audio concerns isolated from other HubFX domains.
 */

#ifndef AUDIO_SERVER_H
#define AUDIO_SERVER_H

#include <Arduino.h>
#include <serial_bus_server.h>

#include "../board_manager/hubfx_protocol.h"

// Forward declaration — avoids coupling to audio_mixer.h
class AudioMixer;

// ============================================================================
// AudioServer — ICommandHandler for Audio Playback
// ============================================================================

class AudioServer : public BusServer {
public:
    AudioServer() = default;

    const char* handlerName() const override { return "AudioServer"; }

    void setAudioMixer(AudioMixer* mixer) { _mixer = mixer; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override  { return 0x84; }
    uint8_t moduleRangeHigh() const override { return 0x8B; }
    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

private:
    void handlePlay(const uint8_t* payload, size_t len);
    void handleStop(const uint8_t* payload, size_t len);
    void handleVolume(const uint8_t* payload, size_t len);
    void handleFade(const uint8_t* payload, size_t len);
    void handleQueue(const uint8_t* payload, size_t len);
    void handleQueueClear(const uint8_t* payload, size_t len);
    void handleStatusReq();

    AudioMixer* _mixer = nullptr;
};

#endif // AUDIO_SERVER_H
