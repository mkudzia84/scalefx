/*
 * esp_input_port.h — ESP32-S3 implementation of `InputPort`.
 *
 * Wraps one GPIO + one ESP32 UART peripheral.  Modes:
 *   - PULSE      : edge-IRQ pulse capture via `PwmInput::beginAsync`
 *   - SBUS       : `HardwareSerial.begin(100000, SERIAL_8E2, RX, -1, invert=true)`
 *   - JETI_EX    : `HardwareSerial.begin(baud, SERIAL_8N1, RX, RX)` +
 *                  `uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)` so the
 *                  same wire auto-flips between RX and TX for the
 *                  master/slave time-slot exchange.
 *   - UART_RAW   : user-specified params (CRSF, DSM, ...).
 *
 * Mode switches release the previously-active peripheral before
 * claiming the new one (so toggling between PULSE and a UART mode
 * doesn't leak interrupts or block the UART driver).
 *
 * The constructor binds this port to one specific UART number (1 or 2
 * — UART0 is the console).  The board declares one EspInputPort per
 * UART it wants to dedicate to input; Rule 31 caps the total count.
 */

#ifndef SFX_ESP_INPUT_PORT_H
#define SFX_ESP_INPUT_PORT_H

#include <platform/sfx_platform.h>

#if !SFX_PLATFORM_ESP32
#  error "EspInputPort is ESP32-only.  Use a Pico-specific driver on RP2040/RP2350."
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
#include <driver/uart.h>          // uart_set_mode + UART_MODE_RS485_HALF_DUPLEX
#include <cstdint>

#include "input_port.h"
#include <rx_input/ppm_input.h>   // PpmDecoder (RMT multi-channel PPM; 1-ch = single RC PWM)

namespace sfx_peripherals {

class EspInputPort final : public InputPort {
public:
    /// @param gpioPin GPIO carrying the input signal.
    /// @param uartNum UART peripheral (1 or 2; UART0 is the console).
    EspInputPort(int gpioPin, uint8_t uartNum)
        : _pin(gpioPin), _uartNum(uartNum) {}

    bool begin() override {
        // No peripheral claim yet — mode-specific configure*() does it.
        // We just sanity-check the args.
        return _pin >= 0 && (_uartNum == 1 || _uartNum == 2);
    }

    // ── Mode-switch helpers ──────────────────────────────────────────

    bool configurePulseCapture() override {
        // PULSE mode = PPM sum-signal capture via the RMT peripheral.
        // A plain single-channel RC PWM decodes as a 1-channel frame
        // (its ~18 ms inter-pulse gap exceeds the PPM sync threshold).
        teardownActive();
        const bool ok = _ppm.begin(_pin);
        _mode = ok ? Mode::PULSE : Mode::IDLE;
        return ok;
    }

    bool configureSbus() override {
        // SBUS: 100 000 baud, 8 data bits, EVEN parity, 2 stop bits,
        // **inverted UART idle** (high idle = logic 0).  ESP32 UART
        // does invert in hardware.
        teardownActive();
        auto& s = uartSerial();
        s.begin(100000, SERIAL_8E2, _pin, /*tx=*/-1, /*invert=*/true);
        _mode = Mode::SBUS;
        return true;
    }

    bool configureJetiEx(uint32_t baud) override {
        // LISTEN-ONLY.  Jeti EX Bus is half-duplex on one wire, but to READ
        // RC channel data we only need RX.  RX-only (tx=-1, normal UART mode)
        // is the configuration verified on the input_monitor bench rig
        // (thousands of bytes/s, clean CRC).  The earlier RS-485 half-duplex
        // + shared-pin (tx=rx) setup received almost nothing — half-duplex
        // holds the line for TX, so the inbound stream never reaches RX
        // (instrumentation showed rxBytes stuck at ~1).  Telemetry talk-back
        // (device->radio, EX telemetry / JetiBox — direction B) will need the
        // half-duplex path restored with response-slot TX timing; that's a
        // separate future feature.  Datasheet baud: 125 000 or 250 000.
        teardownActive();
        auto& s = uartSerial();
        s.begin(baud, SERIAL_8N1, _pin, /*tx=*/-1, /*invert=*/false);
        _mode = Mode::JETI_EX;
        return true;
    }

    bool configureUartRaw(uint32_t baud,
                          uint32_t serialConfig,
                          bool     invert,
                          bool     halfDuplex) override {
        teardownActive();
        auto& s = uartSerial();
        if (halfDuplex) {
            s.begin(baud, serialConfig, _pin, _pin, invert);
            uart_set_mode(static_cast<uart_port_t>(_uartNum),
                          UART_MODE_RS485_HALF_DUPLEX);
        } else {
            s.begin(baud, serialConfig, _pin, /*tx=*/-1, invert);
        }
        _mode = Mode::UART_RAW;
        return true;
    }

    void disable() override {
        teardownActive();
        _mode = Mode::IDLE;
    }

    Mode currentMode() const override { return _mode; }

    uint8_t capabilities() const override {
        // ESP32-S3 UART supports invert, half-duplex, and arbitrary
        // baud — every InputPortFlags bit is supported.
        return /*PULSE*/ 0x01 | /*SBUS*/ 0x02 | /*JETI_EX*/ 0x04 | /*UART_RAW*/ 0x08;
    }

    // ── Pulse-mode read ──────────────────────────────────────────────

    bool readPulseUs(uint16_t* outUs) override {
        if (_mode != Mode::PULSE) return false;
        _ppm.update();                       // drain RMT queue
        if (!_ppm.isValid()) return false;
        if (outUs) *outUs = _ppm.channel_us(1);   // channel 1 (1-based)
        return true;
    }

    int latestPulseUs() const override {
        return (_mode == Mode::PULSE && _ppm.isValid()) ? (int)_ppm.channel_us(1) : 0;
    }

    // Multi-channel PPM frame read (the PULSE-mode path).  Drains the RMT
    // queue, then copies up to `maxCh` decoded channel widths; returns the
    // channel count (0 when not in PULSE mode or no valid frame).
    uint8_t readPpmChannels(uint16_t* out, uint8_t maxCh) override {
        if (_mode != Mode::PULSE || !out || maxCh < 1) return 0;
        _ppm.update();                       // drain RMT queue
        if (!_ppm.isValid()) return 0;
        uint8_t n = _ppm.channelCount();
        if (n > maxCh) n = maxCh;
        for (uint8_t i = 0; i < n; i++) out[i] = _ppm.channel_us((uint8_t)(i + 1));
        return n;
    }

    // ── UART-mode stream ────────────────────────────────────────────

    Stream* uartStream() override {
        switch (_mode) {
            case Mode::SBUS:
            case Mode::JETI_EX:
            case Mode::UART_RAW: return &uartSerial();
            default:             return nullptr;
        }
    }

private:
    HardwareSerial& uartSerial() {
        return (_uartNum == 1) ? Serial1 : Serial2;
    }

    void teardownActive() {
        switch (_mode) {
            case Mode::PULSE:
                _ppm.end();
                break;
            case Mode::SBUS:
            case Mode::JETI_EX:
            case Mode::UART_RAW:
                uartSerial().end();
                break;
            default: break;
        }
    }

    int        _pin;
    uint8_t    _uartNum;          ///< 1 or 2
    PpmDecoder _ppm;              ///< RMT multi-channel PPM capture (PULSE mode)
    Mode       _mode = Mode::IDLE;
};

}  // namespace sfx_peripherals

#endif  // SFX_ESP_INPUT_PORT_H
