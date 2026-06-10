/*
 * sfx_stream.h — project-owned byte-stream interface (no Arduino).
 *
 * `sfx::Stream` / `sfx::Print` are a minimal, Arduino-free re-implementation
 * of the two Arduino base classes the ScaleFX protocol layer used to lean on
 * (the wire to the host, DiagLog, the RC UART chain).  Keeping our own tiny
 * interface — instead of pulling Arduino for its `Stream`/`Print` — is what
 * lets the firmware drop the Arduino framework dependency on ESP32.
 *
 * Surface = exactly what the protocol consumers call (verified by grep): the
 * write side (write byte / write buffer / flush) on `Print`, plus the read side
 * (available / read / peek / readBytes) and an `if (stream)` truthiness on
 * `Stream`.  No `print()` / `println()` / `printf()` — the wire is binary COBS.
 *
 * Concrete implementation: sfx::NativeUartStream (ESP32 native UART —
 * native_uart_stream.h) is used for BOTH the host wire (UART0) and the RC
 * UART (UART1/2 SBUS/Jeti, via EspInputPort).
 */

#ifndef SFX_STREAM_H
#define SFX_STREAM_H

#include <cstdint>
#include <cstddef>

namespace sfx {

// ── Write side (mirror of Arduino Print, minimal) ───────────────────────────
class Print {
public:
    virtual ~Print() = default;

    /// Write a single byte.  Returns bytes written (0 or 1).
    virtual size_t write(uint8_t b) = 0;

    /// Write a buffer.  Default loops single-byte write(); implementations
    /// override with a bulk path.  Returns bytes written.
    virtual size_t write(const uint8_t* buf, size_t n) {
        size_t w = 0;
        while (w < n) {
            if (write(buf[w]) != 1) break;
            ++w;
        }
        return w;
    }

    /// Block until any buffered TX has gone out on the wire.
    virtual void flush() {}
};

// ── Read + write side (mirror of Arduino Stream, minimal) ───────────────────
class Stream : public Print {
public:
    /// Bytes available to read without blocking.
    virtual int available() = 0;

    /// Read one byte (-1 if none available).
    virtual int read() = 0;

    /// Peek the next byte without consuming it (-1 if none).
    virtual int peek() = 0;

    /// Bulk read up to `length` bytes.  Default loops read(); the native UART
    /// overrides with a single driver drain (critical on the upload hot path).
    virtual size_t readBytes(uint8_t* buffer, size_t length) {
        size_t n = 0;
        while (n < length) {
            const int c = read();
            if (c < 0) break;
            buffer[n++] = static_cast<uint8_t>(c);
        }
        return n;
    }
    size_t readBytes(char* buffer, size_t length) {
        return readBytes(reinterpret_cast<uint8_t*>(buffer), length);
    }

    /// Discard everything currently in the RX buffer (driver ring + HW FIFO).
    /// Default no-op; the native UART overrides with uart_flush_input.  Used by
    /// the half-duplex Jeti responder to drop its own TX self-echo in one shot
    /// (incl. the in-flight FIFO tail) during the master's silent reply slot.
    virtual void flushRx() {}

    /// `if (stream)` truthiness — true while the underlying transport is live.
    virtual explicit operator bool() const { return true; }
};

}  // namespace sfx

#endif  // SFX_STREAM_H
