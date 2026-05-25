/*
 * audio_i2s.h — ESP-IDF native I2S TX (i2s_std driver).
 *
 * Wraps the modern `<driver/i2s_std.h>` API.  Single TX channel,
 * Philips standard mode (the format the TAS5825P expects), 16-bit
 * stereo, configurable sample rate.
 *
 * Production HubFX uses pins:
 *   DOUT=GP16  BCLK=GP17  LRCK=GP18
 *
 * `writeBlocking(frames, count)` blocks until the I2S DMA accepts
 * the bytes.  At 48 kHz stereo16 this paces the caller at exactly
 * 192 KB/s = the audio sample rate.
 */

#ifndef AUDIO_I2S_TEST_H
#define AUDIO_I2S_TEST_H

#include <cstdint>
#include <cstddef>

class AudioI2S {
public:
    static AudioI2S& instance() {
        static AudioI2S inst;
        return inst;
    }

    AudioI2S(const AudioI2S&)            = delete;
    AudioI2S& operator=(const AudioI2S&) = delete;

    /// Allocate the i2s_std TX channel, configure clock + slot, enable.
    /// `bclk`/`ws`/`dout` are GPIO numbers.  `sampleRate` = output Hz.
    /// Returns true on success.
    bool begin(int bclk, int ws, int dout, uint32_t sampleRate);

    /// Disable + free the channel.  Idempotent.
    void end();

    /// Write `frames` stereo16 frames (4 bytes each).  Blocks until
    /// DMA has accepted them.  Returns count actually written (may
    /// be < `frames` on timeout).
    size_t writeBlocking(const int16_t* interleavedStereo, size_t frames);

    /// Write `bytes` raw bytes (caller-formatted stereo16 LRLRLR).
    /// Wrapper for paths that already have PCM in the right layout.
    size_t writeBytesBlocking(const uint8_t* buf, size_t bytes);

    bool isRunning() const { return _running; }

private:
    AudioI2S() = default;
    void* _tx = nullptr;        // i2s_chan_handle_t — opaque to header
    bool  _running = false;
};

#endif  // AUDIO_I2S_TEST_H
