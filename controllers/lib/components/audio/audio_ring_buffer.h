/**
 * SPSC Ring Buffer for Audio Pipeline
 *
 * Lock-free single-producer single-consumer ring buffer for stereo int16
 * sample frames. Core 0 writes (producer), Core 1 reads (consumer).
 *
 * Uses std::atomic with acquire/release ordering for cross-core visibility.
 * No mutexes in the hot path — only atomics.
 *
 * Each "frame" = 1 left sample + 1 right sample = 4 bytes.
 * Buffer size must be a power of 2 for fast modulo via bitmask.
 *
 * On ESP32-S3, the buffer is placed in internal SRAM (SFX_DMA_BUFFER)
 * for DMA-compatible, fast access from the I2S peripheral.
 */

#ifndef AUDIO_RING_BUFFER_H
#define AUDIO_RING_BUFFER_H

#include <Arduino.h>
#include <atomic>
#include "platform/sfx_platform.h"

// ============================================================================
// Configuration
// ============================================================================

// Ring buffer size as power of 2
// ESP32-S3 has much more SRAM so we double the buffer for underrun resistance.
// Pico: 16384 frames = 64 KB (~341 ms at 48 kHz)
// ESP32: 32768 frames = 128 KB (~683 ms at 48 kHz)
#if SFX_PLATFORM_ESP32
static constexpr int RING_FRAMES_LOG2 = 15;                              // 32768 frames
#else
static constexpr int RING_FRAMES_LOG2 = 14;                              // 16384 frames
#endif
static constexpr int RING_FRAMES      = 1 << RING_FRAMES_LOG2;
static constexpr int RING_MASK        = RING_FRAMES - 1;
static constexpr int RING_BYTES       = RING_FRAMES * 2 * sizeof(int16_t);

// ============================================================================
// Types
// ============================================================================

struct StereoFrame {
    int16_t left;
    int16_t right;
};

// ============================================================================
// AudioRingBuffer Class (Singleton)
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

    /**
     * Reset read and write indices (call only when both cores are idle)
     */
    void reset() {
        _writeIdx.store(0, std::memory_order_release);
        _readIdx.store(0, std::memory_order_release);
    }

    /**
     * Number of frames available for reading (consumer side)
     */
    uint32_t availableRead() const {
        uint32_t w = _writeIdx.load(std::memory_order_acquire);
        uint32_t r = _readIdx.load(std::memory_order_relaxed);  // own index
        return (w - r);  // works with wrapping unsigned arithmetic
    }

    /**
     * Number of frames available for writing (producer side)
     */
    uint32_t availableWrite() const {
        uint32_t w = _writeIdx.load(std::memory_order_relaxed);  // own index
        uint32_t r = _readIdx.load(std::memory_order_acquire);
        return RING_FRAMES - (w - r);
    }

    /**
     * Write one stereo frame to the ring buffer (producer side)
     * Caller must ensure availableWrite() > 0 before calling
     */
    void write(int16_t left, int16_t right) {
        uint32_t w = _writeIdx.load(std::memory_order_relaxed);  // own index
        _buffer[w & RING_MASK].left  = left;
        _buffer[w & RING_MASK].right = right;
        _writeIdx.store(w + 1, std::memory_order_release);
    }

    /**
     * Write one stereo frame from a StereoFrame struct
     */
    void write(const StereoFrame& frame) {
        write(frame.left, frame.right);
    }

    /**
     * Read one stereo frame from the ring buffer (consumer side)
     * Caller must ensure availableRead() > 0 before calling
     */
    StereoFrame read() {
        uint32_t r = _readIdx.load(std::memory_order_relaxed);  // own index
        StereoFrame f = _buffer[r & RING_MASK];
        _readIdx.store(r + 1, std::memory_order_release);
        return f;
    }

    /**
     * Get fill level as percentage (0-100)
     */
    int fillPercent() const {
        return (availableRead() * 100) / RING_FRAMES;
    }

    /**
     * Buffer is empty (underrun risk)
     */
    bool isEmpty() const {
        return availableRead() == 0;
    }

    /**
     * Buffer is full (producer must wait)
     */
    bool isFull() const {
        return availableWrite() == 0;
    }

private:
    AudioRingBuffer() = default;

    // On ESP32-S3, SFX_DMA_BUFFER places the buffer in internal SRAM
    // for DMA-compatible, low-latency access from the I2S peripheral.
    // On Pico/Pico 2, SFX_DMA_BUFFER is a no-op (all RAM is DMA-capable).
    SFX_DMA_BUFFER StereoFrame _buffer[RING_FRAMES];

    // SPSC indices — only written by one core each
    std::atomic<uint32_t> _writeIdx{0};  // Core 0 writes, Core 1 reads
    std::atomic<uint32_t> _readIdx{0};   // Core 1 writes, Core 0 reads
};

#endif // AUDIO_RING_BUFFER_H
