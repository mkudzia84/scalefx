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

}  // namespace sfx

#endif  // SFX_PLATFORM_ESP32
