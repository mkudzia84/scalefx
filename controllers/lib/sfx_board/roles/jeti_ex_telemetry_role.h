/*
 * JetiExTelemetryRole — downstream (ESC) Jeti EX Bus link (IN_2) marker.
 *
 * Thin marker.  The downstream link is owned and driven by the board-unique
 * JetiExpander (main-loop, driven by the IN_1 Jeti EX role's tick), which the
 * IN_1 attach handler starts with BOTH links — it configures IN_2, monitors the
 * downstream device,
 * decodes its telemetry PASS-THROUGH into the shared JetiTelemetryHub, and the
 * Rx-facing responder serves it.  This role just validates the port and
 * delegates diagnostics to the expander.  ESP32-only at runtime.
 */

#ifndef SFX_JETI_EX_TELEMETRY_ROLE_H
#define SFX_JETI_EX_TELEMETRY_ROLE_H

#include <Arduino.h>
#include <cstdint>

#include <platform/sfx_platform.h>
#include <ports/input_port.h>
#include <serial/ports.h>       // InputPortFlags::JETI_EX

#if SFX_PLATFORM_ESP32
#  include <jeti_ex/jeti_expander.h>
#endif

namespace sfx_core {

class JetiExTelemetryRole {
public:
    JetiExTelemetryRole() = default;
    explicit JetiExTelemetryRole(sfx_peripherals::InputPort* port) { bind(port); }

    /// Validate the JETI_EX capability and record the port.  The expander
    /// (started on the IN_1 link) configures and drives IN_2 — NOT here, so the
    /// two never fight over the UART.
    bool bind(sfx_peripherals::InputPort* port, uint32_t baud = 125000) {
        (void)baud;
        if (!port) return false;
        if ((port->capabilities() & InputPortFlags::JETI_EX) == 0) return false;
        _port = port;
        return true;
    }

    /// No I/O — the JetiExpander (driven by the IN_1 role tick) owns both links.
    void tick() {}

    void setPortIdx(uint8_t idx) { _portIdx = idx; }

    // ── Diagnostics (delegate to the expander's downstream monitor) ──
    uint32_t rxByteCount()     const {
#if SFX_PLATFORM_ESP32
        return JetiEx::JetiExpander::instance().escBytes();
#else
        return 0;
#endif
    }
    uint32_t telemetryFrames() const {
#if SFX_PLATFORM_ESP32
        return JetiEx::JetiExpander::instance().escFrames();
#else
        return 0;
#endif
    }
    uint32_t errorCount()      const {
#if SFX_PLATFORM_ESP32
        return JetiEx::JetiExpander::instance().escErrors();
#else
        return 0;
#endif
    }
    uint8_t  sensorCount()     const {
#if SFX_PLATFORM_ESP32
        return JetiEx::JetiExpander::instance().sensorCount();
#else
        return 0;
#endif
    }

private:
    sfx_peripherals::InputPort* _port = nullptr;
    uint8_t  _portIdx = 0;
};

}  // namespace sfx_core

#endif  // SFX_JETI_EX_TELEMETRY_ROLE_H
