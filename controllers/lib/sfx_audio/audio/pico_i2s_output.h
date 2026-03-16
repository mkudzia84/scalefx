/**
 * Pico I2S Output — RP2040/RP2350 Implementation
 *
 * Wraps the Arduino-Pico I2S library (PIO-based). Uses per-sample
 * write16() since the Pico I2S library doesn't expose a bulk write
 * path. Satisfies the TI2S template concept for AudioMixer<TI2S, TCodec>.
 *
 * NOTE: On RP2040/RP2350, LRCLK must be BCLK+1 (PIO I2S constraint).
 *       This is validated in begin().
 */

#ifndef PICO_I2S_OUTPUT_H
#define PICO_I2S_OUTPUT_H

#include "audio_ring_buffer.h"
#include "audio_config.h"

#if defined(ARDUINO_ARCH_RP2040)

#include <I2S.h>
#include "audio_log.h"

class PicoI2SOutput {
public:
    static PicoI2SOutput& instance() {
        static PicoI2SOutput inst;
        return inst;
    }

    // Delete copy/move
    PicoI2SOutput(const PicoI2SOutput&) = delete;
    PicoI2SOutput& operator=(const PicoI2SOutput&) = delete;
    PicoI2SOutput(PicoI2SOutput&&) = delete;
    PicoI2SOutput& operator=(PicoI2SOutput&&) = delete;

    bool begin(const I2SPinConfig& pins, uint32_t sampleRate, uint8_t bitDepth) {
        if (_running) return true;

        // RP2040/RP2350 PIO I2S requires LRCLK = BCLK + 1 (adjacent GPIOs)
        if (pins.lrclkPin != pins.bclkPin + 1) {
            MIXER_WARN("LRCLK (GP%d) is not BCLK+1 (GP%d) — RP2 I2S PIO requires adjacent pins!",
                       pins.lrclkPin, pins.bclkPin);
        }

        _i2s.setBCLK(pins.bclkPin);
        _i2s.setDATA(pins.dataPin);
        _i2s.setBitsPerSample(bitDepth);
        _i2s.setBuffers(8, 1024);  // 8 DMA buffers × 1024 bytes = 8 KB

        if (!_i2s.begin(sampleRate)) {
            MIXER_ERROR("Pico I2S init failed (rate=%u, bits=%d, data=GP%d, bclk=GP%d)",
                        sampleRate, bitDepth, pins.dataPin, pins.bclkPin);
            return false;
        }

        _running = true;
        return true;
    }

    void end() {
        if (!_running) return;
        _i2s.end();
        _running = false;
    }

    size_t writeSamples(const StereoFrame* frames, size_t count) {
        for (size_t i = 0; i < count; i++) {
            _i2s.write16(frames[i].left, frames[i].right);
        }
        return count;
    }

    void writeSilence() {
        _i2s.write16(0, 0);
    }

    bool isRunning() const { return _running; }

    const char* backendName() const { return "Pico-PIO-I2S"; }

private:
    PicoI2SOutput() : _i2s(OUTPUT), _running(false) {}

    I2S  _i2s;
    bool _running;
};

#endif // ARDUINO_ARCH_RP2040
#endif // PICO_I2S_OUTPUT_H
