/*
 * native_uart_stream.h — Stream-compatible wrapper around the native
 * ESP-IDF UART driver, used in place of Arduino's `Serial` when the
 * firmware runs under the Arduino-as-IDF-component build path.
 *
 * Why
 * ───
 * arduino-esp32's `HardwareSerial` works fine under the regular
 * `framework = arduino` build because the pioarduino-bundled
 * pre-built libraries were compiled with sdkconfig knobs that match
 * Arduino's expected runtime.  Under `framework = arduino` + a
 * non-empty `custom_sdkconfig` (the auto-IDF-component path),
 * arduino-esp32 is rebuilt FROM SOURCE against OUR sdkconfig — and
 * something in that rebuild silently breaks the UART RX path on
 * UART0.  TX still works (boot text + log frames stream to the host),
 * but `Serial.available()` always returns 0 and `Serial.read()`
 * always returns -1 even when the host is writing.  Result: the
 * COBS protocol parser in `BoardServer::process()` never sees host
 * IDENTIFY / command requests, IDENTIFY round-trip times out, and
 * the device looks bricked.
 *
 * A native test sketch using `uart_driver_install` + `uart_read_bytes`
 * directly on UART0 (no Arduino wrapping) at the same 6 Mbps baud
 * receives bytes perfectly — confirming the chip, USB-UART bridge,
 * and IDF driver stack all work.  The bug is specifically in
 * arduino-esp32's wrapping.
 *
 * Rather than continue debugging Arduino's HAL, we own the UART
 * directly via this thin class.  It implements the same Arduino
 * `Stream` interface our existing protocol consumers (BoardServer,
 * DiagLog, StreamWriter, …) expect, so the swap is a one-liner at
 * the BoardServer::begin() call site.
 *
 * Pico
 * ────
 * Not needed.  On RP2040/RP2350 we keep using Arduino's Serial via
 * the upstream framework; this header is platform-gated and the
 * class is only defined when `SFX_PLATFORM_ESP32` is set.
 */

#ifndef SFX_NATIVE_UART_STREAM_H
#define SFX_NATIVE_UART_STREAM_H

#include "platform/sfx_platform.h"

#if SFX_PLATFORM_ESP32

#include <Arduino.h>          // brings in Stream + Print abstract bases
#include <driver/uart.h>
#include <esp_err.h>
#include <cstdint>
#include <cstddef>

namespace sfx {

class NativeUartStream : public Stream {
public:
    /// Idempotent driver install.  Re-calling `begin()` on an already-
    /// installed port is a no-op and returns true.  Safe to call
    /// before any other UART consumer — installs at boot and stays up
    /// for the device's lifetime.
    bool begin(uart_port_t port,
               int rxPin, int txPin,
               uint32_t baudRate,
               size_t rxBufBytes = 8192,
               size_t txBufBytes = 8192);

    /// Free the driver.  Rarely needed (the UART stays up across the
    /// whole runtime); provided for symmetry / hot-reload tests.
    void end();

    // ── Stream interface (read side) ─────────────────────────────────
    int available() override;
    int read() override;
    int peek() override;

    /// Bulk read — bypass Arduino's `Stream::readBytes` polyfill (which
    /// calls `read()` N times = 1 IDF mutex round-trip per byte ≈
    /// 150 ms for 16 KB at 6 Mbps).  This single call drains the UART
    /// in one shot, ~µs for the same 16 KB.  Critical on the stream-
    /// upload hot path where the loop iteration time dominates the
    /// effective UART drain rate (build #491 diag, 2026-05-28).
    size_t readBytes(uint8_t* buffer, size_t length);
    size_t readBytes(char* buffer, size_t length) {
        return readBytes(reinterpret_cast<uint8_t*>(buffer), length);
    }

    // ── Print interface (write side) ─────────────────────────────────
    // Writes are BOUNDED-NON-BLOCKING: a frame goes out only if it fits in the
    // TX ring within `kTxWriteDeadlineMs` (so a legitimately fast stream whose
    // host IS draining still gets sent whole), otherwise the WHOLE frame is
    // dropped.  A UART has no DTR, so when the host closes the port nothing
    // drains the ring; a plain blocking `uart_write_bytes` would then wedge the
    // main loop (and, across reconnects, brick the device until replug).
    // Dropping a whole COBS frame is safe — the host re-syncs on the next
    // delimiter; a PARTIAL frame would corrupt the stream, so we never write one.
    size_t write(uint8_t b) override;
    size_t write(const uint8_t* buf, size_t n) override;
    using Print::write;     // un-hide other write() overloads

    /// Free space (bytes) in the TX ring right now — lets callers skip a write
    /// that won't fit instead of blocking.  0 when the host isn't draining.
    int availableForWrite() override;

    /// Wait for any buffered TX to flush over the wire.
    void flush() override;

    /// Arduino-compatible `if (Serial)` truthiness — true while the
    /// driver is installed.
    explicit operator bool() const { return _installed; }

    uart_port_t port() const { return _port; }
    bool        installed() const { return _installed; }

private:
    /// Block at most this long waiting for TX-ring space before dropping a
    /// frame.  Long enough that a host draining at 6 Mbps never loses data to
    /// transient ring pressure; short enough that a gone host can't stall the
    /// main loop into a wedge.
    static constexpr uint32_t kTxWriteDeadlineMs = 20;

    /// True once the ring has room for `n` bytes (polling up to the deadline);
    /// false if the host isn't draining and the frame should be dropped.
    bool waitTxSpace(size_t n);

    uart_port_t _port      = UART_NUM_0;
    bool        _installed = false;
    int         _peeked    = -1;   ///< single-byte peek cache (-1 = empty)
};

}  // namespace sfx

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_NATIVE_UART_STREAM_H
