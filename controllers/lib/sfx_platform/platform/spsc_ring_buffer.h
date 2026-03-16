/**
 * SpscRingBuffer — Generic lock-free single-producer single-consumer ring buffer
 *
 * Template parameters:
 *   T        — Element type (e.g., StereoFrame, uint8_t)
 *   CAPACITY — Number of elements. MUST be a power of 2 for fast modular
 *              arithmetic via bitmask.
 *
 * Memory ordering (Rule 15 — cross-core safety):
 *   _writeIdx: Producer writes (release), consumer reads (acquire)
 *   _readIdx:  Consumer writes (release), producer reads (acquire)
 *   Own-index reads use relaxed ordering (no cross-core barrier needed).
 *
 * Buffer is dynamically allocated via init() from PSRAM (ESP32-S3) or
 * standard heap (Pico). Call shutdown() to free.
 *
 * Supports both per-element and bulk (memcpy) operations:
 *   - write(const T&) / read() — single element (audio frames)
 *   - writeBulk(ptr, n) / readBulk(ptr, n) — memcpy with wrap (byte streams)
 *
 * Both producers and consumers are non-blocking: callers check
 * availableWrite()/availableRead() before operating.
 *
 * Shared library component (controllers/lib/sfx_platform/).
 */

#ifndef SPSC_RING_BUFFER_H
#define SPSC_RING_BUFFER_H

#include <cstdint>
#include <cstring>
#include <atomic>
#include "sfx_platform.h"

template<typename T, uint32_t CAPACITY>
class SpscRingBuffer {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of 2");
    static_assert(CAPACITY > 0, "CAPACITY must be > 0");

public:
    /// Compile-time constants
    static constexpr uint32_t MASK = CAPACITY - 1;
    static constexpr uint32_t SIZE_BYTES = CAPACITY * sizeof(T);

    SpscRingBuffer() = default;

    ~SpscRingBuffer() {
        shutdown();
    }

    // Non-copyable, non-movable
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Allocate buffer from PSRAM (ESP32) or heap.
     * Idempotent — safe to call multiple times.
     * @return true on success
     */
    bool init() {
        if (_buf) return true;
        _buf = static_cast<T*>(sfxPsramCalloc(CAPACITY, sizeof(T)));
        if (!_buf) return false;
        reset();
        return true;
    }

    /**
     * @brief Free buffer memory. Call only when both sides are idle.
     */
    void shutdown() {
        if (_buf) {
            sfxPsramFree(_buf);
            _buf = nullptr;
        }
        reset();
    }

    /**
     * @brief Reset indices to zero. Both sides must be idle.
     */
    void reset() {
        _writeIdx.store(0, std::memory_order_relaxed);
        _readIdx.store(0, std::memory_order_relaxed);
    }

    /// True if buffer has been allocated
    bool isAllocated() const { return _buf != nullptr; }

    // ========================================================================
    // Single-Element Operations
    // ========================================================================

    /**
     * @brief Write one element (producer side).
     * Caller must ensure availableWrite() > 0.
     */
    void write(const T& item) {
        uint32_t w = _writeIdx.load(std::memory_order_relaxed);
        _buf[w & MASK] = item;
        _writeIdx.store(w + 1, std::memory_order_release);
    }

    /**
     * @brief Read one element (consumer side).
     * Caller must ensure availableRead() > 0.
     */
    T read() {
        uint32_t r = _readIdx.load(std::memory_order_relaxed);
        T item = _buf[r & MASK];
        _readIdx.store(r + 1, std::memory_order_release);
        return item;
    }

    // ========================================================================
    // Bulk Operations (memcpy with wrap-around)
    // ========================================================================

    /**
     * @brief Write up to `count` elements from `src` (producer side).
     * @return Number of elements actually written (may be < count if full).
     */
    uint32_t writeBulk(const T* src, uint32_t count) {
        uint32_t w = _writeIdx.load(std::memory_order_relaxed);
        uint32_t r = _readIdx.load(std::memory_order_acquire);

        uint32_t space = CAPACITY - (w - r);
        if (count > space) count = space;
        if (count == 0) return 0;

        uint32_t pos = w & MASK;
        uint32_t toEnd = CAPACITY - pos;
        if (toEnd >= count) {
            memcpy(_buf + pos, src, count * sizeof(T));
        } else {
            memcpy(_buf + pos, src, toEnd * sizeof(T));
            memcpy(_buf, src + toEnd, (count - toEnd) * sizeof(T));
        }

        _writeIdx.store(w + count, std::memory_order_release);
        return count;
    }

    /**
     * @brief Read up to `maxCount` elements into `dest` (consumer side).
     * @return Number of elements actually read (may be < maxCount if empty).
     */
    uint32_t readBulk(T* dest, uint32_t maxCount) {
        uint32_t r = _readIdx.load(std::memory_order_relaxed);
        uint32_t w = _writeIdx.load(std::memory_order_acquire);

        uint32_t avail = w - r;
        if (maxCount > avail) maxCount = avail;
        if (maxCount == 0) return 0;

        uint32_t pos = r & MASK;
        uint32_t toEnd = CAPACITY - pos;
        if (toEnd >= maxCount) {
            memcpy(dest, _buf + pos, maxCount * sizeof(T));
        } else {
            memcpy(dest, _buf + pos, toEnd * sizeof(T));
            memcpy(dest + toEnd, _buf, (maxCount - toEnd) * sizeof(T));
        }

        _readIdx.store(r + maxCount, std::memory_order_release);
        return maxCount;
    }

    // ========================================================================
    // Status Queries (safe from either core)
    // ========================================================================

    /// Elements available for reading
    uint32_t availableRead() const {
        uint32_t w = _writeIdx.load(std::memory_order_acquire);
        uint32_t r = _readIdx.load(std::memory_order_acquire);
        return w - r;
    }

    /// Elements available for writing
    uint32_t availableWrite() const {
        uint32_t w = _writeIdx.load(std::memory_order_acquire);
        uint32_t r = _readIdx.load(std::memory_order_acquire);
        return CAPACITY - (w - r);
    }

    /// Fill level as percentage (0–100)
    int fillPercent() const {
        return static_cast<int>((availableRead() * 100u) / CAPACITY);
    }

    /// Buffer is empty (underrun risk)
    bool isEmpty() const { return availableRead() == 0; }

    /// Buffer is full (producer must wait)
    bool isFull() const { return availableWrite() == 0; }

    /// Capacity in elements
    static constexpr uint32_t capacity() { return CAPACITY; }

    /// Capacity in bytes
    static constexpr uint32_t capacityBytes() { return SIZE_BYTES; }

private:
    T* _buf = nullptr;
    std::atomic<uint32_t> _writeIdx{0};  // Producer writes, consumer reads
    std::atomic<uint32_t> _readIdx{0};   // Consumer writes, producer reads
};

#endif // SPSC_RING_BUFFER_H
