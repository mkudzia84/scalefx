/*
 * Serial Common - Main Header
 * 
 * Object-oriented serial communication library for ScaleFX controllers.
 * 
 * Provides:
 *   - Protocol utilities (COBS framing, CRC-8, packet types) in SerialProtocol namespace
 *   - UsbHost class for USB HOST functionality (PIO-USB for CDC devices)
 *   - SerialBus class for packet send/receive abstraction
 * 
 * Used by:
 *   - HubFX Pico (master controller)
 *   - GunFX Pico (slave controller)
 *   - Future modules
 * 
 * Packet Format (before COBS encoding):
 *   [type:u8][len:u8][payload:len bytes][crc:u8]
 * 
 * Framing:
 *   COBS encoded, terminated with 0x00 delimiter
 * 
 * CRC:
 *   CRC-8 polynomial 0x07 over type+len+payload
 */

#ifndef SERIAL_COMMON_H
#define SERIAL_COMMON_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <functional>

// ============================================================================
// Protocol Constants & Utilities
// ============================================================================

namespace SerialProtocol {

// Buffer sizes
constexpr size_t MAX_PAYLOAD_SIZE = 64;
constexpr size_t MAX_PACKET_SIZE = 2 + MAX_PAYLOAD_SIZE + 1;
constexpr size_t COBS_BUFFER_SIZE = MAX_PACKET_SIZE + MAX_PACKET_SIZE / 254 + 2;
constexpr uint8_t FRAME_DELIMITER = 0x00;

// Universal Packet Types (0xF0-0xFF)
constexpr uint8_t SFX_PKT_INIT        = 0xF0;
constexpr uint8_t SFX_PKT_SHUTDOWN    = 0xF1;
constexpr uint8_t SFX_PKT_KEEPALIVE   = 0xF2;
constexpr uint8_t SFX_PKT_INIT_READY  = 0xF3;
constexpr uint8_t SFX_PKT_STATUS      = 0xF4;
constexpr uint8_t SFX_PKT_ERROR       = 0xF5;
constexpr uint8_t SFX_PKT_ACK         = 0xF6;
constexpr uint8_t SFX_PKT_NACK        = 0xF7;

// GunFX-Specific Packet Types (0x01-0x2F)
constexpr uint8_t GUNFX_PKT_TRIGGER_ON      = 0x01;
constexpr uint8_t GUNFX_PKT_TRIGGER_OFF     = 0x02;
constexpr uint8_t GUNFX_PKT_SRV_SET         = 0x10;
constexpr uint8_t GUNFX_PKT_SRV_SETTINGS    = 0x11;
constexpr uint8_t GUNFX_PKT_SRV_RECOIL_JERK = 0x12;
constexpr uint8_t GUNFX_PKT_SMOKE_HEAT      = 0x20;

// Protocol functions
uint8_t crc8(const uint8_t* data, size_t len);
size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output);
size_t cobsDecode(const uint8_t* input, size_t length, uint8_t* output, size_t maxOutput);
size_t buildPacket(uint8_t* output, uint8_t type, const uint8_t* payload, size_t payloadLen);
size_t encodePacket(uint8_t* output, uint8_t type, const uint8_t* payload, size_t payloadLen);
bool parsePacket(const uint8_t* input, size_t length, uint8_t* type, 
                 const uint8_t** payload, size_t* payloadLen);

// Payload encoding helpers
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

} // namespace SerialProtocol

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

struct SerialBusStats {
    uint32_t packets_sent = 0;
    uint32_t packets_received = 0;
    uint32_t crc_errors = 0;
    uint32_t framing_errors = 0;
};

using PacketRxCallback = std::function<void(uint8_t type, const uint8_t* payload, size_t len)>;

// ============================================================================
// SerialBus Class
// ============================================================================

/**
 * @brief COBS-framed serial communication over USB CDC
 */
class SerialBus {
public:
    SerialBus() = default;
    ~SerialBus() = default;

    SerialBus(const SerialBus&) = delete;
    SerialBus& operator=(const SerialBus&) = delete;

    bool begin(UsbHost* usbHost, int deviceIndex);
    void end();
    void setDevice(int deviceIndex);

    // Packet transmission
    int sendPacket(uint8_t type, const uint8_t* payload = nullptr, size_t len = 0);
    int sendInit() { return sendPacket(SerialProtocol::SFX_PKT_INIT); }
    int sendShutdown() { return sendPacket(SerialProtocol::SFX_PKT_SHUTDOWN); }
    int sendKeepalive();

    // Packet reception
    void onPacketReceived(PacketRxCallback callback) { _rxCallback = callback; }
    int process();

    // Keepalive management
    void setKeepaliveInterval(unsigned long intervalMs);
    bool processKeepalive();

    // Status
    bool isConnected() const;
    bool isInitialized() const { return _initialized; }
    const SerialBusStats& stats() const { return _stats; }
    void resetStats();

private:
    void processFrame(const uint8_t* frame, size_t frameLen);

    bool _initialized = false;
    UsbHost* _usbHost = nullptr;
    int _deviceIndex = 0;

    uint8_t _rxBuffer[SERIAL_BUS_RX_BUFFER_SIZE];
    size_t _rxIndex = 0;

    PacketRxCallback _rxCallback;
    SerialBusStats _stats;

    unsigned long _lastKeepaliveMs = 0;
    unsigned long _keepaliveIntervalMs = 0;
};

#endif // SERIAL_COMMON_H
