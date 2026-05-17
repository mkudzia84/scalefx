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
 *   [numServo:u8]     × [idx:u8][capFlags:u8]      ← ServoPortFlags (legacy; servos are always EMITS now)
 *   [numPwm:u8]       × [idx:u8][senseFlags:u8]
 *   [numHBridge:u8]   × [idx:u8][senseFlags:u8]
 *   [numInput:u8]     × [idx:u8][capFlags:u8]      ← InputPortFlags (PULSE / SBUS / JETI_EX / UART_RAW)
 */

#ifndef SFX_PORTS_PROTOCOL_H
#define SFX_PORTS_PROTOCOL_H

#include <cstdint>

// ============================================================================
// Port kinds — single-byte enum, append-only (Rule 11)
// ============================================================================

namespace PortKind {
    constexpr uint8_t Unknown  = 0x00;  ///< Reserved / "no port" sentinel.
    constexpr uint8_t Servo    = 0x01;  ///< pulse output (always output).
    constexpr uint8_t Pwm      = 0x02;  ///< PWM output.
    constexpr uint8_t HBridge  = 0x03;  ///< bidirectional PWM output.
    constexpr uint8_t Input    = 0x04;  ///< multi-modal input (pulse capture OR UART RX
                                        ///  for SBUS / Jeti EX / future serial protocols).
                                        ///  Direction is fixed at declaration; mode is
                                        ///  selected by the attached role.
    // 0x05..0xFE reserved.
    constexpr uint8_t Reserved = 0xFF;

    inline const char* getName(uint8_t kind) {
        switch (kind) {
            case Servo:   return "servo";
            case Pwm:     return "pwm";
            case HBridge: return "hbridge";
            case Input:   return "input";
            default:      return "unknown";
        }
    }
}

// ============================================================================
// Per-port capability / sense bitmasks
// ============================================================================

/// Current operating mode of an `InputPort`, surfaced via INPUT_MODE_RESP
/// for diagnostics.  The mode is selected by the attached role
/// (RcPwmInputRole → PULSE, SbusInputRole → SBUS, JetiExInputRole → JETI_EX).
namespace InputMode {
    constexpr uint8_t IDLE     = 0x00;  ///< no role attached
    constexpr uint8_t PULSE    = 0x01;
    constexpr uint8_t SBUS     = 0x02;
    constexpr uint8_t JETI_EX  = 0x03;
    constexpr uint8_t UART_RAW = 0x04;
}

namespace ServoPortFlags {
    /// Servo ports are output-only post the InputPort split (Rule 31).
    /// The capability byte exists for wire-format symmetry — bit 0 is
    /// always set, signalling "outputs servo pulse".
    constexpr uint8_t EMITS = 1u << 0;
}

namespace InputPortFlags {
    /// Capability bits an InputPort advertises in PORT_LIST_RESP so the
    /// hub can choose a compatible role for it.
    constexpr uint8_t PULSE    = 1u << 0;   ///< edge-IRQ pulse capture (RC PWM / PPM)
    constexpr uint8_t SBUS     = 1u << 1;   ///< UART 100000 8E2 inverted (Futaba SBUS)
    constexpr uint8_t JETI_EX  = 1u << 2;   ///< UART 125/250 kbaud 8N1 half-duplex
    constexpr uint8_t UART_RAW = 1u << 3;   ///< user-specified UART parameters (CRSF / future)
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

    // ── H-bridge port raw commands (0x30..0x37) ───────────────────────
    constexpr uint8_t HBRIDGE_SET_SIGNED   = 0x30;  ///< [idx:u8][signed_duty:i16LE] → ACK
    constexpr uint8_t HBRIDGE_BRAKE        = 0x31;  ///< [idx:u8] → ACK  (short brake both pins)
    constexpr uint8_t HBRIDGE_COAST        = 0x32;  ///< [idx:u8] → ACK  (open both pins)
    constexpr uint8_t HBRIDGE_READ_SENSE   = 0x33;  ///< [idx:u8] → HBRIDGE_SENSE_RESP
    constexpr uint8_t HBRIDGE_SENSE_RESP   = 0x34;  ///< [idx:u8][v_mV:i16LE][i_mA:i16LE][t_cx10:i16LE]

    // ── Input port raw commands (0x38..0x3F) ──────────────────────────
    constexpr uint8_t INPUT_READ_PULSE     = 0x38;  ///< [idx:u8] → INPUT_PULSE_RESP (pulse-capture mode only)
    constexpr uint8_t INPUT_PULSE_RESP     = 0x39;  ///< [idx:u8][us:u16LE][valid:u8]
    constexpr uint8_t INPUT_GET_MODE       = 0x3A;  ///< [idx:u8] → INPUT_MODE_RESP
    constexpr uint8_t INPUT_MODE_RESP      = 0x3B;  ///< [idx:u8][mode:u8]  (see InputMode below)
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
