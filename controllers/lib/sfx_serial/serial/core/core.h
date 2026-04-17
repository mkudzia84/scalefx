/*
 * Serial Core - Protocol, Errors, Interfaces, and Command Routing
 *
 * Central header for ScaleFX serial communication. Contains:
 *   - Error codes (SerialError namespace) and CommandResult struct
 *   - Protocol encoding/decoding (CoreProtocol namespace)
 *   - Core packet types and data structures
 *   - ICommandHandler interface and CommandRouter
 *   - Server handler macros (SFX_*)
 *
 * Protocol:
 *   Packet Format: [type:u8][tag:u8][len:u16LE][payload:0-512 bytes][crc8:u8]
 *   Framing: COBS encoded, followed by 0x00 frame delimiter
 *   CRC: CRC-8 polynomial 0x07 over type+tag+len(2 bytes)+payload
 *   Tag: 0x00 = async/unsolicited, 0x01-0xFF = request correlation ID
 *
 * Packet Type Ranges:
 *   0x01-0x2F  GunFX commands - see serial_gunfx.h
 *   0x40-0x5F  LightFX commands - see serial_lightfx.h
 *   0x60-0x7F  GearControl commands - see serial_gearcontrol.h
 *   0x80-0xA3  HubFX commands - see hubfx_protocol.h
 *   0xA4-0xA6  Streaming protocol - see serial_stream.h
 *   0xF0-0xFF  Universal system commands (INIT, ACK, NACK, etc.)
 *
 * CoreCommandServer is defined in serial_bus_server.h (extends BusServer).
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
// Protocol Constants & Buffer Sizes
// ============================================================================
//
// Wire encoding utilities (CRC-8, COBS, endian helpers, packet build/encode/parse)
// are defined in SfxWire namespace (platform/sfx_wire.h) and re-exported here
// in CoreProtocol for backward compatibility. All existing code using
// CoreProtocol::crc8(), CoreProtocol::encodePacket(), etc. continues to work.
//

#include "platform/sfx_wire.h"

namespace CoreProtocol {

// --- Backward-compatible aliases from SfxWire ---
using SfxWire::HEADER_SIZE;
using SfxWire::MAX_PAYLOAD_SIZE;
using SfxWire::MAX_PACKET_SIZE;
using SfxWire::COBS_BUFFER_SIZE;
using SfxWire::FRAME_DELIMITER;
using SfxWire::TAG_ASYNC;
using SfxWire::PICO_MAX_PAYLOAD;
using SfxWire::ESP32_MAX_PAYLOAD;

using SfxWire::crc8;
using SfxWire::cobsEncode;
using SfxWire::cobsDecode;
using SfxWire::buildPacket;
using SfxWire::encodePacket;
using SfxWire::parsePacket;

using SfxWire::putU16LE;
using SfxWire::getU16LE;
using SfxWire::putI16LE;
using SfxWire::getI16LE;
using SfxWire::putU32LE;
using SfxWire::getU32LE;

/// STATUS response core header size in bytes (5×u32 + boardState:u8 + initFlags:u8)
constexpr size_t STATUS_CORE_HEADER_SIZE = 22;

/**
 * @brief Get text name for packet type (debugging)
 * @param type Packet type
 * @return Human-readable name
 */
const char* packetTypeToText(uint8_t type);

} // namespace CoreProtocol

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

    // Battery monitoring (handled by BatteryServerT, not CoreCommandServer):
    //   BATTERY_CONFIG payload: [chemistry:u8][cellCount:u8]
    //     chemistry: 0=LiPo, 1=Li-Ion, 2=NiMH (matches BatteryChemistry enum)
    //     cellCount: 0 = re-arm auto-detect, 1..MAX_CELLS = pinned count
    constexpr uint8_t BATTERY_CONFIG  = 0xEE;
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
};

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
 * Called by CoreCommandServer::sendStatus() after writing the 22-byte core header.
 * The callback should write module-specific status bytes into the buffer.
 * 
 * @param buffer Pointer to write position in payload buffer (after core header)
 * @param maxLen Maximum bytes available (typically STATUS_CORE_HEADER_SIZE subtracted)
 * @return Number of bytes written to buffer
 */
using StatusDataCallback = std::function<size_t(uint8_t* buffer, size_t maxLen)>;

/**
 * @brief Callback for I2C bus scan
 * 
 * Called by CoreCommandServer when I2C_SCAN is received.
 * The callback should perform the scan and return the result.
 */
using I2CScanCallback = std::function<I2CScanResult()>;



// ============================================================================
// Command Handler Interface
// ============================================================================

/**
 * @brief Result of attempting to process a command
 * 
 * Used by all command handlers to indicate how a command was processed
 * in the Chain of Responsibility pattern.
 */
enum class CommandHandleResult : uint8_t {
    Handled,        // Command was recognized and processed (ACK/NACK already sent)
    NotMyCommand,   // Command not recognized by this handler, try next
    Error           // Handler error (couldn't process due to internal issue)
};

/**
 * @brief Interface for binary command handlers
 * 
 * Implementations should:
 * - Return Handled if the packet was recognized (send ACK/NACK as appropriate)
 * - Return NotMyCommand if the packet should be passed to the next handler
 * - Return Error only for internal handler errors
 */
class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;
    
    /**
     * @brief Try to process a binary packet
     * @param type Packet type byte
     * @param payload Pointer to payload data
     * @param len Length of payload
     * @return CommandHandleResult indicating how the packet was handled
     */
    virtual CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) = 0;
    
    /**
     * @brief Get the name of this handler (for debugging)
     */
    virtual const char* handlerName() const = 0;

    /**
     * @brief Set the request correlation tag for the current packet being processed.
     *
     * Called by CommandRouter before tryProcess(). Handlers should echo this tag
     * in their ACK/NACK responses so the client can match responses to requests.
     * Tag 0x00 = unsolicited/async, 0x01-0xFF = client-assigned correlation ID.
     */
    void setCurrentTag(uint8_t tag) { _currentTag = tag; }
    uint8_t currentTag() const { return _currentTag; }

protected:
    uint8_t _currentTag = 0;  ///< Tag from the request currently being processed
};

// ============================================================================
// CoreCommandServer — Forward Declaration
// ============================================================================
//
// CoreCommandServer extends BusServer and is defined in serial_bus_server.h.
// This forward declaration allows headers that only need pointers/references
// (e.g. sfx_server.h) to work without pulling in the full definition.
//
class CoreCommandServer;

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

// ============================================================================
// Command Router - Routes Packets Through Handler Chain
// ============================================================================
//
// Chain of Responsibility pattern: handlers are tried in sequence until one
// processes the command. If none handles it, a NACK is returned to the sender.
//

namespace ScaleFX {

/**
 * @brief Routes binary packets through a chain of handlers
 * 
 * Reads serial input, decodes COBS packets, verifies CRC, and passes packets
 * through handlers in sequence until one handles it or all decline.
 */
class CommandRouter {
public:
    static constexpr size_t MAX_HANDLERS = 8;
    static constexpr size_t RX_BUFFER_SIZE = CoreProtocol::COBS_BUFFER_SIZE;
    
    // Callback type for unhandled packets
    using NackCallback = std::function<void(uint8_t errorCode, uint8_t packetType)>;
    
    CommandRouter() = default;
    
    /**
     * @brief Initialize the router
     * @param serial Stream to read from
     * @param nackFunc Function to call when no handler processes a packet
     */
    void begin(Stream* serial, NackCallback nackFunc = nullptr) {
        _serial = serial;
        _nackCallback = nackFunc;
        _rxIndex = 0;
        _handlerCount = 0;
        _lastActivityMs = 0;
    }
    
    /**
     * @brief Add a handler to the chain
     * @param handler Handler to add (order matters - first added is tried first)
     * @return true if added successfully
     */
    bool addHandler(ICommandHandler* handler) {
        if (_handlerCount >= MAX_HANDLERS || !handler) return false;
        _handlers[_handlerCount++] = handler;
        return true;
    }
    
    /**
     * @brief Clear all handlers
     */
    void clearHandlers() {
        _handlerCount = 0;
    }
    
    /**
     * @brief Process incoming serial data
     * 
     * Reads available bytes, decodes COBS packets, and routes
     * complete packets through the handler chain.
     * 
     * @return Number of packets processed
     */
    int process() {
        if (!_serial) return 0;
        
        int packetsProcessed = 0;
        
        while (_serial->available()) {
            uint8_t b = _serial->read();
            _lastActivityMs = millis();
            
            if (b == CoreProtocol::FRAME_DELIMITER) {
                if (_rxIndex > 0) {
                    processFrame(_rxBuffer, _rxIndex);
                    packetsProcessed++;
                    _rxIndex = 0;
                }
            } else if (_rxIndex < RX_BUFFER_SIZE) {
                _rxBuffer[_rxIndex++] = b;
            } else {
                _rxIndex = 0;  // Buffer overflow - reset
            }
        }
        
        return packetsProcessed;
    }
    
    /**
     * @brief Get time of last activity
     */
    unsigned long lastActivityMs() const { return _lastActivityMs; }
    
private:
    void processFrame(const uint8_t* frame, size_t frameLen) {
        uint8_t decoded[CoreProtocol::MAX_PACKET_SIZE];
        size_t decodedLen = CoreProtocol::cobsDecode(frame, frameLen, decoded, sizeof(decoded));
        
        if (decodedLen < 5) return;  // Minimum: type + tag + len(2) + crc
        
        uint8_t type;
        uint8_t tag;
        const uint8_t* payload;
        size_t payloadLen;
        
        if (!CoreProtocol::parsePacket(decoded, decodedLen, &type, &tag, &payload, &payloadLen)) {
            return;  // CRC error or malformed packet
        }
        
        routePacket(type, tag, payload, payloadLen);
    }
    
    void routePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) {
        for (size_t i = 0; i < _handlerCount; i++) {
            _handlers[i]->setCurrentTag(tag);
            CommandHandleResult result = _handlers[i]->tryProcess(type, payload, len);
            
            if (result == CommandHandleResult::Handled) {
                return;
            }
        }
        
        // No handler processed the packet
        if (_nackCallback) {
            _nackCallback(SerialError::INVALID_COMMAND, type);
        }
    }
    
    Stream* _serial = nullptr;
    NackCallback _nackCallback;
    
    ICommandHandler* _handlers[MAX_HANDLERS] = {nullptr};
    size_t _handlerCount = 0;
    
    uint8_t _rxBuffer[RX_BUFFER_SIZE];
    size_t _rxIndex = 0;
    unsigned long _lastActivityMs = 0;
};

} // namespace ScaleFX

using CommandRouter = ScaleFX::CommandRouter;

#endif // SERIAL_CORE_H
