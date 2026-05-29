/*
 * input_port.h — abstract multi-modal input port.
 *
 * One physical signal pin that can be configured at runtime as either:
 *   - edge-IRQ pulse capture (RC PWM single channel / PPM frame)
 *   - UART RX for SBUS (100 000 8E2 inverted)
 *   - UART for Jeti EX Bus (125 000 / 250 000 8N1, half-duplex)
 *   - generic raw UART (future: CRSF, DSM, ...)
 *
 * The port-direction is **fixed input** (cf. Rule 31 — `ServoPort` /
 * `PwmPort` / `HBridgePort` are output-only; this is the only input
 * kind).  The board declares the port; a role attached at runtime
 * picks the mode via one of the `configure*()` calls.
 *
 * To prevent UART starvation, the board's declared input count MUST be
 * ≤ the number of free UART peripherals on the platform.  See Rule 31
 * in `.github/copilot-instructions.md` / CLAUDE.md.
 */

#ifndef SFX_INPUT_PORT_H
#define SFX_INPUT_PORT_H

#include <Arduino.h>
#include <cstdint>

namespace sfx_peripherals {

class InputPort {
public:
    /// Runtime mode — selected by the attached role.
    enum class Mode : uint8_t {
        IDLE       = 0,    ///< unconfigured / no role
        PULSE      = 1,    ///< edge-IRQ pulse capture
        SBUS       = 2,    ///< UART RX, 100000 8E2 inverted
        JETI_EX    = 3,    ///< UART, 8N1 half-duplex, 125k or 250k
        UART_RAW   = 4,    ///< custom UART params (CRSF, DSM, ...)
    };

    virtual ~InputPort() = default;

    /// Called once by the port registry — drivers do minimal one-shot
    /// init only (no peripheral claim yet).  Mode-specific peripheral
    /// claim happens in `configure*()`.
    virtual bool begin() = 0;

    // ── Mode selection (called by role on attach) ────────────────────
    //
    // Each transition implicitly tears down the previously-active
    // peripheral (pulse IRQ → released, UART → ended) before claiming
    // the new one.  Returns false if the platform doesn't support that
    // mode for this port (capability flag missing).
    //
    virtual bool configurePulseCapture() = 0;
    virtual bool configureSbus()         = 0;   ///< 100000 8E2 inverted
    virtual bool configureJetiEx(uint32_t baud = 125000) = 0;  ///< 125k or 250k 8N1 half-duplex
    virtual bool configureUartRaw(uint32_t baud,
                                  uint32_t serialConfig,
                                  bool invert,
                                  bool halfDuplex) = 0;
    virtual void disable() = 0;                 ///< release peripheral, → IDLE

    virtual Mode    currentMode()  const = 0;

    // ── Capability bitmask (InputPortFlags::*) for PORT_LIST_RESP ───
    virtual uint8_t capabilities() const = 0;

    // ── Pulse-mode read ──────────────────────────────────────────────
    /// Latest valid pulse width.  Returns false if not in PULSE mode
    /// or no sample yet.
    virtual bool readPulseUs(uint16_t* outUs) = 0;

    /// Last-measured pulse width without validity check (0 when not
    /// captured yet).  Useful for diagnostics.
    virtual int latestPulseUs() const = 0;

    /// Multi-channel PPM read.  Drains the capture engine and writes up
    /// to `maxCh` channel pulse-widths (µs) into `out`, returning the
    /// channel count (0 = no valid frame / signal lost).  A PPM sum
    /// signal yields N channels; a plain single-channel RC PWM yields 1.
    /// Default: fall back to the single-channel `readPulseUs` so any
    /// input driver that hasn't implemented framed capture still works.
    virtual uint8_t readPpmChannels(uint16_t* out, uint8_t maxCh) {
        uint16_t us = 0;
        if (!out || maxCh < 1 || !readPulseUs(&us)) return 0;
        out[0] = us;
        return 1;
    }

    // ── UART-mode access ─────────────────────────────────────────────
    /// Returns the underlying `Stream*` once a UART mode is active.
    /// Returns nullptr in any other mode.  Roles use this to feed
    /// their decoder (SbusInput, JetiExBus, future CRSF, …).
    virtual Stream* uartStream() = 0;
};

}  // namespace sfx_peripherals

#endif  // SFX_INPUT_PORT_H
