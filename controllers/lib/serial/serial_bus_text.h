/*
 * Serial Bus Text - Human-Readable Protocol Implementation
 * 
 * Text-based serial bus for testing and debugging.
 * Uses line-based commands instead of binary COBS packets.
 * 
 * Format:
 *   Commands: COMMAND_NAME [key=value ...]\n
 *   Examples:
 *     INIT\n
 *     TRIGGER_ON rpm=600\n
 *     SERVO_SET id=1 us=1500\n
 *     STATUS firing=1 flash=0 heater=1 fan=0\n
 */

#ifndef SERIAL_BUS_TEXT_H
#define SERIAL_BUS_TEXT_H

#include "serial_bus_base.h"
#include <Arduino.h>
#include <Stream.h>
#include <map>
#include <string>

constexpr size_t TEXT_RX_BUFFER_SIZE = 512;
constexpr size_t TEXT_MAX_LINE_LENGTH = 256;

// ============================================================================
// SerialBusText - Text Protocol Implementation
// ============================================================================

/**
 * @brief Text-based serial bus for testing
 * 
 * Implements human-readable line-based protocol.
 * Can work with hardware Serial or mock streams.
 */
class SerialBusText : public SerialBusBase {
public:
    SerialBusText() = default;
    ~SerialBusText() override = default;

    // Delete copy operations
    SerialBusText(const SerialBusText&) = delete;
    SerialBusText& operator=(const SerialBusText&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================
    
    /**
     * @brief Initialize with USB host (for compatibility)
     * @note For text protocol, prefer begin(Stream*) instead
     */
    bool begin(UsbHost* usbHost, int deviceIndex) override;
    
    /**
     * @brief Initialize with a Stream (Serial, mock, etc.)
     * @param stream Pointer to Stream object
     * @return true if successful
     */
    bool begin(Stream* stream);
    
    void end() override;
    void setDevice(int deviceIndex) override { _deviceIndex = deviceIndex; }

    // ========================================================================
    // Packet Transmission (converts to text)
    // ========================================================================
    
    int sendPacket(uint8_t type, const uint8_t* payload = nullptr, size_t len = 0) override;
    int sendKeepalive() override;
    
    /**
     * @brief Send raw text line
     * @param line Text to send (newline added automatically)
     * @return Bytes sent, or -1 on error
     */
    int sendLine(const char* line);
    
    /**
     * @brief Send formatted text command
     * @param format Printf-style format string
     * @return Bytes sent, or -1 on error
     */
    int sendFormatted(const char* format, ...);

    // ========================================================================
    // Packet Reception
    // ========================================================================
    
    void onPacketReceived(PacketRxCallback callback) override { _rxCallback = callback; }
    int process() override;

    // ========================================================================
    // Keepalive
    // ========================================================================
    
    void setKeepaliveInterval(unsigned long intervalMs) override { _keepaliveIntervalMs = intervalMs; }
    bool processKeepalive() override;

    // ========================================================================
    // Status
    // ========================================================================
    
    bool isConnected() const override { return _initialized && _stream != nullptr; }
    bool isInitialized() const override { return _initialized; }
    const SerialBusStats& stats() const override { return _stats; }
    void resetStats() override { _stats = SerialBusStats{}; }

    // ========================================================================
    // Text-Specific Methods
    // ========================================================================
    
    /**
     * @brief Set callback for raw line reception (before parsing)
     */
    using LineRxCallback = std::function<void(const char* line)>;
    void onLineReceived(LineRxCallback callback) { _lineCallback = callback; }
    
    /**
     * @brief Get the underlying stream
     */
    Stream* stream() const { return _stream; }

private:
    void processLine(const char* line);
    bool parseLine(const char* line, uint8_t* type, uint8_t* payload, size_t* payloadLen);
    
    // Encode payload to text format
    void encodePayloadToText(char* output, size_t maxLen, uint8_t type, 
                              const uint8_t* payload, size_t payloadLen);
    
    // Decode text parameters to payload
    bool decodeTextToPayload(const char* params, uint8_t type, 
                             uint8_t* payload, size_t* payloadLen);

    bool _initialized = false;
    Stream* _stream = nullptr;
    UsbHost* _usbHost = nullptr;
    int _deviceIndex = 0;

    char _rxBuffer[TEXT_RX_BUFFER_SIZE];
    size_t _rxIndex = 0;

    PacketRxCallback _rxCallback;
    LineRxCallback _lineCallback;
    SerialBusStats _stats;

    unsigned long _lastKeepaliveMs = 0;
    unsigned long _keepaliveIntervalMs = 0;
};

// ============================================================================
// Text Parsing Utilities
// ============================================================================

namespace TextParse {

/**
 * @brief Parse key=value pairs from a line
 * @param line Input line (after command name)
 * @param key Key to find
 * @param defaultValue Default if not found
 * @return Parsed integer value
 */
int getInt(const char* line, const char* key, int defaultValue = 0);

/**
 * @brief Parse key=value pairs from a line (unsigned)
 */
unsigned int getUInt(const char* line, const char* key, unsigned int defaultValue = 0);

/**
 * @brief Parse key=value pairs from a line (long)
 */
long getLong(const char* line, const char* key, long defaultValue = 0);

/**
 * @brief Parse key=value pairs from a line (unsigned long)
 */
unsigned long getULong(const char* line, const char* key, unsigned long defaultValue = 0);

/**
 * @brief Parse key=value pairs from a line (float)
 */
float getFloat(const char* line, const char* key, float defaultValue = 0.0f);

/**
 * @brief Parse key=value pairs from a line (bool: 0/1, true/false, on/off)
 */
bool getBool(const char* line, const char* key, bool defaultValue = false);

/**
 * @brief Parse key="value" pairs from a line (string)
 * @param line Input line
 * @param key Key to find
 * @param output Output buffer
 * @param maxLen Maximum output length
 * @return true if found
 */
bool getString(const char* line, const char* key, char* output, size_t maxLen);

/**
 * @brief Check if a key exists in the line
 */
bool hasKey(const char* line, const char* key);

/**
 * @brief Get the command name from a line
 * @param line Input line
 * @param output Output buffer for command name
 * @param maxLen Maximum output length
 * @return Pointer to rest of line after command
 */
const char* getCommand(const char* line, char* output, size_t maxLen);

} // namespace TextParse

#endif // SERIAL_BUS_TEXT_H
