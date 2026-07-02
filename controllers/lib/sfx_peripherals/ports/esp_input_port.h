/*
 * esp_input_port.h — ESP32-S3 implementation of `InputPort`.
 *
 * Wraps one RX GPIO + one ESP32 UART peripheral, plus an OPTIONAL
 * dedicated TX GPIO for boards that split the half-duplex line:
 *
 *   - Single-wire boards (HubFX rev A): one header pin carries RX and
 *     TX.  Construct with (rxPin, uartNum); the TX slot is driven onto
 *     the SAME pin via the GPIO matrix (txEnable/Disable).
 *   - Split TX/RX boards (HubFX rev B INP/TELEM headers): the header
 *     line sits DIRECTLY on the TX GPIO and reaches the RX GPIO through
 *     a series resistor (2.2 kΩ bridge), so RX listens continuously
 *     while TX stays high-Z except during its reply slot.  Construct
 *     with (rxPin, txPin, uartNum); txEnable()/txDisable() then attach/
 *     detach the UART TX signal on the dedicated pin instead.
 *
 * Modes:
 *   - PULSE      : native RMT pulse capture via the RMT PpmDecoder (`_ppm`)
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

#include <driver/uart.h>          // native UART config + UART_MODE_RS485_HALF_DUPLEX
#include <driver/gpio.h>          // gpio_set_direction (native half-duplex OE toggle)
#include <esp_rom_gpio.h>         // esp_rom_gpio_connect_out_signal (matrix TX route)
#include <soc/gpio_sig_map.h>     // U1TXD_OUT_IDX / U2TXD_OUT_IDX / SIG_GPIO_OUT_IDX
#include <cstdint>

#include "input_port.h"
#include <platform/native_uart_stream.h>  // sfx::NativeUartStream — native RC UART
#include <rx_input/ppm_input.h>   // PpmDecoder (RMT multi-channel PPM; 1-ch = single RC PWM)

namespace sfx_peripherals {

class EspInputPort final : public InputPort {
public:
    /// Single-wire port: one pad carries RX and the slot-gated TX.
    /// @param rxPin   GPIO carrying the input signal.
    /// @param uartNum UART peripheral (1 or 2; UART0 is the console).
    EspInputPort(int rxPin, uint8_t uartNum)
        : _rxPin(rxPin), _txPin(-1), _uartNum(uartNum) {}

    /// Split TX/RX port: the header line sits on @p txPin and reaches
    /// @p rxPin through a series bridge resistor.
    /// @param rxPin   GPIO carrying the input signal (listens continuously).
    /// @param txPin   Dedicated TX GPIO directly on the header line —
    ///                idles high-Z, drives only its reply slot.
    /// @param uartNum UART peripheral (1 or 2; UART0 is the console).
    EspInputPort(int rxPin, int txPin, uint8_t uartNum)
        : _rxPin(rxPin), _txPin(txPin), _uartNum(uartNum) {}

    bool begin() override {
        // No peripheral claim yet — mode-specific configure*() does it.
        // We just sanity-check the args.  A dedicated TX pin sits DIRECTLY
        // on the shared header line — park it high-Z NOW so it can't fight
        // the external device before the first txEnable().
        if (_txPin >= 0) {
            gpio_set_direction((gpio_num_t)_txPin, GPIO_MODE_INPUT);
        }
        return _rxPin >= 0 && (_uartNum == 1 || _uartNum == 2);
    }

    // ── Mode-switch helpers ──────────────────────────────────────────

    bool configurePulseCapture() override {
        // PULSE mode = PPM sum-signal capture via the RMT peripheral.
        // A plain single-channel RC PWM decodes as a 1-channel frame
        // (its ~18 ms inter-pulse gap exceeds the PPM sync threshold).
        teardownActive();
        const bool ok = _ppm.begin(_rxPin);
        _mode = ok ? Mode::PULSE : Mode::IDLE;
        return ok;
    }

    bool configureSbus() override {
        // SBUS: 100 000 baud, 8 data bits, EVEN parity, 2 stop bits,
        // **inverted UART idle** (high idle = logic 0).  ESP32 UART
        // does the invert in hardware (UART_SIGNAL_RXD_INV).
        teardownActive();
        uart_config_t cfg = {};
        cfg.baud_rate  = 100000;
        cfg.data_bits  = UART_DATA_8_BITS;
        cfg.parity     = UART_PARITY_EVEN;
        cfg.stop_bits  = UART_STOP_BITS_2;
        cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;
        _uart.beginConfig(uartPort(), _rxPin, /*tx=*/-1, cfg,
                          UART_SIGNAL_RXD_INV, /*rs485=*/false, /*rxBuf=*/1024);
        _mode = Mode::SBUS;
        return true;
    }

    bool configureJetiEx(uint32_t baud) override {
        // Half-duplex on ONE wire, RX-clean.  Begin RX-ONLY (tx=-1): the RX
        // signal is routed from the pad via the GPIO matrix and the pin idles
        // as a high-Z input — the config that read channels rock-solid on the
        // full board (git 8a7fc47; bench-proven on input_monitor / jeti_ex_telem).
        //
        // The telemetry reply drives TX only for its ~4 ms slot via txEnable()/
        // txDisable(), which flip ONLY the output-enable with native
        // gpio_set_direction (NOT Arduino pinMode, which re-runs gpio_config and
        // tears down the matrix RX input — that killed RX after one reply, and
        // begin(rx=tx) leaves the pad configured for TX at rest which degraded RX).
        //
        // NOT `UART_MODE_RS485_HALF_DUPLEX`: that mode holds the line for TX and
        // the inbound stream never reaches RX (rxBytes stuck at ~1).
        // Datasheet baud: 125 000 or 250 000.
        teardownActive();
        uart_config_t cfg = {};
        cfg.baud_rate  = (int)baud;
        cfg.data_bits  = UART_DATA_8_BITS;
        cfg.parity     = UART_PARITY_DISABLE;
        cfg.stop_bits  = UART_STOP_BITS_1;
        cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;
        // tx=-1, NO rs485 mode — half-duplex TX is driven manually via the
        // GPIO matrix in txEnable()/txDisable() (see the comment above).
        _uart.beginConfig(uartPort(), _rxPin, /*tx=*/-1, cfg,
                          /*invert=*/0, /*rs485=*/false, /*rxBuf=*/1024);
        _mode = Mode::JETI_EX;
        return true;
    }

    bool configureUartRaw(uint32_t baud,
                          uint32_t /*serialConfig*/,
                          bool     invert,
                          bool     halfDuplex) override {
        // Raw passthrough for future CRSF / DSM roles (none wired yet).
        // Framing defaults to 8N1; per-protocol parity/stop mapping lands
        // when a raw-UART role actually ships.
        teardownActive();
        uart_config_t cfg = {};
        cfg.baud_rate  = (int)baud;
        cfg.data_bits  = UART_DATA_8_BITS;
        cfg.parity     = UART_PARITY_DISABLE;
        cfg.stop_bits  = UART_STOP_BITS_1;
        cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;
        // TX routing: full-duplex uses the dedicated TX pin when present
        // (-1 = RX-only, as before).  Half-duplex on a SPLIT-pin board must
        // NOT attach TX permanently — the TX pad sits directly on the shared
        // line and UART TX idles high, which would fight the external
        // driver; begin RX-only and gate the reply slot with txEnable()/
        // txDisable() exactly like JETI_EX.  Single-wire boards keep the
        // legacy rs485-flag path on _rxPin.
        const bool splitPins = (_txPin >= 0);
        const int  txPin     = splitPins ? (halfDuplex ? -1 : _txPin)
                                         : (halfDuplex ? _rxPin : -1);
        _uart.beginConfig(uartPort(), _rxPin, txPin, cfg,
                          invert ? UART_SIGNAL_RXD_INV : 0,
                          halfDuplex && !splitPins, /*rxBuf=*/1024);
        _rawHalfDuplex = halfDuplex && splitPins;
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

    sfx::Stream* uartStream() override {
        switch (_mode) {
            case Mode::SBUS:
            case Mode::JETI_EX:
            case Mode::UART_RAW: return &_uart;   // native UART, IS an sfx::Stream
            default:             return nullptr;
        }
    }

    // ── Half-duplex TX control (JETI_EX / raw half-duplex) ───────────
    // Per-slot route: raise OE + route the UART TX onto the drive pad for the
    // reply, then route the pad-out back to the GPIO simple-output and drop
    // OE → high-Z.  Single-wire boards drive _rxPin itself (INPUT_OUTPUT so the
    // matrix RX input set by begin stays alive — Arduino pinMode would tear it
    // down → no signal); split TX/RX boards drive the dedicated _txPin (plain
    // OUTPUT — RX lives on its own pin and is never disturbed).
    void txEnable() override {
        if (!txSlotAllowed()) return;
        const int tp = txDrivePin();
        gpio_set_direction((gpio_num_t)tp,
                           tp == _rxPin ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(tp, txSignalIdx(), /*invert=*/false, /*oen_invert=*/false);
    }
    void txDisable() override {
        if (!txSlotAllowed()) return;
        const int tp = txDrivePin();
        esp_rom_gpio_connect_out_signal(tp, SIG_GPIO_OUT_IDX, /*invert=*/false, /*oen_invert=*/false);
        gpio_set_direction((gpio_num_t)tp, GPIO_MODE_INPUT);   // high-Z; matrix RX intact
    }

private:
    uart_port_t uartPort() const {
        return (_uartNum == 1) ? UART_NUM_1 : UART_NUM_2;
    }

    /// Pad the TX slot drives: the dedicated TX pin when present, else the
    /// shared single-wire pin.
    int txDrivePin() const { return (_txPin >= 0) ? _txPin : _rxPin; }

    /// Modes whose reply slot is gated manually via txEnable()/txDisable().
    bool txSlotAllowed() const {
        return _mode == Mode::JETI_EX ||
               (_mode == Mode::UART_RAW && _rawHalfDuplex);
    }

    /// UART TX peripheral-output signal index for the GPIO matrix.
    uint32_t txSignalIdx() const {
        return (_uartNum == 1) ? U1TXD_OUT_IDX : U2TXD_OUT_IDX;
    }

    void teardownActive() {
        switch (_mode) {
            case Mode::PULSE:
                _ppm.end();
                break;
            case Mode::SBUS:
            case Mode::JETI_EX:
            case Mode::UART_RAW:
                _uart.end();
                break;
            default: break;
        }
    }

    int               _rxPin;
    int               _txPin;     ///< dedicated TX pad (-1 = single-wire on _rxPin)
    uint8_t           _uartNum;   ///< 1 or 2
    bool              _rawHalfDuplex = false;  ///< UART_RAW slot-gated TX (split pins)
    PpmDecoder        _ppm;       ///< RMT multi-channel PPM capture (PULSE mode)
    Mode              _mode = Mode::IDLE;
    sfx::NativeUartStream _uart;  ///< native UART (SBUS/Jeti/raw modes)
};

}  // namespace sfx_peripherals

#endif  // SFX_ESP_INPUT_PORT_H
