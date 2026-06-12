/*
 * native_uart_stream.cpp — implementation; see native_uart_stream.h
 * for the why.
 */

#include "native_uart_stream.h"

#if SFX_PLATFORM_ESP32

namespace sfx {

bool NativeUartStream::begin(uart_port_t port,
                             int rxPin, int txPin,
                             uint32_t baudRate,
                             size_t rxBufBytes,
                             size_t txBufBytes) {
    uart_config_t cfg = {};
    cfg.baud_rate  = (int)baudRate;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    return beginConfig(port, rxPin, txPin, cfg, /*invert=*/0,
                       /*rs485=*/false, rxBufBytes, txBufBytes);
}

bool NativeUartStream::beginConfig(uart_port_t port,
                                   int rxPin, int txPin,
                                   const uart_config_t& cfg,
                                   uint32_t lineInverseMask,
                                   bool     rs485HalfDuplex,
                                   size_t   rxBufBytes,
                                   size_t   txBufBytes) {
    if (_installed) return true;
    _port = port;

    // Driver install first — sizes the RX / TX ring buffers in internal
    // SRAM (ISR-safe).  Pass 0 as the event-queue length; we poll, no
    // event task needed.
    esp_err_t err = uart_driver_install(_port,
                                        (int)rxBufBytes,
                                        (int)txBufBytes,
                                        0, nullptr, 0);
    if (err != ESP_OK) return false;

    err = uart_param_config(_port, &cfg);
    if (err != ESP_OK) {
        uart_driver_delete(_port);
        return false;
    }

    err = uart_set_pin(_port, txPin, rxPin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        uart_driver_delete(_port);
        return false;
    }

    // Flush the RX hardware FIFO to the driver ring promptly even when the
    // sender pauses mid-stream.  available() reads uart_get_buffered_data_len(),
    // which counts ONLY the driver ring — bytes still in the 128-byte HW FIFO
    // are invisible until the ISR moves them, which it does on either a
    // rx_full_threshold crossing OR a rx_timeout idle.  The stream-upload client
    // blasts a 16 KB segment then waits for the ACK, so the segment's tail bytes
    // (fewer than the threshold) sit in the FIFO with NO further bytes to cross
    // the threshold; if the idle-timeout flush is too lax they never reach the
    // ring, available() reports 0, the segment never completes, and the firmware
    // aborts on the 5 s inactivity timer.  Observed: 86/121 MB uploads dying
    // exactly 12 bytes short of a random segment boundary with the SD perfectly
    // healthy (avg 4 ms / max ~98 ms writes), 2026-05-31.  A low full-threshold
    // (also keeps the FIFO well clear of overrun during a blocking SD write) +
    // a short symbol-period idle-timeout guarantee the tail reaches the ring
    // within microseconds.  Best-effort: failures here are non-fatal.
    uart_set_rx_full_threshold(_port, 64);
    uart_set_rx_timeout(_port, 10);

    // SBUS inverts RXD; Jeti / wire do not.
    if (lineInverseMask) uart_set_line_inverse(_port, lineInverseMask);
    // Optional hardware RS-485 half-duplex (Jeti uses MANUAL matrix toggling
    // via EspInputPort::txEnable/txDisable instead, so it passes false).
    if (rs485HalfDuplex) uart_set_mode(_port, UART_MODE_RS485_HALF_DUPLEX);

    _installed = true;
    _peeked    = -1;
    return true;
}

void NativeUartStream::end() {
    if (!_installed) return;
    uart_driver_delete(_port);
    _installed = false;
    _peeked    = -1;
}

int NativeUartStream::available() {
    if (!_installed) return 0;
    size_t buffered = 0;
    uart_get_buffered_data_len(_port, &buffered);
    return (int)buffered + (_peeked >= 0 ? 1 : 0);
}

int NativeUartStream::read() {
    if (!_installed) return -1;
    if (_peeked >= 0) {
        const int v = _peeked;
        _peeked = -1;
        return v;
    }
    uint8_t b = 0;
    const int n = uart_read_bytes(_port, &b, 1, 0);
    return (n == 1) ? (int)b : -1;
}

int NativeUartStream::peek() {
    if (!_installed) return -1;
    if (_peeked >= 0) return _peeked;
    uint8_t b = 0;
    const int n = uart_read_bytes(_port, &b, 1, 0);
    if (n == 1) {
        _peeked = (int)b;
        return (int)b;
    }
    return -1;
}

size_t NativeUartStream::readBytes(uint8_t* buffer, size_t length) {
    if (!_installed || !buffer || length == 0) return 0;
    size_t consumed = 0;

    // Honour any pre-peeked byte first so the caller's stream is
    // continuous (Stream::peek() may have stashed one byte that
    // readBytes() must return as the very first byte).
    if (_peeked >= 0) {
        buffer[0] = (uint8_t)_peeked;
        _peeked = -1;
        consumed = 1;
        if (consumed == length) return consumed;
    }

    // Bulk drain — zero ticks of timeout: take whatever the driver has
    // queued right now and return.  The caller (processStream) has
    // already done its own avail() check, so we never ask for more
    // bytes than the ring buffer holds.
    const int n = uart_read_bytes(_port,
                                  buffer + consumed,
                                  length - consumed,
                                  0);
    if (n > 0) consumed += (size_t)n;
    return consumed;
}

size_t NativeUartStream::write(uint8_t b) {
    if (!_installed) return 0;
    return (size_t)uart_write_bytes(_port, (const char*)&b, 1);
}

size_t NativeUartStream::write(const uint8_t* buf, size_t n) {
    if (!_installed || !buf || n == 0) return 0;
    const int written = uart_write_bytes(_port, (const char*)buf, n);
    return (written < 0) ? 0 : (size_t)written;
}

void NativeUartStream::flush() {
    if (!_installed) return;
    uart_wait_tx_done(_port, portMAX_DELAY);
}

void NativeUartStream::flushRx() {
    if (!_installed) return;
    _peeked = -1;                 // drop the cached peek byte too
    uart_flush_input(_port);      // clears the driver RX ring + HW FIFO
}

}  // namespace sfx

#endif  // SFX_PLATFORM_ESP32
