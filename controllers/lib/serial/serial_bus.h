/*
 * Serial Bus - USB Host Infrastructure (Master Side)
 *
 * USB Host communication for ScaleFX master controllers (HubFX).
 * Provides PIO-USB host stack integration and COBS-framed serial protocol.
 *
 * Components:
 *   UsbHost   - PIO-USB host manager for CDC devices
 *   SerialBus - COBS-framed binary protocol (implements ISerialCore)
 *
 * This file is for MASTER devices only (HubFX). Slave devices (GunFX Pico,
 * LightFX Pico) should use CoreCommandHandler from serial_core.h instead.
 *
 * Protocol Format (before COBS encoding):
 *   [type:u8][len:u8][payload:0-64 bytes][crc8:u8]
 *
 * Framing:
 *   COBS encoded packet followed by 0x00 delimiter
 *
 * Universal Packet Types (0xF0-0xFF, defined in serial_protocol.h):
 *   INIT (0xF0)        - Initialize connection
 *   SHUTDOWN (0xF1)    - Graceful shutdown
 *   KEEPALIVE (0xF2)   - Connection heartbeat
 *   INIT_READY (0xF3)  - Slave ready response
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
 *   GunFxMaster gunfx;  // Extends SerialBus
 *   gunfx.begin(&usbHost, 0);
 */

#ifndef SERIAL_BUS_H
#define SERIAL_BUS_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <functional>

#include "serial_core.h"

// ============================================================================
// USB Host Constants & Types
// ============================================================================

constexpr int USB_HOST_MAX_PORTS = 1;
constexpr int USB_HOST_MAX_CDC_DEVICES = 4;
constexpr uint8_t USB_HOST_PORT_0_DP_DEFAULT = 2;

enum class UsbDeviceState {
    Disconnected = 0,
    Connected,
    Mounted,
    Ready
};

/**
 * @brief USB Host port configuration
 */
struct UsbPortConfig {
    bool enabled = false;
    uint8_t dp_pin = USB_HOST_PORT_0_DP_DEFAULT;  // D+ pin (D- is dp_pin + 1)
    char name[32] = "";
    
    UsbPortConfig() = default;
    UsbPortConfig(bool en, uint8_t pin, const char* n) : enabled(en), dp_pin(pin) {
        strncpy(name, n, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
};

/**
 * @brief CDC device information
 */
struct CdcDeviceInfo {
    bool connected = false;
    uint8_t dev_addr = 0;
    uint8_t itf_num = 0;
    uint16_t vid = 0;
    uint16_t pid = 0;
    UsbDeviceState state = UsbDeviceState::Disconnected;
    uint8_t port_id = 0;
};

/**
 * @brief USB Host statistics
 */
struct UsbHostStats {
    uint32_t devices_mounted = 0;
    uint32_t devices_unmounted = 0;
    uint32_t bytes_sent = 0;
    uint32_t bytes_received = 0;
};

// Callback types
using UsbMountCallback = std::function<void(uint8_t devAddr, uint16_t vid, uint16_t pid)>;
using UsbUnmountCallback = std::function<void(uint8_t devAddr)>;
using UsbCdcRxCallback = std::function<void(uint8_t devAddr, const uint8_t* data, size_t len)>;

// ============================================================================
// UsbHost Class
// ============================================================================

/**
 * @brief USB Host manager for PIO-USB CDC devices
 */
class UsbHost {
public:
    UsbHost() = default;
    ~UsbHost();

    UsbHost(const UsbHost&) = delete;
    UsbHost& operator=(const UsbHost&) = delete;

    bool begin();
    bool begin(const UsbPortConfig* configs, int numPorts);
    void end();
    
    // Must be called from Core 1 (PIO-USB requirement)
    bool init();            // Initialize TinyUSB host stack
    void process();         // Process USB host tasks

    // CDC Communication
    bool cdcConnected() const;
    int cdcDeviceCount() const { return _cdcDeviceCount; }
    int cdcAvailable(int devIndex) const;
    int cdcRead(int devIndex, uint8_t* buffer, size_t maxLen);
    int cdcReadByte(int devIndex);
    int cdcWrite(int devIndex, const uint8_t* data, size_t len);
    int cdcPrint(int devIndex, const char* str);
    int cdcPrintln(int devIndex, const char* str);
    void cdcFlush(int devIndex);

    // Callbacks
    void onMount(UsbMountCallback callback) { _mountCallback = callback; }
    void onUnmount(UsbUnmountCallback callback) { _unmountCallback = callback; }
    void onCdcReceive(UsbCdcRxCallback callback) { _cdcRxCallback = callback; }

    // Device info
    const CdcDeviceInfo* getCdcDevice(int devIndex) const;
    void printStatus() const;

    // Status
    bool isReady() const { return _initialized && _taskRunning; }
    bool isInitialized() const { return _initialized; }
    bool isTaskRunning() const { return _taskRunning; }
    const UsbHostStats& stats() const { return _stats; }

    // For internal use by TinyUSB callbacks
    void _onDeviceMount(uint8_t devAddr);
    void _onDeviceUnmount(uint8_t devAddr);
    void _onCdcMount(uint8_t idx);
    void _onCdcUnmount(uint8_t idx);
    void _onCdcRx(uint8_t idx);

private:
    int findDeviceByAddr(uint8_t devAddr) const;
    int addCdcDevice(uint8_t devAddr, uint8_t itfNum);
    void removeCdcDevice(uint8_t devAddr);

    bool _initialized = false;
    bool _taskRunning = false;

    UsbPortConfig _ports[USB_HOST_MAX_PORTS];
    CdcDeviceInfo _cdcDevices[USB_HOST_MAX_CDC_DEVICES];
    int _cdcDeviceCount = 0;

    UsbHostStats _stats;

    UsbMountCallback _mountCallback;
    UsbUnmountCallback _unmountCallback;
    UsbCdcRxCallback _cdcRxCallback;
};

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
class SerialBus : public ISerialCore {
public:
    SerialBus() = default;
    ~SerialBus() = default;

    SerialBus(const SerialBus&) = delete;
    SerialBus& operator=(const SerialBus&) = delete;

    bool begin(UsbHost* usbHost, int deviceIndex) override;
    void end() override;
    void setDevice(int deviceIndex) override;

    // Packet transmission
    int sendPacket(uint8_t type, const uint8_t* payload = nullptr, size_t len = 0) override;
    
    // Convenient overrides for core commands
    int sendInit() { return sendPacket(CorePacket::INIT); }
    int sendShutdown() { return sendPacket(CorePacket::SHUTDOWN); }
    int sendReboot() { return sendPacket(CorePacket::REBOOT); }
    int sendBootsel() { return sendPacket(CorePacket::BOOTSEL); }
    int sendKeepalive() override;

    // Packet reception
    void onPacketReceived(PacketRxCallback callback) override { _rxCallback = callback; }
    int process() override;

    // Keepalive management
    void setKeepaliveInterval(unsigned long intervalMs) override;
    bool processKeepalive() override;

    // Status
    bool isConnected() const override;
    bool isInitialized() const override { return _initialized; }
    int deviceIndex() const { return _deviceIndex; }
    const CoreStats& stats() const override { return _stats; }
    void resetStats() override;

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
