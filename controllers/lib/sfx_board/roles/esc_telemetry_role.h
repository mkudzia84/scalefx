/*
 * esc_telemetry_role.h — native ESC telemetry role (RoleKind 0x05).
 *
 *   Binds an input port RX-ONLY to a manufacturer ESC telemetry broadcast and
 *   runs the matching decoder policy over it (telemetry/esc/):
 *
 *     0 kontronik      115200 8E1  — native KODL/KODI stream (spec V5)
 *     1 scorpion       38400  8N1  — Tribunus "Unsc Telem" records
 *     2 hobbywing-v4   19200  8N1  — Platinum PRO V4 / FlyFun V5 stream
 *     3 hobbywing-v5   115200 8N1  — Platinum V5 stream
 *
 *   Strictly PASSIVE: the ESC broadcasts push-pull on its own; we configure
 *   the UART RX-only and parse (EscTelemetryMonitor), publishing a normalized
 *   sensor set + fault messages into the TelemetryHub → the Jeti radio +
 *   Studio Telemetry panel, device-identified where the protocol provides it
 *   (Kontronik KODI).
 *
 *   (The legacy jeti-exbus "downstream marker" mode — the port loaned to the
 *   JetiExpander as an EX-Bus master link — was REMOVED 2026-07-15.)
 *
 *   Attach config (Rule 11 append-only):
 *     [protocol:u8][baudKHi:u8][baudKLo:u8][ratioHi:u8][ratioLo:u8]
 *     baud in kbaud (0 = protocol default); ratio = RPM divider ×100
 *     (pole pairs × gearbox, 0 = 1.00); zero-length = kontronik defaults.
 */

#ifndef SFX_ESC_TELEMETRY_ROLE_H
#define SFX_ESC_TELEMETRY_ROLE_H

#include <cstdint>
#include <cstdio>   // snprintf (instrumentation hex)
#include <platform/sfx_platform.h>       // SFX_MILLIS
#include <ports/input_port.h>
#include <telemetry/esc_telemetry_monitor.h>
#include <serial/ports.h>                // InputPortFlags
#include <serial/diag_log.h>             // [esctelem] bench instrumentation

namespace sfx_core {

class EscTelemetryRole {
public:
    EscTelemetryRole() = default;

    /// @param protocol     EscProtocol value (0..3, telemetry/esc/).
    /// @param baud         0 = protocol default (from the decoder pack).
    /// @param rpmRatioX100 RPM divider ×100 (pole pairs × gear ratio) applied
    ///                     at publish; 0 = 1.00 (as transmitted).
    bool bind(sfx_peripherals::InputPort* port, uint8_t protocol, uint32_t baud,
              uint16_t rpmRatioX100 = 0) {
        if (!port) return false;
        if ((port->capabilities() & InputPortFlags::UART_RAW) == 0) return false;
        _port = port;
        _protocol = protocol;
        sfx_peripherals::EscUartParams p{};
        if (!sfx_peripherals::EscTelemetryMonitor::uartParamsFor(protocol, p))
            return false;                          // not in the decoder pack
        const uint32_t useBaud = baud ? baud : p.baud;
        if (!port->configureUartRaw(useBaud,
                                    p.parityEven ? sfx_peripherals::kSerial8E1
                                                 : sfx_peripherals::kSerial8N1,
                                    /*invert=*/false, /*halfDuplex=*/false))
            return false;
        auto* s = port->uartStream();
        if (!s) return false;
        if (!_mon.begin(s, protocol)) return false;
        _mon.setRpmDividerX100(rpmRatioX100);
        _active = true;
        return true;
    }

    /// Main-loop drain — the native parsers are pure RX (≤ ~4 KB/s), so a
    /// per-pass bounded drain on the loop task is plenty (4 KB UART ring
    /// rides out multi-hundred-ms loop stalls).
    void tick() {
        if (!_active) return;
        const uint32_t now = SFX_MILLIS();
        _mon.update(now);
#if SFX_INSTRUMENTATION
        if (now - _lastLogMs >= 2000) {
            _lastLogMs = now;
            uint8_t snap[48];
            const uint8_t n = _mon.takeSnap(snap, sizeof snap);
            char hex[33] = {};
            const uint8_t h1 = (n < 16) ? n : 16;
            for (uint8_t i = 0; i < h1; ++i)
                std::snprintf(&hex[i * 2], 3, "%02X", snap[i]);
            SFX_LOG_INFO("[esctelem] in[%u] proto=%u rxB=%lu frames=%lu errs=%lu dev=%s rx[..16]=%s",
                         (unsigned)_portIdx, (unsigned)_protocol,
                         (unsigned long)_mon.rxBytes(), (unsigned long)_mon.frames(),
                         (unsigned long)_mon.errors(), _mon.data().deviceName,
                         h1 ? hex : "-");
            if (n > 16) {
                // Second line: the rest of the 48-byte raw snapshot — enough
                // to see the NEXT frame magic and measure the true stride.
                char hex2[65] = {};
                for (uint8_t i = 16; i < n; ++i)
                    std::snprintf(&hex2[(i - 16) * 2], 3, "%02X", snap[i]);
                SFX_LOG_INFO("[esctelem] in[%u] rx[16..48]=%s", (unsigned)_portIdx, hex2);
            }
            uint8_t bad[48];
            const size_t bn = _mon.takeBadFrame(bad, sizeof bad);
            if (bn) {
                // A CRC-rejected info frame — dump it whole (two lines) so an
                // unknown device variant's length/region can be pinned exactly.
                char hexb[49] = {};
                const size_t h = (bn < 24) ? bn : 24;
                for (size_t i = 0; i < h; ++i)
                    std::snprintf(&hexb[i * 2], 3, "%02X", bad[i]);
                SFX_LOG_INFO("[esctelem] in[%u] badframe[0..%u]=%s",
                             (unsigned)_portIdx, (unsigned)h, hexb);
                if (bn > 24) {
                    char hexc[49] = {};
                    for (size_t i = 24; i < bn; ++i)
                        std::snprintf(&hexc[(i - 24) * 2], 3, "%02X", bad[i]);
                    SFX_LOG_INFO("[esctelem] in[%u] badframe[24..%u]=%s",
                                 (unsigned)_portIdx, (unsigned)bn, hexc);
                }
            }
        }
#endif
    }

    void setPortIdx(uint8_t idx) { _portIdx = idx; }
    uint8_t portIdx()  const { return _portIdx; }
    uint8_t protocol() const { return _protocol; }
    bool    nativeActive() const { return _active; }

    // Diagnostics (CLI/Studio surfaces).
    uint32_t frames() const { return _mon.frames(); }
    uint32_t errors() const { return _mon.errors(); }
    const sfx_peripherals::EscTelemData& data() const { return _mon.data(); }

private:
    sfx_peripherals::InputPort* _port = nullptr;
    uint8_t _portIdx  = 0;
    uint8_t _protocol = 0;
    bool    _active   = false;
    uint32_t _lastLogMs = 0;
    sfx_peripherals::EscTelemetryMonitor _mon;
};

}  // namespace sfx_core

#endif  // SFX_ESC_TELEMETRY_ROLE_H
