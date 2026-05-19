/*
 * Serial Core — Protocol, Errors, and Result Types.
 *
 * Central header for ScaleFX serial communication. Contains:
 *   - Error codes (SerialError namespace) and CommandResult struct
 *   - Protocol encoding/decoding (CoreProtocol namespace)
 *   - Core packet types + IDENTIFY/STATUS data structures
 *   - Server handler macros (SFX_*)
 *
 * Routing/dispatch live in `system_service.h` (BoardServer<...Policies>)
 * and `packet_reader.h` (PacketReader<TDispatch>).  This header is pure
 * data + macros and is included by both client and server sides.
 *
 * Protocol:
 *   Packet Format: [type:u8][tag:u8][len:u16LE][payload:0-512 bytes][crc8:u8]
 *   Framing: COBS encoded, followed by 0x00 frame delimiter
 *   CRC: CRC-8 polynomial 0x07 over type+tag+len(2 bytes)+payload
 *   Tag: 0x00 = async/unsolicited, 0x01-0xFF = request correlation ID
 *
 * Packet Type Ranges (post legacy-slave-protocol archival 2026-05-17):
 *   0x01-0x0F  Generic-expander identity / enumeration — see components/components.h
 *   0x10-0x2F  Generic-expander servo control
 *   0x30-0x4F  Generic-expander PWM control
 *   0x50-0x7F  Generic-expander LED control (event-sequence runtime)
 *   0x80-0xAF  HubFX master commands — see hubfx/hubfx.h
 *   0xA4-0xA6  Streaming protocol — see core/stream.h
 *   0xB0-0xED  Available
 *   0xEE-0xFF  Universal system commands (INIT, ACK, NACK, ...) —
 *              handled by BoardServicePolicy in board_service.h
 */

#ifndef SERIAL_CORE_H
#define SERIAL_CORE_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <cstring>

// ============================================================================
// Generic Error Codes
// ============================================================================

/**
 * @brief Generic serial error codes for ACK/NACK responses
 * 
 * Error Code Ranges:
 *   0x00-0x0F  General/common errors (OK, UNKNOWN, INVALID_COMMAND, etc.)
 *   0x10-0x1F  Parameter validation errors (INVALID_PARAM, OUT_OF_RANGE, etc.)
 *   0x20-0x4F  GunFX-specific errors (SERVO_*, SMOKE_*, TRIGGER_*)
 *   0x50-0x5F  LightFX-specific errors (LED_*, SERVO_*)
 *   0x60-0x6F  GearControl-specific errors (GEAR_*, MOTOR_*, SERVO_*, YAW_*)
 *   0x70-0x8F  Reserved for future modules
 *   0xF0-0xFF  System/transport errors (TIMEOUT, CRC_ERROR, etc.)
 */
namespace SerialError {
    // General errors (0x00-0x0F)
    constexpr uint8_t OK                    = 0x00;
    constexpr uint8_t UNKNOWN               = 0x01;
    constexpr uint8_t NOT_INITIALIZED       = 0x02;
    constexpr uint8_t INVALID_COMMAND       = 0x03;
    constexpr uint8_t MISSING_PARAMETER     = 0x04;
    constexpr uint8_t BUSY                  = 0x05;
    constexpr uint8_t NOT_SUPPORTED         = 0x06;
    constexpr uint8_t PERMISSION_DENIED     = 0x07;
    
    // Parameter validation errors (0x10-0x1F)
    constexpr uint8_t INVALID_PARAM         = 0x10;
    constexpr uint8_t PARAM_OUT_OF_RANGE    = 0x11;
    constexpr uint8_t INVALID_ID            = 0x12;
    constexpr uint8_t INVALID_VALUE         = 0x13;
    constexpr uint8_t PARAM_TOO_LONG        = 0x14;
    
    // Domain-specific errors: 0x20-0x7F (defined in respective module headers)
    
    // System/transport errors (0xF0-0xFF)
    constexpr uint8_t INTERNAL_ERROR        = 0xF0;
    constexpr uint8_t TIMEOUT               = 0xF1;
    constexpr uint8_t COMM_ERROR            = 0xF2;
    constexpr uint8_t BUFFER_OVERFLOW       = 0xF3;
    constexpr uint8_t CRC_ERROR             = 0xF4;
    constexpr uint8_t FRAMING_ERROR         = 0xF5;
    
    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case OK:                    return "OK";
            case UNKNOWN:               return "Unknown error";
            case NOT_INITIALIZED:       return "Not initialized";
            case INVALID_COMMAND:       return "Invalid command";
            case MISSING_PARAMETER:     return "Missing parameter";
            case BUSY:                  return "Busy";
            case NOT_SUPPORTED:         return "Not supported";
            case PERMISSION_DENIED:     return "Permission denied";
            case INVALID_PARAM:         return "Invalid parameter";
            case PARAM_OUT_OF_RANGE:    return "Parameter out of range";
            case INVALID_ID:            return "Invalid ID";
            case INVALID_VALUE:         return "Invalid value";
            case PARAM_TOO_LONG:        return "Parameter too long";
            case INTERNAL_ERROR:        return "Internal error";
            case TIMEOUT:               return "Timeout";
            case COMM_ERROR:            return "Communication error";
            case BUFFER_OVERFLOW:       return "Buffer overflow";
            case CRC_ERROR:             return "CRC error";
            case FRAMING_ERROR:         return "Framing error";
            default:
                if (code >= 0x20 && code <= 0x7F) return "Domain-specific error";
                return "Unknown error code";
        }
    }
    
    inline bool isGenericError(uint8_t code) {
        return (code <= 0x1F) || (code >= 0xF0);
    }
    
    inline bool isDomainError(uint8_t code) {
        return (code >= 0x20 && code <= 0x7F);
    }
}

// ============================================================================
// Command Result
// ============================================================================

/**
 * @brief Result of a command sent to slave
 * 
 * Encapsulates the result of a blocking command, including:
 * - Success/failure status
 * - Error code (0 = OK)
 * - Human-readable error message
 */
struct CommandResult {
    bool success = false;
    uint8_t errorCode = 0;
    char errorMessage[64] = "";
    
    CommandResult() = default;
    CommandResult(bool ok) : success(ok), errorCode(ok ? SerialError::OK : SerialError::UNKNOWN) {}
    CommandResult(uint8_t code) : success(code == SerialError::OK), errorCode(code) {
        if (code != SerialError::OK) {
            strncpy(errorMessage, SerialError::getMessage(code), sizeof(errorMessage) - 1);
        }
    }
    CommandResult(uint8_t code, const char* msg) : success(code == SerialError::OK), errorCode(code) {
        if (msg) strncpy(errorMessage, msg, sizeof(errorMessage) - 1);
    }
    
    operator bool() const { return success; }
    bool isTimeout() const { return errorCode == SerialError::TIMEOUT; }
    bool isNack() const { return !success && errorCode != SerialError::TIMEOUT; }
    const char* message() const { 
        return errorMessage[0] ? errorMessage : SerialError::getMessage(errorCode); 
    }
    
    static CommandResult Ack() { return CommandResult(SerialError::OK); }
    static CommandResult Nack(uint8_t code, const char* msg = nullptr) { 
        return CommandResult(code, msg ? msg : SerialError::getMessage(code)); 
    }
    static CommandResult Timeout() { return CommandResult(SerialError::TIMEOUT); }
    static CommandResult SendFailed() { return CommandResult(SerialError::COMM_ERROR, "Send failed"); }
    static CommandResult NotConnected() { return CommandResult(SerialError::NOT_INITIALIZED, "Not connected"); }
};

// ============================================================================
// Wire encoding + protocol-level helpers
// ============================================================================
//
// Wire-level utilities (CRC-8, COBS, endian helpers, packet build/encode/
// parse, framing constants) live in the SfxWire namespace (serial/wire.h)
// — callers use `SfxWire::crc8(...)`, `SfxWire::encodePacket(...)`, etc.
// Protocol-level helpers (debug name lookup, STATUS header size) live in
// CorePacket alongside the packet type constants.

#include "serial/wire.h"

// ============================================================================
// Core Packet Types (0xF0-0xFF range)
// ============================================================================

namespace CorePacket {
    constexpr uint8_t INIT        = 0xF0;  // Initialize connection: [mode:u8][flags:u8] (optional, default SLAVE/0)
    constexpr uint8_t SHUTDOWN    = 0xF1;  // Graceful shutdown
    constexpr uint8_t KEEPALIVE   = 0xF2;  // Connection heartbeat
    constexpr uint8_t INIT_READY  = 0xF3;  // Slave ready response
    constexpr uint8_t STATUS      = 0xF4;  // Status payload
    constexpr uint8_t ERROR       = 0xF5;  // Error notification
    constexpr uint8_t ACK         = 0xF6;  // Command acknowledged
    constexpr uint8_t NACK        = 0xF7;  // Command rejected with error code
    constexpr uint8_t REBOOT      = 0xF8;  // Restart device
    constexpr uint8_t BOOTSEL     = 0xF9;  // Enter bootloader
    constexpr uint8_t STATUS_REQ  = 0xFA;  // Request status
    constexpr uint8_t I2C_SCAN       = 0xFB;  // Request I2C bus scan
    constexpr uint8_t I2C_SCAN_RESULT = 0xFC;  // I2C scan response
    constexpr uint8_t LOG_MESSAGE     = 0xFD;  // [level:u8][millis:u32LE][message:str] (async, unsolicited)
    constexpr uint8_t IDENTIFY        = 0xFE;  // Query board info without triggering INIT (response uses same type + INIT_READY payload format)
    constexpr uint8_t DIAG_HISTORY    = 0xFF;  // Request diagnostic log history (sends buffered LOG_MESSAGE packets without draining)
    constexpr uint8_t STATUS_UPDATE   = 0xEF;  // Async verbose status: [source:u8][type:u8][data:variable]

    // Battery monitoring (handled by BatteryServicePolicy):
    //   BATTERY_CONFIG payload: [chemistry:u8][cellCount:u8]
    //     chemistry: 0=LiPo, 1=Li-Ion, 2=NiMH (matches BatteryChemistry enum)
    //     cellCount: 0 = re-arm auto-detect, 1..MAX_CELLS = pinned count
    constexpr uint8_t BATTERY_CONFIG  = 0xEE;

    /// STATUS response core-header size: 5×u32 + boardState:u8 + initFlags:u8.
    constexpr size_t STATUS_CORE_HEADER_SIZE = 22;

    /// Human-readable name for a packet type byte (debugging).
    const char* packetTypeToText(uint8_t type);
}

// ============================================================================
// INIT Mode & Flags
// ============================================================================

/**
 * @brief INIT packet mode byte — determines controller operating mode
 *
 * INIT payload format: [mode:u8][flags:u8]
 * If payload is empty (len==0), defaults to SLAVE mode with no flags
 * (backward-compatible with existing firmware).
 */
namespace InitMode {
    constexpr uint8_t SLAVE  = 0x00;  ///< Board controlled by HubFX master, keep-alive required
    constexpr uint8_t DIRECT = 0x01;  ///< Direct CLI/GUI control, no keep-alive

    /// @deprecated Use DIRECT — kept for backward compatibility
    constexpr uint8_t CONFIG = DIRECT;

    inline const char* getName(uint8_t mode) {
        switch (mode) {
            case SLAVE:  return "SLAVE";
            case DIRECT: return "DIRECT";
            default:     return "UNKNOWN";
        }
    }
}

// ============================================================================
// Board State — Runtime Operational State
// ============================================================================

/**
 * @brief Board operational state — reported in STATUS response
 *
 * Tracks the full lifecycle of a board including autonomous (standalone)
 * operation when a config file is loaded from flash, and idle states.
 * Distinct from InitMode which only describes the INIT command parameter.
 *
 * State transitions:
 *   Power-on → IDLE
 *   IDLE + config loaded from flash → STANDALONE
 *   IDLE/STANDALONE + INIT(SLAVE) → SLAVE
 *   IDLE/STANDALONE + INIT(DIRECT) → DIRECT
 *   SLAVE/DIRECT + SHUTDOWN/timeout → STANDALONE (if config loaded) or IDLE
 */
namespace BoardState {
    constexpr uint8_t IDLE       = 0x00;  ///< No INIT received, no config loaded — waiting
    constexpr uint8_t STANDALONE = 0x01;  ///< Config loaded from flash, running autonomously
    constexpr uint8_t SLAVE      = 0x02;  ///< INIT(SLAVE) received, master controls board
    constexpr uint8_t DIRECT     = 0x03;  ///< INIT(DIRECT) received, CLI/GUI direct control

    inline const char* getName(uint8_t state) {
        switch (state) {
            case IDLE:       return "IDLE";
            case STANDALONE: return "STANDALONE";
            case SLAVE:      return "SLAVE";
            case DIRECT:     return "DIRECT";
            default:         return "UNKNOWN";
        }
    }
}

/**
 * @brief INIT flags bitmask (second byte of INIT payload)
 */
namespace InitFlags {
    constexpr uint8_t NONE    = 0x00;
    constexpr uint8_t VERBOSE = 0x01;  ///< Enable STATUS_UPDATE async packets for in-flight operations
}

// ============================================================================
// STATUS_UPDATE Types — Generic Envelope for Verbose Async Updates
// ============================================================================

/**
 * @brief Source module identifiers for STATUS_UPDATE packets
 *
 * STATUS_UPDATE payload: [source:u8][updateType:u8][data:variable]
 * Emitted asynchronously when verbose flag is set in INIT.
 */
namespace StatusUpdateSource {
    constexpr uint8_t GUNFX       = 0x01;  ///< GunFX module
    constexpr uint8_t LIGHTFX     = 0x40;  ///< LightFX module
    constexpr uint8_t GEARCONTROL = 0x60;  ///< GearControl module
    constexpr uint8_t HUBFX       = 0x80;  ///< HubFX module
    constexpr uint8_t CORE        = 0xF0;  ///< Core system
}

/**
 * @brief Common update type codes (module-specific codes extend from these)
 *
 * Each module can define additional update types starting from 0x10+.
 * Common types 0x01-0x0F are shared across modules.
 */
namespace StatusUpdateType {
    constexpr uint8_t SERVO_POSITION    = 0x01;  ///< [servoId:u8][position_us:u16LE]
    constexpr uint8_t VOLTAGE           = 0x02;  ///< [channelId:u8][voltage_mV:u16LE]
    constexpr uint8_t CURRENT           = 0x03;  ///< [channelId:u8][current_mA:u16LE]
    constexpr uint8_t TEMPERATURE       = 0x04;  ///< [sensorId:u8][temp_C_x10:i16LE]
    constexpr uint8_t STATUS_BROADCAST  = 0x10;  ///< Full module status blob (same format as STATUS module data)
}

// ============================================================================
// Core Error Codes
// ============================================================================

namespace CoreError {
    constexpr uint8_t OK                = 0x00;
    constexpr uint8_t TIMEOUT           = 0x01;
    constexpr uint8_t NOT_CONNECTED     = 0x02;
    constexpr uint8_t NOT_INITIALIZED   = 0x03;
    
    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case OK:              return "OK";
            case TIMEOUT:         return "Timeout";
            case NOT_CONNECTED:   return "Not connected";
            case NOT_INITIALIZED: return "Not initialized";
            default:              return SerialError::getMessage(code);
        }
    }
}

// ============================================================================
// Core Statistics
// ============================================================================

struct CoreStats {
    uint32_t packets_sent = 0;
    uint32_t packets_received = 0;
    uint32_t crc_errors = 0;
    uint32_t framing_errors = 0;
};

// ============================================================================
// Core Data Types
// ============================================================================

/**
 * @brief Board and firmware information for INIT_READY response
 */
struct CoreBoardInfo {
    char deviceName[32] = "";
    char firmwareVersion[16] = "";
    char platform[32] = "";
    uint32_t cpuFrequencyMHz = 0;
    uint32_t freeRamBytes = 0;
    uint32_t buildNumber = 0;
    /// Bitmask of `CoreCapability::*` feature bits advertised by this
    /// board.  Single source of truth for subsystem + service + port-kind
    /// presence — see the `CoreCapability` block below for the full
    /// catalog.  Lets clients gate UI / queries without per-board
    /// if-ladders or speculative status probes.  Appended to
    /// INIT_READY/IDENTIFY payload (Rule 11 append-only): firmware
    /// that pre-dates this field is treated as `capabilities == 0`.
    uint32_t capabilities = 0;
};

// ──────────────────────────────────────────────────────────────────────
// CoreCapability — board feature bitmask
// ──────────────────────────────────────────────────────────────────────
//
// 32-bit feature catalog carried in `CoreBoardInfo.capabilities` and
// emitted as the tail of every INIT_READY / IDENTIFY payload.  Acts as
// the SINGLE source of truth for "which subsystems does this board
// support" — masters use it to gate UI, skip speculative queries, and
// route commands appropriately.
//
// Layout is grouped by domain.  **Bits are append-only (Rule 11)** —
// never renumber; assign new features to reserved positions in their
// domain block, or extend with a new block.
//
//   Domain                     Bits        Notes
//   ────────────────────────── ─────────── ──────────────────────────────
//   Storage                    0..1, 11..15
//   Comm / bus                 3, 6
//   Logic / config             4..5
//   Audio                      2
//   Sensors                    7
//   Generic-expander services  8..10       added 2026-05
//   Port-kind presence         16..19      added 2026-05
//   Reserved                   20..31
//
// Adding a feature: pick the lowest free bit in the matching domain,
// add a constant below + a mirror in [app/go/protocol/core/core.go],
// and either OR it into a policy's `kCapabilityBits` (compile-time) or
// have the board sketch call `core().addCapability(...)` at runtime.
//
namespace CoreCapability {
    // ── Storage ──────────────────────────────────────────────────────
    constexpr uint32_t FLASH         = 1u << 0;   // LittleFS storage commands
    constexpr uint32_t SD            = 1u << 1;   // SD card storage commands

    // ── Audio ────────────────────────────────────────────────────────
    constexpr uint32_t AUDIO         = 1u << 2;   // AudioMixer + audio playback commands

    // ── Comm / bus ───────────────────────────────────────────────────
    constexpr uint32_t USB_HOST      = 1u << 3;   // USB host stack + device enumeration
    constexpr uint32_t EXPANDER_BUS  = 1u << 6;   // Master can enumerate / route to expanders
    constexpr uint32_t SLAVE_BUS     = EXPANDER_BUS;  // legacy alias — pre-rename code path

    // ── Logic / config ───────────────────────────────────────────────
    constexpr uint32_t ENGINE        = 1u << 4;   // Sound-engine commands
    constexpr uint32_t CONFIG        = 1u << 5;   // YAML config store commands

    // ── Sensors ──────────────────────────────────────────────────────
    constexpr uint32_t BATTERY       = 1u << 7;   // Battery sensor present

    // ── Generic-expander services (bits 8..10) ───────────────────────
    constexpr uint32_t PORTS         = 1u << 8;   // PortServicePolicy raw-port commands (0x10..0x3F)
    constexpr uint32_t ROLES         = 1u << 9;   // RoleServicePolicy attach/detach + per-role commands (0x40..0x7F)
    constexpr uint32_t TOPOLOGY      = 1u << 10;  // TopologyServicePolicy GUID-addressed access (master only)

    // ── Port-kind presence (bits 16..19) ─────────────────────────────
    // True if the board declares at least one port of that kind.
    // Populated automatically by `BoardOf<>::begin()` from the static
    // `kServoPorts` / `kPwmPorts` / `kHBridgePorts` / `kInputPorts`
    // descriptor tuples.
    constexpr uint32_t HAS_SERVO_PORTS   = 1u << 16;
    constexpr uint32_t HAS_PWM_PORTS     = 1u << 17;
    constexpr uint32_t HAS_HBRIDGE_PORTS = 1u << 18;
    constexpr uint32_t HAS_INPUT_PORTS   = 1u << 19;

    // bits 20..31 reserved.
}

// ============================================================================
// I2C Scan Data Types
// ============================================================================

/**
 * @brief I2C scan result entry for one expected device
 */
struct I2CScanEntry {
    uint8_t address = 0;       // Expected I2C address
    bool found = false;        // Device ACK'd the address
    bool identified = false;   // Device-specific ID verified (e.g., INA226 MFG_ID)
};

/**
 * @brief I2C bus scan result
 *
 * Contains results for expected devices plus any additional devices
 * found on the bus.
 *
 * Wire format for I2C_SCAN_RESULT (0xFC):
 *   [numExpected:u8]
 *   Per expected device × N (3 bytes each):
 *     [address:u8][found:u8][identified:u8]
 *   [numExtra:u8]
 *   Per extra device × M (1 byte each):
 *     [address:u8]
 */
struct I2CScanResult {
    static constexpr uint8_t MAX_EXPECTED = 8;
    static constexpr uint8_t MAX_EXTRA = 16;

    I2CScanEntry expected[MAX_EXPECTED];
    uint8_t numExpected = 0;
    uint8_t extraAddresses[MAX_EXTRA];
    uint8_t numExtra = 0;
};

// ============================================================================
// Callback Types
// ============================================================================

using PacketRxCallback = std::function<void(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len)>;

// Core command callbacks
using CoreInitCallback = std::function<void(uint8_t mode, uint8_t flags)>;
using CoreShutdownCallback = std::function<void()>;
using CoreRebootCallback = std::function<void()>;
using CoreBootselCallback = std::function<void()>;
using CoreKeepaliveCallback = std::function<void()>;

/**
 * @brief Callback for appending module-specific data to STATUS response
 *
 * Called by BoardServicePolicy::sendStatus() after writing the 22-byte
 * core header. The callback writes module-specific status bytes into
 * the buffer.
 *
 * @param buffer Pointer to write position in payload buffer (after core header)
 * @param maxLen Maximum bytes available (typically STATUS_CORE_HEADER_SIZE subtracted)
 * @return Number of bytes written to buffer
 */
using StatusDataCallback = std::function<size_t(uint8_t* buffer, size_t maxLen)>;

/**
 * @brief Callback for I2C bus scan
 *
 * Called by BoardServicePolicy when I2C_SCAN is received. The callback
 * should perform the scan and return the result.
 */
using I2CScanCallback = std::function<I2CScanResult()>;

// ============================================================================
// Command Handle Result
// ============================================================================

/**
 * @brief Result of a policy attempting to handle a packet.
 *
 * BoardServer walks its policy tuple; the first policy whose ownsType()
 * returns true gets `handle()` called.  The result decides whether
 * BoardServer continues walking or stops:
 *   - Handled: ACK/NACK already sent, stop.
 *   - NotMyCommand: skip this policy, keep walking.
 *   - Error: handler couldn't process due to internal issue (rare).
 */
enum class CommandHandleResult : uint8_t {
    Handled,
    NotMyCommand,
    Error,
};

// ============================================================================
// INIT_READY Payload Encoding/Decoding
// ============================================================================

namespace CorePayload {

/**
 * @brief Encode CoreBoardInfo into INIT_READY payload
 * @param info Board information
 * @param payload Output buffer (must be at least 64 bytes)
 * @return Payload length
 */
size_t encodeInitReady(const CoreBoardInfo& info, uint8_t* payload);

/**
 * @brief Decode INIT_READY payload into CoreBoardInfo
 * @param payload Payload data
 * @param len Payload length
 * @param info Output board information
 * @return true if successful
 */
bool decodeInitReady(const uint8_t* payload, size_t len, CoreBoardInfo& info);

} // namespace CorePayload

// ============================================================================
// Server Handler Macros - Reduce Boilerplate in Command Handlers
// ============================================================================
//
// These macros reduce repetitive code in Server::tryProcess() handlers.
// They use local variables: `len`, `sendNack()`, and return CommandHandleResult.
//
// Usage example:
//   case MyPacket::SOME_CMD:
//       SFX_REQUIRE_LEN(3);
//       SFX_VALIDATE(isValid(payload[0]), MyError::INVALID);
//       SFX_DISPATCH(_callback, payload[0], payload[1]);
//

/**
 * @brief Check minimum payload length, NACK and return if too short
 * 
 * @param min_len Minimum required payload length
 */
#define SFX_REQUIRE_LEN(min_len) \
    do { \
        if (len < (min_len)) { \
            sendNack(SerialError::MISSING_PARAMETER); \
            return CommandHandleResult::Handled; \
        } \
    } while(0)

/**
 * @brief Validate a condition, NACK and return if false
 * 
 * @param condition Boolean expression to validate
 * @param error_code Error code to send if condition is false
 */
#define SFX_VALIDATE(condition, error_code) \
    do { \
        if (!(condition)) { \
            sendNack(error_code); \
            return CommandHandleResult::Handled; \
        } \
    } while(0)

/**
 * @brief Dispatch to callback and send ACK/NACK based on result
 * 
 * If callback is null, sends ACK (no-op mode).
 * If callback returns 0 (OK), sends ACK.
 * Otherwise sends NACK with the returned error code.
 * 
 * @param callback The callback function pointer/lambda
 * @param ... Arguments to pass to callback
 */
#define SFX_DISPATCH(callback, ...) \
    do { \
        if (callback) { \
            uint8_t _sfx_result = callback(__VA_ARGS__); \
            if (_sfx_result == 0) sendAck(); \
            else sendNack(_sfx_result); \
        } else { \
            sendAck(); \
        } \
        return CommandHandleResult::Handled; \
    } while(0)

/**
 * @brief Complete handler for simple single-channel commands
 * 
 * Validates channel/ID, then dispatches to callback.
 * 
 * @param validator Validation function (e.g., LightFxSpec::isValidLedChannel)
 * @param error_code Error code if validation fails
 * @param callback The callback to dispatch to
 */
#define SFX_HANDLE_CHANNEL_CMD(validator, error_code, callback) \
    do { \
        SFX_REQUIRE_LEN(1); \
        uint8_t _sfx_ch = payload[0]; \
        SFX_VALIDATE(validator(_sfx_ch), error_code); \
        SFX_DISPATCH(callback, _sfx_ch); \
    } while(0)

// Wave-5 note:
//   ICommandHandler + CommandRouter + ScaleFX namespace have been removed.
//   The dispatcher is now BoardServer<...Policies> (system_service.h) +
//   PacketReader<TDispatch> (packet_reader.h).
//
//     PacketReader<BoardServer<BoardServicePolicy, ModulePolicy, ...>> reader;
//     reader.begin(&Serial, &board);

#endif // SERIAL_CORE_H
