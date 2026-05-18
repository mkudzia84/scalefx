/*
 * Audio Protocol Server — Template for Audio Command Handling
 *
 * AudioServicePolicy<TMixer> is a system-service policy that handles the
 * HubFX audio control protocol (0x84-0x8B). It translates wire-format
 * commands into AudioMixer async API calls, providing thread-safe audio
 * control from the serial protocol layer.
 *
 * The policy is parameterized by the mixer type so it works with any
 * AudioMixer<TI2S, TCodec> instantiation without hard-coding the
 * I2S/codec types.
 *
 * Usage:
 *   // In controller firmware (slotted into BoardServer's UserPolicies):
 *   using Mixer = AudioMixer<EspI2SOutput, TAS5825PCodec>;
 *   using AudioPolicy = AudioServicePolicy<Mixer>;
 *
 * Commands handled:
 *   AUDIO_PLAY        (0x84) — Play WAV file on channel
 *   AUDIO_STOP        (0x85) — Stop channel or all
 *   AUDIO_VOLUME      (0x86) — Set channel or master volume
 *   AUDIO_FADE        (0x87) — Fade-stop channel
 *   AUDIO_QUEUE       (0x88) — Queue sound on channel
 *   AUDIO_QUEUE_CLEAR (0x89) — Clear channel queue
 *   AUDIO_STATUS_REQ  (0x8A) — Request full audio status
 *   CODEC_STATUS_REQ  (0xAA) — Request codec hardware status (I2C, faults, volume)
 *
 * All playback commands use the mixer's Async methods (thread-safe
 * command queue) since protocol handling runs on Core 0 while I2S
 * output runs on Core 1.
 */

#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <serial/serial.h>
#include <serial/audio/audio_protocol.h>
#include <server/board_server.h>           // SystemServicePolicy + BoardServerBase
#include "audio/audio_config.h"
#include "audio/audio_mixer.h"
#include "audio/audio_ring_buffer.h"

/**
 * @brief Protocol server for HubFX audio commands.
 *
 * TMixer must provide (AudioMixer<TI2S, TCodec> satisfies all):
 *
 *   static TMixer& instance();
 *
 *   // Async playback control (thread-safe, Core 0 safe)
 *   bool playAsync(int ch, const char* file, const AudioPlaybackOptions& opts);
 *   void stopAsync(int ch, AudioStopMode mode);
 *   void setVolumeAsync(int ch, float vol);
 *   void setMasterVolumeAsync(float vol);
 *   bool queueSoundAsync(int ch, const char* file, const AudioPlaybackOptions& opts,
 *                        QueueLoopBehavior behavior);
 *   void clearQueueAsync(int ch);
 *
 *   // Status queries
 *   float masterVolume() const;
 *   bool isInitialized() const;
 *   bool isI2SRunning() const;
 *   bool isPlaying(int ch) const;
 *   bool isAnyPlaying() const;
 *   float remainingSec(int ch) const;
 *   int queueLength(int ch) const;
 *   float getChannelVolume(int ch) const;
 *   bool isLooping(int ch) const;
 *   int getLoopCount(int ch) const;
 *   AudioOutput getOutput(int ch) const;
 *   uint32_t getSampleRate(int ch) const;
 *   uint16_t getNumChannels(int ch) const;
 *   uint16_t getBitsPerSample(int ch) const;
 *   const char* getFilename(int ch) const;
 *
 *   // Ring buffer stats
 *   int getRingFillPercent() const;
 *   uint32_t getRingAvailableRead() const;
 *   uint32_t getRingAvailableWrite() const;
 *   uint32_t getUnderruns() const;
 *   uint32_t getConsumeLoops() const;
 *   uint32_t getConsumeFrames() const;
 *
 *   // Codec access
 *   auto& getCodec();  // must have getModelName()
 *
 * @tparam TMixer  Concrete AudioMixer type (e.g., AudioMixer<EspI2SOutput, TAS5825PCodec>)
 */
template <typename TMixer>
class AudioServicePolicy {
public:
    /// AudioServicePolicy advertises the AUDIO capability bit.
    static constexpr uint32_t kCapabilityBits = CoreCapability::AUDIO;

    AudioServicePolicy() = default;

    // ── SystemServicePolicy surface ───────────────────────────────────

    bool begin(sfx_core::BoardServerBase* ctx) {
        _ctx = ctx;
        return _ctx != nullptr;
    }

    bool ownsType(uint8_t type) const {
        // Range covers the audio block plus the CODEC_STATUS_REQ/RESP
        // pair which lives outside the contiguous 0x84..0x8B run.
        return (type >= AudioPacket::AUDIO_PLAY && type <= AudioPacket::AUDIO_STATUS_RESP)
            || (type == AudioPacket::CODEC_STATUS_REQ)
            || (type == AudioPacket::CODEC_STATUS_RESP);
    }

    CommandHandleResult handle(uint8_t type,
                               const uint8_t* payload, size_t len);

    void update() { /* mixer ticks itself on Core 1 */ }

    const char* getErrorMessage(uint8_t code) const {
        return AudioError::getMessage(code);
    }

protected:
    // Inline wire-helper wrappers — let existing handler code + the
    // SFX_REQUIRE_LEN / SFX_VALIDATE / SFX_DISPATCH macros work without
    // qualifying every send with `_ctx->`.  Each just forwards to the
    // BoardServerBase supplied at begin() time.
    int     sendAck()                                                       { return _ctx->sendAck(); }
    int     sendNack(uint8_t errorCode, const char* reason = nullptr)       { return _ctx->sendNack(errorCode, reason); }
    int     sendRawPacket(uint8_t type, uint8_t tag, const uint8_t* p = nullptr, size_t len = 0)
                                                                            { return _ctx->sendRawPacket(type, tag, p, len); }
    uint8_t currentTag() const                                              { return _ctx->currentTag(); }

private:
    sfx_core::BoardServerBase* _ctx = nullptr;

    /// @brief Get mixer singleton
    TMixer& mixer() { return TMixer::instance(); }

    // ---- Command handlers ----
    void handlePlay(const uint8_t* payload, size_t len);
    void handleStop(const uint8_t* payload, size_t len);
    void handleVolume(const uint8_t* payload, size_t len);
    void handleFade(const uint8_t* payload, size_t len);
    void handleQueue(const uint8_t* payload, size_t len);
    void handleQueueClear(const uint8_t* payload, size_t len);
    void handleStatusReq();
    void handleCodecStatusReq();
};

// ============================================================================
// Template Implementation
// ============================================================================

#include "audio_service.ipp"

#endif // AUDIO_SERVICE_H
