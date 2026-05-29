/*
 * PacketReader — stateless byte-by-byte COBS frame accumulator.
 *
 *   Owns the "buffer N bytes, emit one frame per FRAME_DELIMITER"
 *   state machine that ALL three protocol consumers used to inline
 *   identically:
 *
 *     - SerialBus::process()             (client side, USB CDC)
 *     - BoardServerBase::readFrames()    (server side, Stream*)
 *     - BoardServer<TStream, ...>::readFrames()   (templated server)
 *
 *   Each consumer feeds raw bytes from its peripheral, the reader
 *   accumulates into an internal buffer, and on every encountered
 *   FRAME_DELIMITER (0x00) with the buffer non-empty it invokes
 *   onFrame(buf, len).  Overflow is recovered by dropping the
 *   in-progress frame and incrementing `framingErrors()`; the next
 *   delimiter re-syncs.
 *
 *   The frame buffer holds COBS-encoded bytes — the caller is
 *   responsible for `SfxWire::cobsDecode(frame, len, ...)` +
 *   `SfxWire::parsePacket(...)` on the emitted frame.  PacketReader
 *   stays at the "byte stream → frame" layer so the same class can
 *   be tested on the host without an Arduino, a USB stack, or a
 *   Stream — just feed bytes, observe frames.
 *
 *   Why a callback parameter instead of a stored std::function:
 *   std::function pulls in <functional> + heap allocations on some
 *   embedded RTSes.  Passing the callback as a template parameter
 *   per call lets the compiler inline it, with zero runtime overhead
 *   over the original hand-rolled loops.
 *
 *   Buffer size defaults to SfxWire::COBS_BUFFER_SIZE which is
 *   already MAX_PAYLOAD_SIZE-derived (512 on Pico, 2048 on ESP32) so
 *   the default matches what every existing consumer was using.
 *
 *   Thread safety: NONE.  PacketReader is single-producer.  The wire
 *   hot path on every controller already serialises onto one task
 *   (loopTask on ESP32, the Pico main loop).  Callers that need
 *   cross-task feeding wrap externally.
 */

#ifndef SFX_PACKET_READER_H
#define SFX_PACKET_READER_H

#include <cstddef>
#include <cstdint>
#include <utility>

#include "wire.h"   // SfxWire::FRAME_DELIMITER + COBS_BUFFER_SIZE

namespace sfx_serial {

template <std::size_t RxBufferSize>
class PacketReaderT {
public:
    PacketReaderT() = default;

    /// Feed `len` bytes from the wire.  For each completed frame (a
    /// non-empty run terminated by FRAME_DELIMITER) `onFrame(buf,
    /// frameLen)` is invoked synchronously; the `buf` pointer is
    /// only valid for the duration of the callback (subsequent
    /// feedBytes invalidates it).  Returns the number of frames
    /// emitted in this call.
    ///
    /// Run-on delimiters with an empty buffer are ignored (normal
    /// at session start and between idle bursts).
    ///
    /// Buffer overflow drops the in-progress frame and bumps
    /// `framingErrors()`.  The next delimiter re-syncs cleanly.
    template <typename FrameCallback>
    int feedBytes(const std::uint8_t* data, std::size_t len, FrameCallback&& onFrame) {
        int frames = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const std::uint8_t b = data[i];
            if (b == SfxWire::FRAME_DELIMITER) {
                if (_rxIndex > 0) {
                    onFrame(static_cast<const std::uint8_t*>(_rxBuffer), _rxIndex);
                    ++frames;
                }
                _rxIndex = 0;
            } else if (_rxIndex < RxBufferSize) {
                _rxBuffer[_rxIndex++] = b;
            } else {
                _rxIndex = 0;
                ++_framingErrors;
            }
        }
        return frames;
    }

    /// Single-byte variant — used by call sites that read from a
    /// `Stream` one byte at a time inside a `while (available())`
    /// loop.  Returns true when this byte completed a frame.
    template <typename FrameCallback>
    bool feedByte(std::uint8_t b, FrameCallback&& onFrame) {
        return feedBytes(&b, 1, std::forward<FrameCallback>(onFrame)) > 0;
    }

    /// Number of buffer-overflow events observed since construction
    /// (or the last reset of the counter — see resetStats()).
    std::uint32_t framingErrors() const { return _framingErrors; }

    /// Bytes currently held in the internal buffer (0 when idle, > 0
    /// when a partial frame is in flight).
    std::size_t bufferedBytes() const { return _rxIndex; }

    /// Discard any in-flight partial frame.  Does NOT zero the
    /// framing-error counter (use resetStats for that).
    void reset() { _rxIndex = 0; }

    /// Zero both the buffer and the error counter.  For diagnostics.
    void resetStats() { _rxIndex = 0; _framingErrors = 0; }

    static constexpr std::size_t bufferSize() { return RxBufferSize; }

private:
    std::uint8_t  _rxBuffer[RxBufferSize] = {};
    std::size_t   _rxIndex                 = 0;
    std::uint32_t _framingErrors           = 0;
};

/// Default alias matching the existing per-platform buffer size
/// (Pico 516, ESP32 2058 — `SfxWire::COBS_BUFFER_SIZE` does the
/// math).  All three pre-refactor inline copies used this size.
using PacketReader = PacketReaderT<SfxWire::COBS_BUFFER_SIZE>;

} // namespace sfx_serial

#endif // SFX_PACKET_READER_H
