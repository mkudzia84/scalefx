/*
 * pico_serial_stream.h — Arduino-stream → sfx::Stream adapter for the Pico.
 *
 * Why
 * ───
 * `BoardServer<TStream>` (and `BoardServerBaseT<TStream>`) require their wire
 * type to derive `sfx::Stream` — the project-owned, Arduino-free byte interface
 * (platform/sfx_stream.h) — because the templated mid-tier mirrors `&_stream`
 * into the non-template `sfx::Stream* _serial` base pointer.  On ESP32 that
 * type is `sfx::NativeUartStream` (native UART).  The Pico keeps the Arduino
 * framework (P8), so its wire to the hub is the USB-CDC `Serial` object — an
 * Arduino `Stream`, NOT an `sfx::Stream`.  This thin adapter bridges the two so
 * a Pico expander can instantiate `BoardOf<Board, PicoSerialStream<...>, …>`.
 *
 * It is the Pico analogue of NativeUartStream: same role (the wire), different
 * transport (USB-CDC via Arduino instead of native UART).  Templated on the
 * concrete Arduino stream type so `operator bool()` (the USB-CDC presence check
 * on earlephilhower's `SerialUSB`) forwards to the right object.
 *
 * Usage (Pico expander sketch):
 *   using WireStream = sfx::PicoSerialStream<std::remove_reference_t<decltype(Serial)>>;
 *   WireStream wireStream{Serial};
 *   class MyBoard : public sfx_core::BoardOf<MyBoard, WireStream, Caps…> { … };
 *   // setup(): Serial.begin(115200); board.begin(wireStream, ver, build, …);
 */

#ifndef SFX_PICO_SERIAL_STREAM_H
#define SFX_PICO_SERIAL_STREAM_H

#include "platform/sfx_platform.h"

#if SFX_PLATFORM_PICO

#include "platform/sfx_stream.h"   // sfx::Stream / sfx::Print
#include <Arduino.h>               // Arduino Stream / Serial (Pico keeps the framework)
#include <cstdint>
#include <cstddef>

namespace sfx {

/// Bridges an Arduino `Stream`/USB-CDC object (`TArduino`) to `sfx::Stream` so
/// the Arduino-free `BoardServer<TStream>` can drive it.  Holds a reference to
/// the Arduino object — does NOT own it.
template <typename TArduino>
class PicoSerialStream : public Stream {
public:
    explicit PicoSerialStream(TArduino& s) : _s(s) {}

    // ── Stream interface (read side) ─────────────────────────────────
    int available() override { return _s.available(); }
    int read()      override { return _s.read(); }
    int peek()      override { return _s.peek(); }

    /// Non-blocking bulk drain.  Deliberately does NOT delegate to Arduino's
    /// `Stream::readBytes` — that one blocks up to the stream's 1 s timeout
    /// waiting to fill the buffer, which would stall the COBS framer.  We drain
    /// only what's already buffered, matching the sfx::Stream contract.
    size_t readBytes(uint8_t* buffer, size_t length) override {
        size_t n = 0;
        while (n < length && _s.available()) {
            const int c = _s.read();
            if (c < 0) break;
            buffer[n++] = static_cast<uint8_t>(c);
        }
        return n;
    }
    using Stream::readBytes;   // un-hide the char* overload

    // ── Print interface (write side) ─────────────────────────────────
    size_t write(uint8_t b)                  override { return _s.write(b); }
    size_t write(const uint8_t* buf, size_t n) override { return _s.write(buf, n); }
    using Print::write;        // un-hide other write() overloads
    void flush() override { _s.flush(); }

    /// Always-live: once USB is enumerated the CDC endpoint can carry data
    /// regardless of whether the host (the HubFX master) asserts DTR, so we do
    /// NOT gate the wire on `(bool)Serial` (which tracks DTR on earlephilhower)
    /// — that would leave the framer mute against a host that never raises it.
    explicit operator bool() const override { return true; }

private:
    TArduino& _s;
};

}  // namespace sfx

#endif  // SFX_PLATFORM_PICO
#endif  // SFX_PICO_SERIAL_STREAM_H
