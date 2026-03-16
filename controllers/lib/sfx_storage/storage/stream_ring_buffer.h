/*
 * StreamRingBuffer — Lock-free SPSC ring buffer for streaming uploads
 *
 * Designed for ESP32-S3 dual-core streaming file transfers:
 *   Core 0 (producer): reads raw serial bytes into buffer
 *   Core 1 (consumer): reads from buffer, writes to SD card in large chunks
 *
 * Properties:
 *   - 1 MB capacity (PSRAM), power-of-2 for efficient modular arithmetic
 *   - Lock-free with std::atomic head/tail (release/acquire ordering)
 *   - memcpy with automatic wrap-around handling
 *   - Monotonically increasing indices — no overflow issues for TB of data
 *
 * Memory ordering (Rule 15):
 *   _head:  Core 0 writes (release), Core 1 reads (acquire)
 *   _tail:  Core 1 writes (release), Core 0 reads (acquire)
 *   Own-index reads use relaxed ordering (no cross-core barrier needed).
 *
 * Shared library component (controllers/lib/sfx_storage/).
 */

#ifndef STREAM_RING_BUFFER_H
#define STREAM_RING_BUFFER_H

#include <cstdint>
#include <cstring>
#include <atomic>
#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32
#include <esp_heap_caps.h>
#endif

class StreamRingBuffer {
public:
    /// Buffer capacity: 1 MB (must be power of 2)
    static constexpr size_t CAPACITY = 1 << 20;  // 1,048,576 bytes
    static constexpr size_t MASK = CAPACITY - 1;

    StreamRingBuffer() = default;

    /**
     * @brief Allocate buffer memory from PSRAM (ESP32) or heap (other)
     * @return true on success
     */
    bool allocate() {
        if (_buf) return true;  // Already allocated
#if SFX_PLATFORM_ESP32
        _buf = (uint8_t*)heap_caps_malloc(CAPACITY, MALLOC_CAP_SPIRAM);
#else
        _buf = (uint8_t*)malloc(CAPACITY);
#endif
        if (!_buf) return false;
        reset();
        return true;
    }

    /// Free buffer memory
    void release() {
        if (_buf) {
            free(_buf);
            _buf = nullptr;
        }
        reset();
    }

    /// Reset indices to zero (call before starting a new stream)
    void reset() {
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Write data into the ring buffer (Core 0 — producer)
     *
     * Copies up to `len` bytes from `data`. Returns the number of bytes
     * actually written. If insufficient space, writes what fits.
     *
     * @param data  Source data pointer
     * @param len   Number of bytes to write
     * @return      Bytes actually written (may be < len if buffer nearly full)
     */
    size_t write(const uint8_t* data, size_t len) {
        uint32_t h = _head.load(std::memory_order_relaxed);  // Own index (relaxed)
        uint32_t t = _tail.load(std::memory_order_acquire);  // Consumer's index

        size_t space = CAPACITY - (h - t);
        if (len > space) len = space;
        if (len == 0) return 0;

        uint32_t pos = h & MASK;
        size_t toEnd = CAPACITY - pos;
        if (toEnd >= len) {
            memcpy(_buf + pos, data, len);
        } else {
            memcpy(_buf + pos, data, toEnd);
            memcpy(_buf, data + toEnd, len - toEnd);
        }

        _head.store(h + len, std::memory_order_release);
        return len;
    }

    /**
     * @brief Read data from the ring buffer (Core 1 — consumer)
     *
     * Copies up to `maxLen` bytes into `dest`. Returns bytes actually read.
     * If buffer is empty, returns 0.
     *
     * @param dest    Destination buffer
     * @param maxLen  Maximum bytes to read
     * @return        Bytes actually read
     */
    size_t read(uint8_t* dest, size_t maxLen) {
        uint32_t t = _tail.load(std::memory_order_relaxed);  // Own index (relaxed)
        uint32_t h = _head.load(std::memory_order_acquire);  // Producer's index

        size_t avail = h - t;
        if (maxLen > avail) maxLen = avail;
        if (maxLen == 0) return 0;

        uint32_t pos = t & MASK;
        size_t toEnd = CAPACITY - pos;
        if (toEnd >= maxLen) {
            memcpy(dest, _buf + pos, maxLen);
        } else {
            memcpy(dest, _buf + pos, toEnd);
            memcpy(dest + toEnd, _buf, maxLen - toEnd);
        }

        _tail.store(t + maxLen, std::memory_order_release);
        return maxLen;
    }

    /// Number of bytes available to read (safe from either core)
    size_t used() const {
        uint32_t h = _head.load(std::memory_order_acquire);
        uint32_t t = _tail.load(std::memory_order_acquire);
        return h - t;
    }

    /// Free space available for writes (safe from either core)
    size_t freeSpace() const {
        return CAPACITY - used();
    }

    /// True if buffer has been allocated
    bool isAllocated() const { return _buf != nullptr; }

private:
    uint8_t* _buf = nullptr;
    std::atomic<uint32_t> _head{0};  // Core 0 writes, Core 1 reads (producer index)
    std::atomic<uint32_t> _tail{0};  // Core 1 writes, Core 0 reads (consumer index)

    // Non-copyable, non-movable
    StreamRingBuffer(const StreamRingBuffer&) = delete;
    StreamRingBuffer& operator=(const StreamRingBuffer&) = delete;
    StreamRingBuffer(StreamRingBuffer&&) = delete;
    StreamRingBuffer& operator=(StreamRingBuffer&&) = delete;
};

#endif // STREAM_RING_BUFFER_H
