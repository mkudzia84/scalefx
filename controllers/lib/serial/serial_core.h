/*
 * Serial Core - Protocol, Interface, and Slave Command Handling
 *
 * Core serial communication infrastructure for ScaleFX controllers.
 * Provides protocol encoding/decoding, abstract interface for packet-based
 * communication, and the slave-side system command handler.
 *
 * Protocol:
 *   Packet Format: [type:u8][len:u8][payload:0-64 bytes][crc8:u8]
 *   Framing: COBS encoded, followed by 0x00 frame delimiter
 *   CRC: CRC-8 polynomial 0x07 over type+len+payload
 *
 * Packet Type Ranges:
 *   0x01-0x2F  GunFX commands (trigger, servo, smoke) - see serial_gunfx.h
 *   0x40-0x5F  LightFX commands (LED, servo, power) - see serial_lightfx.h
 *   0x60-0x7F  GearControl commands (gear, servo, yaw) - see serial_gearcontrol.h
 *   0x80-0x8F  Reserved for future modules
 *   0xF0-0xFF  Universal system commands (INIT, ACK, NACK, etc.)
 *
 * Components:
 *   CoreProtocol       - COBS encoding, CRC-8, packet utilities
 *   ISerialCore        - Abstract interface for serial communication
 *   CoreCommandServer  - Server-side system command handler
 *   CoreBoardInfo      - Device information for INIT_READY response
 *   CoreStats          - Packet statistics
 *
 * Core Commands (0xF0-0xFF, handled by CoreCommandServer):
 *   INIT (0xF0)        - Initialize connection
 *   SHUTDOWN (0xF1)    - Graceful shutdown
 *   KEEPALIVE (0xF2)   - Connection heartbeat
 *   INIT_READY (0xF3)  - Slave ready response
 *   REBOOT (0xF8)      - Restart device
 *   BOOTSEL (0xF9)     - Enter bootloader
 *
 * Response Types:
 *   ACK (0xF6)         - Command acknowledged
 *   NACK (0xF7)        - Command rejected with error code
 *   STATUS (0xF4)      - Status payload
 *   ERROR (0xF5)       - Error notification
 *
 * Usage (Slave Side):
 *   CoreCommandServer coreServer;
 *   coreServer.begin(&Serial);
 *   coreServer.setBoardInfo("GunFX", "1.0.0", "RP2040", 125, 200000);
 *   // In packet callback: coreServer.tryHandle(type, payload, len);
 *
 * For Master Side:
 *   See SerialBus in serial_bus.h which implements ISerialCore.
 */

#ifndef SERIAL_CORE_H
#define SERIAL_CORE_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <functional>
#include "serial_error.h"

// Forward declaration
class UsbHost;

// ============================================================================
// Protocol Constants & Buffer Sizes
// ============================================================================

namespace CoreProtocol {

// Buffer sizes
constexpr size_t MAX_PAYLOAD_SIZE = 64;
constexpr size_t MAX_PACKET_SIZE = 2 + MAX_PAYLOAD_SIZE + 1;
constexpr size_t COBS_BUFFER_SIZE = MAX_PACKET_SIZE + MAX_PACKET_SIZE / 254 + 2;
constexpr uint8_t FRAME_DELIMITER = 0x00;

// ============================================================================
// Binary Protocol Functions
// ============================================================================

/**
 * @brief Calculate CRC-8 checksum
 * @param data Input data
 * @param len Data length
 * @return CRC-8 value
 */
uint8_t crc8(const uint8_t* data, size_t len);

/**
 * @brief COBS encode data
 * @param input Input data
 * @param length Input length
 * @param output Output buffer
 * @return Encoded length
 */
size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output);

/**
 * @brief COBS decode data
 * @param input Encoded data
 * @param length Encoded length
 * @param output Output buffer
 * @param maxOutput Maximum output size
 * @return Decoded length, 0 on error
 */
size_t cobsDecode(const uint8_t* input, size_t length, uint8_t* output, size_t maxOutput);

/**
 * @brief Build raw packet (type + len + payload + crc)
 * @param output Output buffer
 * @param type Packet type
 * @param payload Payload data
 * @param payloadLen Payload length
 * @return Packet length, 0 on error
 */
size_t buildPacket(uint8_t* output, uint8_t type, const uint8_t* payload, size_t payloadLen);

/**
 * @brief Build and COBS-encode packet with frame delimiter
 * @param output Output buffer (must be COBS_BUFFER_SIZE)
 * @param type Packet type
 * @param payload Payload data
 * @param payloadLen Payload length
 * @return Total encoded length including delimiter, 0 on error
 */
size_t encodePacket(uint8_t* output, uint8_t type, const uint8_t* payload, size_t payloadLen);

/**
 * @brief Parse and verify a decoded packet
 * @param input Decoded packet data
 * @param length Packet length
 * @param type Output packet type
 * @param payload Output pointer to payload
 * @param payloadLen Output payload length
 * @return true if valid, false on CRC error or malformed
 */
bool parsePacket(const uint8_t* input, size_t length, uint8_t* type, 
                 const uint8_t** payload, size_t* payloadLen);

/**
 * @brief Get text name for packet type (debugging)
 * @param type Packet type
 * @return Human-readable name
 */
const char* packetTypeToText(uint8_t type);

// ============================================================================
// Payload Encoding Helpers
// ============================================================================

inline void putU16LE(uint8_t* buf, uint16_t value) {
    buf[0] = value & 0xFF;
    buf[1] = (value >> 8) & 0xFF;
}

inline uint16_t getU16LE(const uint8_t* buf) {
    return buf[0] | ((uint16_t)buf[1] << 8);
}

inline void putI16LE(uint8_t* buf, int16_t value) {
    putU16LE(buf, (uint16_t)value);
}

inline int16_t getI16LE(const uint8_t* buf) {
    return (int16_t)getU16LE(buf);
}

inline void putU32LE(uint8_t* buf, uint32_t value) {
    buf[0] = value & 0xFF;
    buf[1] = (value >> 8) & 0xFF;
    buf[2] = (value >> 16) & 0xFF;
    buf[3] = (value >> 24) & 0xFF;
}

inline uint32_t getU32LE(const uint8_t* buf) {
    return buf[0] | 
           ((uint32_t)buf[1] << 8) | 
           ((uint32_t)buf[2] << 16) | 
           ((uint32_t)buf[3] << 24);
}

} // namespace CoreProtocol

// ============================================================================
// Core Packet Types (0xF0-0xFF range)
// ============================================================================

namespace CorePacket {
    constexpr uint8_t INIT        = 0xF0;  // Initialize connection
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
// Callback Types
// ============================================================================

using PacketRxCallback = std::function<void(uint8_t type, const uint8_t* payload, size_t len)>;

// Core command callbacks
using CoreInitCallback = std::function<void()>;
using CoreShutdownCallback = std::function<void()>;
using CoreRebootCallback = std::function<void()>;
using CoreBootselCallback = std::function<void()>;
using CoreKeepaliveCallback = std::function<void()>;

/**
 * @brief Callback for appending module-specific data to STATUS response
 * 
 * Called by CoreCommandServer::sendStatus() after writing the 12-byte core header.
 * The callback should write module-specific status bytes into the buffer.
 * 
 * @param buffer Pointer to write position in payload buffer (after core header)
 * @param maxLen Maximum bytes available (typically 52 = 64 - 12)
 * @return Number of bytes written to buffer
 */
using StatusDataCallback = std::function<size_t(uint8_t* buffer, size_t maxLen)>;

// ============================================================================
// ISerialCore - Abstract Interface for Serial Communication
// ============================================================================

/**
 * @brief Abstract base class for serial bus communication
 * 
 * Defines the interface for packet-based binary serial communication.
 * Only binary COBS-encoded protocol is supported.
 */
class ISerialCore {
public:
    virtual ~ISerialCore() = default;

    // ========================================================================
    // Lifecycle
    // ========================================================================
    
    /**
     * @brief Initialize with USB host connection
     * @param usbHost Pointer to USB host
     * @param deviceIndex CDC device index
     * @return true if successful
     */
    virtual bool begin(UsbHost* usbHost, int deviceIndex) = 0;
    
    /**
     * @brief End communication
     */
    virtual void end() = 0;
    
    /**
     * @brief Set the target device index
     */
    virtual void setDevice(int deviceIndex) = 0;

    // ========================================================================
    // Packet Transmission
    // ========================================================================
    
    /**
     * @brief Send a packet with optional payload
     * @param type Packet type
     * @param payload Payload data (can be nullptr)
     * @param len Payload length
     * @return Bytes sent, or -1 on error
     */
    virtual int sendPacket(uint8_t type, const uint8_t* payload = nullptr, size_t len = 0) = 0;
    
    /**
     * @brief Send INIT command
     */
    virtual int sendInit() { return sendPacket(CorePacket::INIT); }
    
    /**
     * @brief Send SHUTDOWN command
     * @note Slave will ACK since device stays running
     */
    virtual int sendShutdown() { return sendPacket(CorePacket::SHUTDOWN); }
    
    /**
     * @brief Send REBOOT command (fire-and-forget)
     * @note No ACK expected - device reboots immediately
     */
    virtual int sendReboot() { return sendPacket(CorePacket::REBOOT); }
    
    /**
     * @brief Send BOOTSEL command (fire-and-forget)
     * @note No ACK expected - device enters bootloader immediately
     */
    virtual int sendBootsel() { return sendPacket(CorePacket::BOOTSEL); }
    
    /**
     * @brief Send KEEPALIVE packet
     */
    virtual int sendKeepalive() { return sendPacket(CorePacket::KEEPALIVE); }
    
    /**
     * @brief Send ACK response
     */
    virtual int sendAck() { return sendPacket(CorePacket::ACK); }
    
    /**
     * @brief Send NACK response with error code
     * @param errorCode Error code from SerialError namespace
     */
    virtual int sendNack(uint8_t errorCode) {
        return sendPacket(CorePacket::NACK, &errorCode, 1);
    }
    
    /**
     * @brief Send INIT_READY response with board info
     * @param info Board information
     */
    int sendInitReady(const CoreBoardInfo& info);
    
    /**
     * @brief Send STATUS_REQ command
     */
    virtual int sendStatusRequest() { return sendPacket(CorePacket::STATUS_REQ); }

    // ========================================================================
    // Packet Reception
    // ========================================================================
    
    /**
     * @brief Set callback for received packets
     */
    virtual void onPacketReceived(PacketRxCallback callback) = 0;
    
    /**
     * @brief Process incoming data
     * @return Number of packets processed
     */
    virtual int process() = 0;

    // ========================================================================
    // Keepalive Management
    // ========================================================================
    
    /**
     * @brief Set keepalive interval (0 to disable)
     */
    virtual void setKeepaliveInterval(unsigned long intervalMs) = 0;
    
    /**
     * @brief Process keepalive timing
     * @return true if keepalive was sent
     */
    virtual bool processKeepalive() = 0;

    // ========================================================================
    // Status
    // ========================================================================
    
    /**
     * @brief Check if connected to device
     */
    virtual bool isConnected() const = 0;
    
    /**
     * @brief Check if initialized
     */
    virtual bool isInitialized() const = 0;
    
    /**
     * @brief Get statistics
     */
    virtual const CoreStats& stats() const = 0;
    
    /**
     * @brief Reset statistics
     */
    virtual void resetStats() = 0;
};

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
};

// ============================================================================
// CoreCommandServer (Server Side)
// ============================================================================

/**
 * @brief Handles core system commands on server devices
 * 
 * Processes INIT, SHUTDOWN, REBOOT, BOOTSEL, and KEEPALIVE commands.
 * Implements ICommandHandler for use with CommandRouter.
 */
class CoreCommandServer : public ICommandHandler {
public:
    CoreCommandServer() = default;
    ~CoreCommandServer() override = default;
    
    /**
     * @brief Initialize the handler
     * @param serial Serial port for sending responses (server side uses Serial)
     */
    void begin(Stream* serial);
    
    /**
     * @brief Set board information for INIT_READY response
     */
    void setBoardInfo(const char* deviceName, const char* firmwareVersion,
                      const char* platform, uint32_t cpuMHz, uint32_t freeRam,
                      uint32_t buildNumber = 0);
    
    /**
     * @brief Try to handle a received packet
     * @param type Packet type
     * @param payload Payload data
     * @param len Payload length
     * @return true if handled (core command), false if not a core command
     */
    bool tryHandle(uint8_t type, const uint8_t* payload, size_t len);
    
    // ========================================================================
    // ICommandHandler Interface
    // ========================================================================
    
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override {
        return tryHandle(type, payload, len) ? CommandHandleResult::Handled : CommandHandleResult::NotMyCommand;
    }
    
    const char* handlerName() const override { return "CoreCommandServer"; }
    
    /**
     * @brief Update last activity timestamp
     */
    void updateActivity() { _lastActivityMs = millis(); }
    
    /**
     * @brief Check for connection timeout
     * @param timeoutMs Timeout in milliseconds (0 to disable)
     * @return true if timed out
     */
    bool checkTimeout(unsigned long timeoutMs);
    
    /**
     * @brief Get time since last activity
     */
    unsigned long lastActivityMs() const { return _lastActivityMs; }
    
    /**
     * @brief Check if initialized (INIT received)
     */
    bool isInitialized() const { return _initialized; }
    
    /**
     * @brief Reset state (on connection loss or new INIT)
     */
    void reset();
    
    // Callbacks
    void onInit(CoreInitCallback callback) { _initCallback = callback; }
    void onShutdown(CoreShutdownCallback callback) { _shutdownCallback = callback; }
    void onReboot(CoreRebootCallback callback) { _rebootCallback = callback; }
    void onBootsel(CoreBootselCallback callback) { _bootselCallback = callback; }
    void onKeepalive(CoreKeepaliveCallback callback) { _keepaliveCallback = callback; }
    
    /**
     * @brief Register callback for appending module-specific data to STATUS response
     * 
     * The callback receives a buffer pointer and max length, and should write
     * module-specific status bytes. The bytes are appended after the 12-byte
     * core header: [counter:u32LE][uptime:u32LE][freeRam:u32LE].
     */
    void onStatusData(StatusDataCallback callback) { _statusDataCallback = callback; }
    
    // Statistics
    uint32_t commandCounter() const { return _commandCounter; }
    uint32_t keepaliveCounter() const { return _keepaliveCounter; }
    
    /**
     * @brief Update free RAM value for STATUS response (call periodically)
     * 
     * The core STATUS header includes freeRam. Since CoreCommandServer
     * stores this from setBoardInfo(), call this to keep it current.
     * Example: coreServer.updateFreeRam(rp2040.getFreeHeap());
     */
    void updateFreeRam(uint32_t freeRam) { _boardInfo.freeRamBytes = freeRam; }
    
    /**
     * @brief Send ACK packet
     */
    void sendAck();
    
    /**
     * @brief Send NACK packet with error code
     */
    void sendNack(uint8_t errorCode);

private:
    void sendInitReady();
    void handleInit(const uint8_t* payload, size_t len);
    void sendStatus();
    
    Stream* _serial = nullptr;
    CoreBoardInfo _boardInfo;
    bool _initialized = false;
    unsigned long _lastActivityMs = 0;
    uint32_t _commandCounter = 0;
    uint32_t _keepaliveCounter = 0;
    
    CoreInitCallback _initCallback;
    CoreShutdownCallback _shutdownCallback;
    CoreRebootCallback _rebootCallback;
    CoreBootselCallback _bootselCallback;
    CoreKeepaliveCallback _keepaliveCallback;
    StatusDataCallback _statusDataCallback;
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

#endif // SERIAL_CORE_H
