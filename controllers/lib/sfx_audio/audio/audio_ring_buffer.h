/**
 * SPSC Ring Buffer for Audio Pipeline
 *
 * Singleton wrapper around SpscRingBuffer<StereoFrame, RING_FRAMES>
 * for the audio mixing pipeline.
 *
 * Core 0 writes (producer): WAV decode + float mixing → StereoFrame
 * Core 1 reads (consumer):  StereoFrame → I2S DMA
 *
 * Each "frame" = 1 left sample + 1 right sample = 4 bytes.
 * Buffer is dynamically allocated from PSRAM (ESP32-S3) or heap (Pico).
 */

#ifndef AUDIO_RING_BUFFER_H
#define AUDIO_RING_BUFFER_H

#include <cstdint>
#include "platform/sfx_platform.h"
#include "platform/spsc_ring_buffer.h"

// ============================================================================
// Configuration
// ============================================================================

// Ring buffer size as power of 2
// ESP32-S3: 65536 frames = 256 KB in PSRAM (~1.36s at 48 kHz).
//           Large buffer absorbs Core 0 stalls (SD reads, protocol processing).
// Pico (RP2350): 16384 frames = 64 KB (~341 ms at 48 kHz) — SRAM only.
#if SFX_PLATFORM_ESP32
static constexpr uint32_t RING_FRAMES_LOG2 = 16;                         // 65536 frames = 256 KB
#else
static constexpr uint32_t RING_FRAMES_LOG2 = 14;                         // 16384 frames = 64 KB
#endif
static constexpr uint32_t RING_FRAMES      = 1u << RING_FRAMES_LOG2;
static constexpr uint32_t RING_MASK        = RING_FRAMES - 1;
static constexpr uint32_t RING_BYTES       = RING_FRAMES * 2 * sizeof(int16_t);

// ============================================================================
// Types
// ============================================================================

struct StereoFrame {
    int16_t left;
    int16_t right;
};

// ============================================================================
// AudioRingBuffer — Singleton over SpscRingBuffer<StereoFrame, RING_FRAMES>
// ============================================================================

class AudioRingBuffer {
public:
    static AudioRingBuffer& instance() {
        static AudioRingBuffer inst;
        return inst;
    }

    // Delete copy/move
    AudioRingBuffer(const AudioRingBuffer&) = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;
    AudioRingBuffer(AudioRingBuffer&&) = delete;
    AudioRingBuffer& operator=(AudioRingBuffer&&) = delete;

    // ---- Lifecycle ----
    bool init()       { return _ring.init(); }
    void shutdown()   { _ring.shutdown(); }
    void reset()      { _ring.reset(); }
    bool isAllocated() const { return _ring.isAllocated(); }

    // ---- Status ----
    uint32_t availableRead()  const { return _ring.availableRead(); }
    uint32_t availableWrite() const { return _ring.availableWrite(); }
    int      fillPercent()    const { return _ring.fillPercent(); }
    bool     isEmpty()        const { return _ring.isEmpty(); }
    bool     isFull()         const { return _ring.isFull(); }

    // ---- Single-element ops ----
    void write(int16_t left, int16_t right) {
        _ring.write(StereoFrame{left, right});
    }
    void write(const StereoFrame& frame) { _ring.write(frame); }
    StereoFrame read() { return _ring.read(); }

private:
    AudioRingBuffer() = default;

    SpscRingBuffer<StereoFrame, RING_FRAMES> _ring;
};

#endif // AUDIO_RING_BUFFER_H
