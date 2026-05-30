/*
 * native_uart_stream.cpp — implementation; see native_uart_stream.h
 * for the why.
 */

#include "native_uart_stream.h"

#if SFX_PLATFORM_ESP32

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>     // vTaskDelay — yield while waiting for TX-ring space

namespace sfx {

bool NativeUartStream::begin(uart_port_t port,
                             int rxPin, int txPin,
                             uint32_t baudRate,
                             size_t rxBufBytes,
                             size_t txBufBytes) {
    if (_installed) return true;
    _port = port;

    uart_config_t cfg = {};
    cfg.baud_rate  = (int)baudRate;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

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

int NativeUartStream::availableForWrite() {
    if (!_installed) return 0;
    size_t freeBytes = 0;
    if (uart_get_tx_buffer_free_size(_port, &freeBytes) != ESP_OK) return 0;
    return (int)freeBytes;
}

bool NativeUartStream::waitTxSpace(size_t n) {
    // Frame larger than the whole ring can never fit — caller must chunk; we
    // refuse rather than spin to the deadline.  (Protocol frames are ≤ ~600 B,
    // the ring is multiple KB, so this guards only against misuse.)
    size_t freeBytes = 0;
    if (uart_get_tx_buffer_free_size(_port, &freeBytes) != ESP_OK) return false;
    if (freeBytes >= n) return true;                 // common case: fits now
    // Ring is under pressure — poll briefly.  If the host is draining (6 Mbps),
    // space frees within a tick or two; if it's gone, we hit the deadline and
    // the caller drops the frame instead of wedging the loop.
    for (uint32_t i = 0; i < kTxWriteDeadlineMs; ++i) {
        vTaskDelay(1);                               // ~1 ms; lets the TX ISR drain
        if (uart_get_tx_buffer_free_size(_port, &freeBytes) == ESP_OK &&
            freeBytes >= n)
            return true;
    }
    return false;
}

size_t NativeUartStream::write(uint8_t b) {
    if (!_installed) return 0;
    if (!waitTxSpace(1)) return 0;                   // host not draining → drop
    const int written = uart_write_bytes(_port, (const char*)&b, 1);
    return (written < 0) ? 0 : (size_t)written;
}

size_t NativeUartStream::write(const uint8_t* buf, size_t n) {
    if (!_installed || !buf || n == 0) return 0;
    if (!waitTxSpace(n)) return 0;                   // host not draining → drop whole frame
    const int written = uart_write_bytes(_port, (const char*)buf, n);
    return (written < 0) ? 0 : (size_t)written;
}

void NativeUartStream::flush() {
    if (!_installed) return;
    uart_wait_tx_done(_port, portMAX_DELAY);
}

}  // namespace sfx

#endif  // SFX_PLATFORM_ESP32
