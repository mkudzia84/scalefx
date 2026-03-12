/*
 * Serial USB Host - PIO-USB Host Infrastructure (Client Side)
 *
 * USB Host communication for ScaleFX client controllers (HubFX).
 * Provides PIO-USB host stack integration for CDC device communication.
 *
 * Components:
 *   UsbHost   - PIO-USB host manager for CDC devices
 *
 * This file is for CLIENT devices only (HubFX). Server devices (GunFX Pico,
 * LightFX Pico) should use CoreCommandServer from serial_core.h instead.
 */

#ifndef SERIAL_USB_HOST_H
#define SERIAL_USB_HOST_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <functional>

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

#endif // SERIAL_USB_HOST_H
