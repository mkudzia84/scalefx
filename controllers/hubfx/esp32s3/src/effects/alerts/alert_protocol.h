/*
 * alert_protocol.h — HubFX wire surface for system alerts.
 *
 *   The alert layer owns channel 0 of the mixer (`HubFxLayout::Alert`)
 *   and plays short cue sounds for system-level events: battery
 *   warnings, connection lost / restored, error chimes, success
 *   beeps, future voice messages.  Effects + services call into the
 *   master-internal C++ API; Studio / CLI can fire alerts for testing
 *   via the minimal wire surface below.
 *
 *   Severity table (extensible — append-only per Rule 11):
 *     0 Info     — soft chirp, success ack
 *     1 Warning  — mid beep, "battery getting low"
 *     2 Error    — double beep, "command rejected"
 *     3 Critical — urgent siren, "battery cutoff fired"
 *     0x10+      — reserved for future voice-message / preset packs
 *
 *   Packet slice: 0xD3..0xD6.
 */

#ifndef HUBFX_ALERT_PROTOCOL_H
#define HUBFX_ALERT_PROTOCOL_H

#include <cstdint>

namespace hubfx::effects::alerts {

namespace AlertPacket {
    /// `[severity:u8][outputMask:u8?]` → ACK / NACK.  Plays the
    /// preset sound for the given severity.  `outputMask` is
    /// optional; absent means "use the effect's configured default".
    constexpr uint8_t ALERT_BEEP        = 0xD3;
    /// `[]` → ACK.  Stops whatever is playing on the alert channel.
    constexpr uint8_t ALERT_STOP        = 0xD4;
    /// `[]` → ALERT_STATUS_RESP
    constexpr uint8_t ALERT_STATUS_REQ  = 0xD5;
    /// `[playing:u8][lastSeverity:u8]`
    constexpr uint8_t ALERT_STATUS_RESP = 0xD6;
}

namespace AlertSeverity {
    constexpr uint8_t Info     = 0;
    constexpr uint8_t Warning  = 1;
    constexpr uint8_t Error    = 2;
    constexpr uint8_t Critical = 3;
    // 0x10..0xFF reserved for future categories (voice msg, etc.).

    inline const char* getName(uint8_t s) {
        switch (s) {
            case Info:     return "info";
            case Warning:  return "warning";
            case Error:    return "error";
            case Critical: return "critical";
            default:       return "unknown";
        }
    }
}

namespace AlertError {
    constexpr uint8_t ALERT_DISABLED      = 0xD1;
    constexpr uint8_t UNKNOWN_SEVERITY    = 0xD2;
    constexpr uint8_t SOUND_NOT_CONFIGURED = 0xD3;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case ALERT_DISABLED:       return "Alert effect disabled";
            case UNKNOWN_SEVERITY:     return "Unknown alert severity";
            case SOUND_NOT_CONFIGURED: return "Alert sound path not configured";
            default:                   return nullptr;
        }
    }
}

}  // namespace hubfx::effects::alerts

#endif  // HUBFX_ALERT_PROTOCOL_H
