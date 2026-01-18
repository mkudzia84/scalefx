/*
 * Serial Command Handler - Binary Protocol Command Routing
 *
 * Command handler framework for binary COBS protocol. Each handler processes
 * commands it recognizes and returns a result indicating whether it handled
 * the command.
 *
 * Design Pattern: Chain of Responsibility
 *   - Handlers are tried in sequence until one processes the command
 *   - If no handler processes it, a NACK is returned to the sender
 *   - Each handler is responsible for its own ACK/NACK responses
 *
 * Key Types:
 *   CommandHandleResult - Result enum: Handled, NotMyCommand, Error
 *   ICommandHandler     - Interface for binary command handlers
 *   CommandRouter       - Routes packets through handler chain
 *
 * Usage (Slave Side):
 *   CommandRouter router;
 *   router.begin(&Serial, nackCallback);
 *   router.addHandler(&coreHandler);     // System commands (via CoreCommandHandler)
 *   router.addHandler(&protocolHandler); // Protocol-specific commands
 *   // In loop(): router.process();
 *
 * Binary Packets:
 *   Routed by packet type byte (0x01-0xFF)
 *   COBS-encoded with CRC-8 verification
 */

#ifndef SERIAL_COMMAND_HANDLER_H
#define SERIAL_COMMAND_HANDLER_H

#include <Arduino.h>
#include <functional>
#include "serial_error.h"
#include "serial_core.h"

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
//  COMMAND HANDLER INTERFACE
// ============================================================================

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
//  COMMAND ROUTER
// ============================================================================

namespace Serial {

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
        
        if (decodedLen < 3) return;  // Minimum: type + len + crc
        
        uint8_t type;
        const uint8_t* payload;
        size_t payloadLen;
        
        if (!CoreProtocol::parsePacket(decoded, decodedLen, &type, &payload, &payloadLen)) {
            return;  // CRC error or malformed packet
        }
        
        routePacket(type, payload, payloadLen);
    }
    
    void routePacket(uint8_t type, const uint8_t* payload, size_t len) {
        for (size_t i = 0; i < _handlerCount; i++) {
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

} // namespace Serial

// ============================================================================
//  TYPE ALIASES
// ============================================================================

using CommandRouter = Serial::CommandRouter;

#endif // SERIAL_COMMAND_HANDLER_H
