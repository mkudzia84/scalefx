/*
 * Serial Command Handler - Chain of Responsibility Pattern
 * 
 * Unified command handler framework supporting both text and binary protocols
 * through a common interface. Each handler processes commands it recognizes
 * and returns a result indicating whether it handled the command.
 * 
 * Design Pattern: Chain of Responsibility
 * - Handlers are tried in sequence until one processes the command
 * - If no handler processes it, an error is returned to the sender
 * - Each handler is responsible for its own ACK/NACK responses
 * 
 * Class Hierarchy:
 *   SerialCommand           - Unified representation of text or binary command
 *   CommandHandleResult     - Shared result enum for all handlers
 *   ICommandHandler         - Single interface for all protocol handlers
 *   CommandRouter           - Routes commands through handler chain (any protocol)
 * 
 * Usage:
 *   CommandRouter router;
 *   router.begin(&Serial, ProtocolMode::Text, nackCallback);
 *   router.addHandler(&initHandler);
 *   router.addHandler(&protocolSlave);
 *   // In loop(): router.process();
 */

#ifndef SERIAL_COMMAND_HANDLER_H
#define SERIAL_COMMAND_HANDLER_H

#include <Arduino.h>
#include <functional>
#include "serial_error.h"
#include "serial_protocol.h"

// ============================================================================
//  PROTOCOL MODE
// ============================================================================

/**
 * @brief Protocol mode for command routing
 */
enum class ProtocolMode : uint8_t {
    Text,       // Newline-delimited text commands
    Binary      // COBS-encoded binary packets
};

// ============================================================================
//  SERIAL COMMAND (Unified Text/Binary Representation)
// ============================================================================

/**
 * @brief Unified command representation for both text and binary protocols
 * 
 * Allows a single handler interface to process either protocol type.
 * Use isText()/isBinary() to check type, then access appropriate fields.
 */
struct SerialCommand {
    ProtocolMode mode;
    
    // Text mode data
    const char* text;           // Null-terminated command line
    
    // Binary mode data
    uint8_t packetType;         // Packet type byte
    const uint8_t* payload;     // Payload data (may be nullptr)
    size_t payloadLen;          // Payload length
    
    // Convenience accessors
    bool isText() const { return mode == ProtocolMode::Text; }
    bool isBinary() const { return mode == ProtocolMode::Binary; }
    
    // Factory methods for cleaner construction
    static SerialCommand fromText(const char* line) {
        SerialCommand cmd;
        cmd.mode = ProtocolMode::Text;
        cmd.text = line;
        cmd.packetType = 0;
        cmd.payload = nullptr;
        cmd.payloadLen = 0;
        return cmd;
    }
    
    static SerialCommand fromBinary(uint8_t type, const uint8_t* data, size_t len) {
        SerialCommand cmd;
        cmd.mode = ProtocolMode::Binary;
        cmd.text = nullptr;
        cmd.packetType = type;
        cmd.payload = data;
        cmd.payloadLen = len;
        return cmd;
    }
};

// ============================================================================
//  COMMAND RESULT
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

// ============================================================================
//  UNIFIED COMMAND HANDLER INTERFACE
// ============================================================================

/**
 * @brief Unified interface for command handlers (text or binary)
 * 
 * Implementations should:
 * - Check cmd.isText() or cmd.isBinary() to determine protocol
 * - Return Handled if the command was recognized (send ACK/NACK as appropriate)
 * - Return NotMyCommand if the command should be passed to the next handler
 * - Return Error only for internal handler errors
 * 
 * Handlers can support one or both protocols by checking the command mode.
 */
class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;
    
    /**
     * @brief Try to process a command (text or binary)
     * @param cmd The command to process (check cmd.isText() or cmd.isBinary())
     * @return CommandHandleResult indicating how the command was handled
     */
    virtual CommandHandleResult tryProcess(const SerialCommand& cmd) = 0;
    
    /**
     * @brief Get the name of this handler (for debugging)
     */
    virtual const char* handlerName() const = 0;
};

// ============================================================================
//  LEGACY INTERFACES (for backward compatibility)
// ============================================================================

/**
 * @brief Legacy interface for text-only handlers
 * @deprecated Use ICommandHandler with SerialCommand instead
 */
class ITextCommandHandler : public ICommandHandler {
public:
    // Implement unified interface by delegating to text-specific method
    CommandHandleResult tryProcess(const SerialCommand& cmd) override {
        if (cmd.isText()) {
            return tryProcessCommand(cmd.text);
        }
        return CommandHandleResult::NotMyCommand;
    }
    
    /**
     * @brief Try to process a text command line
     * @param line The complete command line (command + arguments)
     * @return CommandHandleResult indicating how the command was handled
     */
    virtual CommandHandleResult tryProcessCommand(const char* line) = 0;
};

/**
 * @brief Legacy interface for binary-only handlers
 * @deprecated Use ICommandHandler with SerialCommand instead
 */
class IBinaryCommandHandler : public ICommandHandler {
public:
    // Implement unified interface by delegating to binary-specific method
    CommandHandleResult tryProcess(const SerialCommand& cmd) override {
        if (cmd.isBinary()) {
            return tryProcessPacket(cmd.packetType, cmd.payload, cmd.payloadLen);
        }
        return CommandHandleResult::NotMyCommand;
    }
    
    /**
     * @brief Try to process a binary packet
     * @param type Packet type byte
     * @param payload Pointer to payload data
     * @param len Length of payload
     * @return CommandHandleResult indicating how the packet was handled
     */
    virtual CommandHandleResult tryProcessPacket(uint8_t type, const uint8_t* payload, size_t len) = 0;
};

// ============================================================================
//  UNIFIED COMMAND ROUTER
// ============================================================================

/**
 * @brief Routes commands through a chain of handlers (text or binary mode)
 * 
 * Reads serial input, parses according to protocol mode, and passes commands
 * through handlers in sequence until one handles it or all decline.
 */
class CommandRouter {
public:
    static constexpr size_t MAX_HANDLERS = 4;
    static constexpr size_t RX_BUFFER_SIZE = 256;
    
    // Callback types for unhandled commands
    using TextNackCallback = std::function<void(uint8_t errorCode, const char* cmd)>;
    using BinaryNackCallback = std::function<void(uint8_t errorCode, uint8_t packetType)>;
    
    CommandRouter() = default;
    
    /**
     * @brief Initialize the router in text mode
     * @param serial Stream to read from
     * @param sendNackFunc Function to call when no handler processes a command
     */
    void begin(Stream* serial, TextNackCallback sendNackFunc = nullptr) {
        _serial = serial;
        _mode = ProtocolMode::Text;
        _textNack = sendNackFunc;
        _binaryNack = nullptr;
        _rxIndex = 0;
        _handlerCount = 0;
        _lastActivityMs = 0;
    }
    
    /**
     * @brief Initialize the router in specified mode
     * @param serial Stream to read from
     * @param mode Protocol mode (Text or Binary)
     * @param textNack Callback for text mode NACKs (optional)
     * @param binaryNack Callback for binary mode NACKs (optional)
     */
    void begin(Stream* serial, ProtocolMode mode,
               TextNackCallback textNack = nullptr,
               BinaryNackCallback binaryNack = nullptr) {
        _serial = serial;
        _mode = mode;
        _textNack = textNack;
        _binaryNack = binaryNack;
        _rxIndex = 0;
        _handlerCount = 0;
        _lastActivityMs = 0;
    }
    
    /**
     * @brief Switch protocol mode (e.g., after INIT negotiation)
     */
    void setMode(ProtocolMode mode) {
        _mode = mode;
        _rxIndex = 0;  // Clear buffer on mode switch
    }
    
    /**
     * @brief Get current protocol mode
     */
    ProtocolMode mode() const { return _mode; }
    
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
     * Reads available bytes, parses according to protocol mode, and routes
     * complete commands through the handler chain.
     * 
     * @return Number of commands processed
     */
    int process() {
        if (!_serial) return 0;
        
        if (_mode == ProtocolMode::Text) {
            return processText();
        } else {
            return processBinary();
        }
    }
    
    /**
     * @brief Get time of last activity
     */
    unsigned long lastActivityMs() const { return _lastActivityMs; }
    
private:
    int processText() {
        int commandsProcessed = 0;
        
        while (_serial->available()) {
            char c = _serial->read();
            _lastActivityMs = millis();
            
            if (c == '\n' || c == '\r') {
                if (_rxIndex > 0) {
                    _rxBuffer[_rxIndex] = '\0';
                    routeCommand(SerialCommand::fromText((const char*)_rxBuffer));
                    commandsProcessed++;
                    _rxIndex = 0;
                }
            } else if (_rxIndex < RX_BUFFER_SIZE - 1) {
                _rxBuffer[_rxIndex++] = c;
            }
        }
        
        return commandsProcessed;
    }
    
    int processBinary() {
        int packetsProcessed = 0;
        
        while (_serial->available()) {
            uint8_t b = _serial->read();
            _lastActivityMs = millis();
            
            if (b == SerialProtocol::FRAME_DELIMITER) {
                if (_rxIndex > 0) {
                    processFrame(_rxBuffer, _rxIndex);
                    packetsProcessed++;
                    _rxIndex = 0;
                }
            } else if (_rxIndex < SerialProtocol::COBS_BUFFER_SIZE) {
                _rxBuffer[_rxIndex++] = b;
            } else {
                _rxIndex = 0;  // Buffer overflow - reset
            }
        }
        
        return packetsProcessed;
    }
    
    void processFrame(const uint8_t* frame, size_t frameLen) {
        uint8_t decoded[SerialProtocol::MAX_PACKET_SIZE];
        size_t decodedLen = SerialProtocol::cobsDecode(frame, frameLen, decoded, sizeof(decoded));
        
        if (decodedLen < 2) return;
        
        uint8_t type;
        const uint8_t* payload;
        size_t payloadLen;
        
        if (!SerialProtocol::parsePacket(decoded, decodedLen, &type, &payload, &payloadLen)) {
            return;
        }
        
        routeCommand(SerialCommand::fromBinary(type, payload, payloadLen));
    }
    
    void routeCommand(const SerialCommand& cmd) {
        for (size_t i = 0; i < _handlerCount; i++) {
            CommandHandleResult result = _handlers[i]->tryProcess(cmd);
            
            if (result == CommandHandleResult::Handled) {
                return;
            }
        }
        
        // No handler processed the command
        if (cmd.isText() && _textNack) {
            _textNack(SerialError::INVALID_COMMAND, cmd.text);
        } else if (cmd.isBinary() && _binaryNack) {
            _binaryNack(SerialError::INVALID_COMMAND, cmd.packetType);
        }
    }
    
    Stream* _serial = nullptr;
    ProtocolMode _mode = ProtocolMode::Text;
    TextNackCallback _textNack;
    BinaryNackCallback _binaryNack;
    
    ICommandHandler* _handlers[MAX_HANDLERS] = {nullptr};
    size_t _handlerCount = 0;
    
    uint8_t _rxBuffer[RX_BUFFER_SIZE];
    size_t _rxIndex = 0;
    unsigned long _lastActivityMs = 0;
};

// ============================================================================
//  LEGACY ROUTER ALIASES (for backward compatibility)
// ============================================================================

using TextCommandRouter = CommandRouter;
using BinaryCommandRouter = CommandRouter;

#endif // SERIAL_COMMAND_HANDLER_H
