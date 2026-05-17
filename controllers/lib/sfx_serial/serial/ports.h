/*
 * Port wire protocol — enumeration + raw access to board-declared ports.
 *
 * A board declares three kinds of physical ports today (extensible):
 *   - Servo    — PWM output for hobby servos, optionally pulse-width input
 *   - Pwm      — uni-directional PWM output, optional voltage/current/temp sense
 *   - HBridge  — bi-directional PWM (two-pin / direction-pin / dedicated chip),
 *                optional sense channels
 *
 * The port layer is hardware-only.  Functional roles (LED animator, DC
 * motor, heater, RC PWM input, servo motion-profile actuator) sit on top
 * and are attached at runtime via the wire commands in `roles.h`.
 *
 * Packet ranges (within the generic-expander 0x10..0x3F slice):
 *   0x10..0x17  port enumeration
 *   0x18..0x1F  servo port raw access
 *   0x20..0x2F  PWM port raw access
 *   0x30..0x3F  H-bridge port raw access
 *
 * Wire layout for PORT_LIST_RESP (single packet, append-only Rule 11):
 *
 *   [numServo:u8]     × [idx:u8][capFlags:u8]
 *   [numPwm:u8]       × [idx:u8][senseFlags:u8]
 *   [numHBridge:u8]   × [idx:u8][senseFlags:u8]
 */

#ifndef SFX_PORTS_PROTOCOL_H
#define SFX_PORTS_PROTOCOL_H

#include <cstdint>

// ============================================================================
// Port kinds — single-byte enum, append-only (Rule 11)
// ============================================================================

namespace PortKind {
    constexpr uint8_t Unknown  = 0x00;  ///< Reserved / "no port" sentinel.
    constexpr uint8_t Servo    = 0x01;
    constexpr uint8_t Pwm      = 0x02;
    constexpr uint8_t HBridge  = 0x03;
    // 0x04..0xFE reserved for future kinds (e.g., SerialInput for SBUS/Jeti).
    constexpr uint8_t Reserved = 0xFF;

    inline const char* getName(uint8_t kind) {
        switch (kind) {
            case Servo:   return "servo";
            case Pwm:     return "pwm";
            case HBridge: return "hbridge";
            default:      return "unknown";
        }
    }
}

// ============================================================================
// Per-port capability / sense bitmasks
// ============================================================================

namespace ServoPortFlags {
    /// `EMITS` / `SAMPLES` rather than OUTPUT / INPUT because the Arduino
    /// core `#define`s both names as pinMode constants — they would
    /// clobber namespaced constexpr declarations.
    constexpr uint8_t EMITS    = 1u << 0;   ///< can emit servo-pulse output
    constexpr uint8_t SAMPLES  = 1u << 1;   ///< can sample pulse width (RC PWM capture)
}

namespace PortSenseFlags {
    constexpr uint8_t VOLTAGE     = 1u << 0;
    constexpr uint8_t CURRENT     = 1u << 1;
    constexpr uint8_t TEMPERATURE = 1u << 2;
}

// ============================================================================
// Wire packet types
// ============================================================================

namespace PortPacket {
    // ── Enumeration (0x10..0x17) ──────────────────────────────────────
    constexpr uint8_t PORT_LIST_REQ      = 0x10;  ///< [] → PORT_LIST_RESP
    constexpr uint8_t PORT_LIST_RESP     = 0x11;  ///< see header layout above

    // ── Servo port raw commands (0x18..0x1F) ──────────────────────────
    constexpr uint8_t SERVO_PORT_SET_US  = 0x18;  ///< [idx:u8][us:u16LE] → ACK / NACK
    constexpr uint8_t SERVO_PORT_READ_US = 0x19;  ///< [idx:u8] → SERVO_PORT_READ_RESP
    constexpr uint8_t SERVO_PORT_READ_RESP = 0x1A;///< [idx:u8][us:u16LE][valid:u8]

    // ── PWM port raw commands (0x20..0x2F) ────────────────────────────
    constexpr uint8_t PWM_PORT_SET_DUTY    = 0x20;  ///< [idx:u8][duty:u16LE] → ACK
    constexpr uint8_t PWM_PORT_SET_FREQ    = 0x21;  ///< [idx:u8][hz:u16LE] → ACK / NACK
    constexpr uint8_t PWM_PORT_READ_SENSE  = 0x22;  ///< [idx:u8] → PWM_PORT_SENSE_RESP
    constexpr uint8_t PWM_PORT_SENSE_RESP  = 0x23;  ///< [idx:u8][v_mV:i16LE][i_mA:i16LE][t_cx10:i16LE]

    // ── H-bridge port raw commands (0x30..0x3F) ───────────────────────
    constexpr uint8_t HBRIDGE_SET_SIGNED   = 0x30;  ///< [idx:u8][signed_duty:i16LE] → ACK
    constexpr uint8_t HBRIDGE_BRAKE        = 0x31;  ///< [idx:u8] → ACK  (short brake both pins)
    constexpr uint8_t HBRIDGE_COAST        = 0x32;  ///< [idx:u8] → ACK  (open both pins)
    constexpr uint8_t HBRIDGE_READ_SENSE   = 0x33;  ///< [idx:u8] → HBRIDGE_SENSE_RESP
    constexpr uint8_t HBRIDGE_SENSE_RESP   = 0x34;  ///< [idx:u8][v_mV:i16LE][i_mA:i16LE][t_cx10:i16LE]
}

// ============================================================================
// Port-layer error codes (range 0x20..0x2F inside the generic SerialError space)
// ============================================================================

namespace PortError {
    constexpr uint8_t PORT_NOT_FOUND        = 0x20;  ///< (kind, idx) out of range
    constexpr uint8_t PORT_KIND_MISMATCH    = 0x21;  ///< command issued against wrong port kind
    constexpr uint8_t PORT_HAS_ROLE         = 0x22;  ///< raw access refused while a role owns the port
    constexpr uint8_t SENSE_NOT_AVAILABLE   = 0x23;  ///< port has no sense channel of that kind
    constexpr uint8_t PORT_FREQ_UNSUPPORTED = 0x24;  ///< driver doesn't support runtime freq change

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case PORT_NOT_FOUND:        return "Port not found";
            case PORT_KIND_MISMATCH:    return "Port kind mismatch";
            case PORT_HAS_ROLE:         return "Port has a role attached";
            case SENSE_NOT_AVAILABLE:   return "Sense channel not available";
            case PORT_FREQ_UNSUPPORTED: return "Port frequency change not supported";
            default:                    return nullptr;
        }
    }
}

#endif  // SFX_PORTS_PROTOCOL_H
