/*
 * Serial Bus Base - Abstract Interface
 * 
 * Abstract base class defining the interface for serial bus implementations.
 * Concrete implementations:
 *   - SerialBus (binary COBS/CRC protocol)
 *   - SerialBusText (human-readable text protocol for testing)
 */

#ifndef SERIAL_BUS_BASE_H
#define SERIAL_BUS_BASE_H

#include <Arduino.h>
#include <stdint.h>
#include <functional>
#include "serial_protocol.h"

// Forward declaration
class UsbHost;

// ============================================================================
// Statistics
// ============================================================================

struct SerialBusStats {
    uint32_t packets_sent = 0;
    uint32_t packets_received = 0;
    uint32_t crc_errors = 0;
    uint32_t framing_errors = 0;
};

// ============================================================================
// Callback Types
// ============================================================================

using PacketRxCallback = std::function<void(uint8_t type, const uint8_t* payload, size_t len)>;

// ============================================================================
// SerialBusBase - Abstract Interface
// ============================================================================

/**
 * @brief Abstract base class for serial bus communication
 * 
 * Defines the interface for packet-based serial communication.
 * Implementations can use different wire formats (binary vs text).
 */
class SerialBusBase {
public:
    virtual ~SerialBusBase() = default;

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
    virtual int sendInit() { return sendPacket(SerialProtocol::SFX_PKT_INIT); }
    
    /**
     * @brief Send SHUTDOWN command
     * @note Slave will ACK since device stays running
     */
    virtual int sendShutdown() { return sendPacket(SerialProtocol::SFX_PKT_SHUTDOWN); }
    
    /**
     * @brief Send REBOOT command (fire-and-forget)
     * @note No ACK expected - device reboots immediately
     */
    virtual int sendReboot() { return sendPacket(SerialProtocol::SFX_PKT_REBOOT); }
    
    /**
     * @brief Send BOOTSEL command (fire-and-forget)
     * @note No ACK expected - device enters bootloader immediately
     */
    virtual int sendBootsel() { return sendPacket(SerialProtocol::SFX_PKT_BOOTSEL); }
    
    /**
     * @brief Send KEEPALIVE packet
     */
    virtual int sendKeepalive() = 0;

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
    virtual const SerialBusStats& stats() const = 0;
    
    /**
     * @brief Reset statistics
     */
    virtual void resetStats() = 0;
};

#endif // SERIAL_BUS_BASE_H
