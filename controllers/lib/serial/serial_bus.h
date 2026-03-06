/*
 * Serial Bus - COBS-Framed Protocol (Client Side)
 *
 * COBS-framed serial protocol for ScaleFX client controllers (HubFX).
 * Provides binary protocol encoding/decoding over USB CDC.
 *
 * Components:
 *   SerialBus - COBS-framed binary protocol over USB CDC
 *
 * This file is for CLIENT devices only (HubFX). Server devices (GunFX Pico,
 * LightFX Pico) should use CoreCommandServer from serial_bus_server.h instead.
 *
 * Protocol Format (before COBS encoding):
 *   [type:u8][len:u8][payload:0-64 bytes][crc8:u8]
 *
 * Framing:
 *   COBS encoded packet followed by 0x00 delimiter
 *
 * Universal Packet Types (0xF0-0xFF, defined in serial_core.h):
 *   INIT (0xF0)        - Initialize connection
 *   SHUTDOWN (0xF1)    - Graceful shutdown
 *   KEEPALIVE (0xF2)   - Connection heartbeat
 *   INIT_READY (0xF3)  - Server ready response
 *   STATUS (0xF4)      - Status payload
 *   ERROR (0xF5)       - Error notification
 *   ACK (0xF6)         - Command acknowledged
 *   NACK (0xF7)        - Command rejected
 *   REBOOT (0xF8)      - Restart device
 *   BOOTSEL (0xF9)     - Enter bootloader
 *   STATUS_REQ (0xFA)  - Request status
 *
 * CRC:
 *   CRC-8 polynomial 0x07 over type+len+payload
 *
 * Usage:
 *   UsbHost usbHost;
 *   usbHost.begin();
 *   GunFxClient gunfx;  // Extends SerialBus
 *   gunfx.begin(&usbHost, 0);
 */

#ifndef SERIAL_BUS_H
#define SERIAL_BUS_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <functional>

#include "serial_core.h"
#include "serial_usb_host.h"

// ============================================================================
// Serial Bus Constants & Types
// ============================================================================

constexpr size_t SERIAL_BUS_RX_BUFFER_SIZE = 256;

// ============================================================================
// SerialBus Class - Binary COBS Protocol
// ============================================================================

/**
 * @brief COBS-framed serial communication over USB CDC
 * 
 * This is the primary serial bus implementation using binary COBS protocol.
 */
class SerialBus {
public:
    SerialBus() = default;
    virtual ~SerialBus() = default;

    SerialBus(const SerialBus&) = delete;
    SerialBus& operator=(const SerialBus&) = delete;

    // Lifecycle
    bool begin(UsbHost* usbHost, int deviceIndex);
    void end();
    void setDevice(int deviceIndex);

    // Packet transmission
    int sendPacket(uint8_t type, const uint8_t* payload = nullptr, size_t len = 0, uint8_t tag = 0);
    int sendInit() { return sendPacket(CorePacket::INIT); }
    int sendShutdown() { return sendPacket(CorePacket::SHUTDOWN); }
    int sendReboot() { return sendPacket(CorePacket::REBOOT); }
    int sendBootsel() { return sendPacket(CorePacket::BOOTSEL); }
    int sendKeepalive();
    int sendStatusRequest() { return sendPacket(CorePacket::STATUS_REQ); }

    // Packet reception
    void onPacketReceived(PacketRxCallback callback) { _rxCallback = callback; }
    int process();

    // Keepalive management
    void setKeepaliveInterval(unsigned long intervalMs);
    bool processKeepalive();

    // Status
    bool isConnected() const;
    bool isInitialized() const { return _initialized; }
    int deviceIndex() const { return _deviceIndex; }
    const CoreStats& stats() const { return _stats; }
    void resetStats();

protected:
    // Timing state - accessible to subclasses for send time tracking
    unsigned long _lastSendMs = 0;         // Time of last packet sent (any type)
    unsigned long _keepaliveIntervalMs = 0;

private:
    void processFrame(const uint8_t* frame, size_t frameLen);

    bool _initialized = false;
    UsbHost* _usbHost = nullptr;
    int _deviceIndex = 0;

    uint8_t _rxBuffer[SERIAL_BUS_RX_BUFFER_SIZE];
    size_t _rxIndex = 0;

    PacketRxCallback _rxCallback;
    CoreStats _stats;
};

#endif // SERIAL_BUS_H
