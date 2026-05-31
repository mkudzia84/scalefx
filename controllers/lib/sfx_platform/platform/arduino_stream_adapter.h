/*
 * arduino_stream_adapter.h — bridge an Arduino `::Stream` to `sfx::Stream`.
 *
 * Lets Arduino-owned transports (Pico's `Serial` wire; the ESP32 RC UART on
 * `Serial1`/`Serial2`) plug into the project-owned `sfx::Stream` interface that
 * the protocol layer now speaks.  This is the ONE place Arduino's `Stream`
 * leaks across the seam — everything upstream (BoardServer, DiagLog, the RC
 * input chain) sees only `sfx::Stream`.
 *
 * On ESP32 the wire itself is `NativeUartStream` (no adapter); the adapter is
 * used only for the RC UART until that path goes native too.  On Pico the wire
 * AND the RC UART both wrap Arduino `HardwareSerial` through this adapter.
 */

#ifndef SFX_ARDUINO_STREAM_ADAPTER_H
#define SFX_ARDUINO_STREAM_ADAPTER_H

#include "platform/sfx_platform.h"

// Only meaningful where an Arduino core is present (it provides ::Stream).
#if defined(ARDUINO) || SFX_PLATFORM_PICO || SFX_PLATFORM_ESP32

#include <Arduino.h>
#include "platform/sfx_stream.h"

namespace sfx {

class ArduinoStreamAdapter final : public Stream {
public:
    ArduinoStreamAdapter() = default;
    explicit ArduinoStreamAdapter(::Stream& s) : _s(&s) {}

    void bind(::Stream& s) { _s = &s; }
    ::Stream* underlying() const { return _s; }

    // ── write side ──────────────────────────────────────────────────────
    size_t write(uint8_t b) override { return _s ? _s->write(b) : 0; }
    size_t write(const uint8_t* buf, size_t n) override {
        return _s ? _s->write(buf, n) : 0;
    }
    void flush() override { if (_s) _s->flush(); }

    // ── read side ───────────────────────────────────────────────────────
    int available() override { return _s ? _s->available() : 0; }
    int read()      override { return _s ? _s->read() : -1; }
    int peek()      override { return _s ? _s->peek() : -1; }
    size_t readBytes(uint8_t* buffer, size_t length) override {
        return _s ? _s->readBytes(buffer, length) : 0;
    }
    using Stream::readBytes;   // keep the char* overload visible

    explicit operator bool() const override { return _s != nullptr; }

private:
    ::Stream* _s = nullptr;
};

}  // namespace sfx

#endif  // ARDUINO present
#endif  // SFX_ARDUINO_STREAM_ADAPTER_H
