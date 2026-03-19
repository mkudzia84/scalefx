/*
 * Audio Protocol Server — Template for Audio Command Handling
 *
 * AudioServerT<TMixer> is a BusServer subclass that handles the HubFX
 * audio control protocol (0x84-0x8B). It translates wire-format commands
 * into AudioMixer async API calls, providing thread-safe audio control
 * from the serial protocol layer.
 *
 * The server is parameterized by the mixer type so it works with any
 * AudioMixer<TI2S, TCodec> instantiation without hard-coding the
 * I2S/codec types.
 *
 * Usage:
 *   // In controller firmware:
 *   using Mixer = AudioMixer<EspI2SOutput, SimpleI2SCodec>;
 *   AudioServerT<Mixer> audioServer;
 *   audioServer.begin(&Serial);
 *   server.addModuleHandler(&audioServer);
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

#ifndef AUDIO_SERVER_H
#define AUDIO_SERVER_H

#include <serial/serial.h>
#include <serial/hubfx/hubfx.h>
#include "audio/audio_config.h"

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
 *   int remainingMs(int ch) const;
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
 * @tparam TMixer  Concrete AudioMixer type (e.g., AudioMixer<EspI2SOutput, SimpleI2SCodec>)
 */
template <typename TMixer>
class AudioServerT : public BusServer {
public:
    AudioServerT() = default;

    const char* handlerName() const override { return "AudioServer"; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type,
                                           const uint8_t* payload,
                                           size_t len) override;

    uint8_t moduleRangeLow()  const override { return HubFxPacket::AUDIO_PLAY; }
    uint8_t moduleRangeHigh() const override { return HubFxPacket::CODEC_STATUS_RESP; }

    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

private:
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

#include "audio_server.ipp"

#endif // AUDIO_SERVER_H
