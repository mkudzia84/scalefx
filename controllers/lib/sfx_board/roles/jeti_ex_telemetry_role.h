/*
 * JetiExTelemetryRole — Jeti EX Bus telemetry MONITOR on an `InputPort`.
 *
 * Listen-only role for the concentrator's secondary input (e.g. HubFX IN_2 /
 * UART2): it watches the EX Bus telemetry a downstream slave (an ESC) reports
 * and merges those sensor values into the board-shared JetiTelemetryHub under
 * SRC_DOWNSTREAM.  The master Jeti channel's responder then serves the merged
 * hub back to the receiver (phase 2 — half-duplex TX).
 *
 * RX-only this phase (no master polling) — the port is put in JETI_EX mode,
 * which `EspInputPort::configureJetiEx()` brings up RX-only at 125000 8N1.
 * ESP32-only (matches the monitor's build gating).
 */

#ifndef SFX_JETI_EX_TELEMETRY_ROLE_H
#define SFX_JETI_EX_TELEMETRY_ROLE_H

#include <Arduino.h>
#include <cstdint>
#include <functional>

#include <platform/sfx_platform.h>
#include <ports/input_port.h>
#include <serial/ports.h>       // InputPortFlags::JETI_EX
#include <serial/diag_log.h>

#if SFX_PLATFORM_ESP32
#  include <jeti_ex/jeti_ex_telemetry_monitor.h>
#  include <jeti_ex/jeti_telemetry_hub.h>
#  include <jeti_ex/jeti_expander.h>      // compiled here; wired in Phase C
#endif

namespace sfx_core {

class JetiExTelemetryRole {
public:
    JetiExTelemetryRole() = default;
    explicit JetiExTelemetryRole(sfx_peripherals::InputPort* port) { bind(port); }

    /// Put the port in JETI_EX (RX-only) mode and attach the telemetry
    /// monitor.  Returns false on capability mismatch or configure failure.
    bool bind(sfx_peripherals::InputPort* port, uint32_t baud = 125000) {
        if (!port) return false;
        if ((port->capabilities() & InputPortFlags::JETI_EX) == 0)
            return false;
        if (!port->configureJetiEx(baud)) return false;
#if SFX_PLATFORM_ESP32
        Stream* s = port->uartStream();
        if (!s) { port->disable(); return false; }
        if (!_monitor.begin(s)) { port->disable(); return false; }
#else
        (void)baud;
#endif
        _port = port;
        return true;
    }

    /// Tick — drain UART, decode telemetry → hub, expire stale, broadcast.
    void tick() {
        if (!_port) return;
#if SFX_PLATFORM_ESP32
        const uint32_t now = millis();
        _monitor.update(now);
        // Expire downstream sensors not refreshed within ~2 s so a
        // disconnected ESC drops out of the merged telemetry.
        if (now - _lastExpireMs >= 1000) {
            _lastExpireMs = now;
            JetiEx::JetiTelemetryHub::instance().expireStale(now, 2000);
        }
#if SFX_INSTRUMENTATION
        // Health on the diag stream (visible via CLI `subscribe`): rxB climbs
        // = bytes on the wire; telem/sensors climb = decoding works; err high
        // with rxB climbing = wrong baud / framing.
        if (now - _lastLogMs >= 1000) {
            _lastLogMs = now;
            SFX_LOG_INFO("[jtelem] in%u rxB=%lu frames=%lu telem=%lu sensors=%u err=%lu",
                         (unsigned)_portIdx,
                         (unsigned long)_monitor.rxByteCount(),
                         (unsigned long)_monitor.frameCount(),
                         (unsigned long)_monitor.telemetryFrames(),
                         (unsigned)sensorCount(),
                         (unsigned long)_monitor.errorCount());
        }
#endif
#endif
    }

    void setPortIdx(uint8_t idx) { _portIdx = idx; }

    // ── Diagnostics ──────────────────────────────────────────────────
    uint32_t rxByteCount()     const {
#if SFX_PLATFORM_ESP32
        return _monitor.rxByteCount();
#else
        return 0;
#endif
    }
    uint32_t frameCount()      const {
#if SFX_PLATFORM_ESP32
        return _monitor.frameCount();
#else
        return 0;
#endif
    }
    uint32_t telemetryFrames() const {
#if SFX_PLATFORM_ESP32
        return _monitor.telemetryFrames();
#else
        return 0;
#endif
    }
    uint32_t errorCount()      const {
#if SFX_PLATFORM_ESP32
        return _monitor.errorCount();
#else
        return 0;
#endif
    }
    /// Number of (active) sensors currently merged in the hub.
    uint8_t  sensorCount()     const {
#if SFX_PLATFORM_ESP32
        return JetiEx::JetiTelemetryHub::instance().activeSensorCount();
#else
        return 0;
#endif
    }

private:
    sfx_peripherals::InputPort* _port = nullptr;
#if SFX_PLATFORM_ESP32
    JetiEx::JetiExTelemetryMonitor _monitor;
#endif
    uint8_t  _portIdx      = 0;
    uint32_t _lastExpireMs = 0;
    uint32_t _lastLogMs    = 0;
};

}  // namespace sfx_core

#endif  // SFX_JETI_EX_TELEMETRY_ROLE_H
