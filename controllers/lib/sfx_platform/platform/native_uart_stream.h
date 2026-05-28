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

    // ── Print interface (write side) ─────────────────────────────────
    size_t write(uint8_t b) override;
    size_t write(const uint8_t* buf, size_t n) override;
    using Print::write;     // un-hide other write() overloads

    /// Wait for any buffered TX to flush over the wire.
    void flush() override;

    /// Arduino-compatible `if (Serial)` truthiness — true while the
    /// driver is installed.
    explicit operator bool() const { return _installed; }

    uart_port_t port() const { return _port; }
    bool        installed() const { return _installed; }

private:
    uart_port_t _port      = UART_NUM_0;
    bool        _installed = false;
    int         _peeked    = -1;   ///< single-byte peek cache (-1 = empty)
};

}  // namespace sfx

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_NATIVE_UART_STREAM_H
