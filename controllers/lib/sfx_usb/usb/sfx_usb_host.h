/*
 * USB Host — Abstract Interface + Platform Factory Singleton
 *
 * Defines the common USB Host CDC interface for ScaleFX client controllers
 * (HubFX). Platform-specific implementations:
 *
 *   - RP2040/RP2350 (Pico): PicoUsbHost — PIO-USB software USB
 *   - ESP32-S3:             EspUsbHost  — native HW USB-OTG
 *
 * Singleton pattern: UsbHost::instance() returns the platform-appropriate
 * implementation at compile time. All consumers use the abstract interface.
 *
 * All diagnostic output uses DiagLog (SFX_LOG_* macros).
 *
 * NOTE: Client-only (HubFX). Servers define SCALEFX_SERVER to exclude.
 *
 * Usage:
 *   #include <usb/sfx_usb_host.h>
 *   UsbHost& usb = UsbHost::instance();
 *   usb.begin();
 *   usb.onMount([](uint8_t addr, uint16_t vid, uint16_t pid) { ... });
 *   // Core 1 (Pico) or USB task (ESP32): usb.init(); usb.process();
 */

#ifndef SFX_USB_HOST_H
#define SFX_USB_HOST_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <functional>
#include "platform/sfx_platform.h"

// ============================================================================
// USB Host Constants & Types
// ============================================================================

/// Maximum USB host ports (PIO-USB: 1, ESP32-S3 OTG: 1)
constexpr int USB_HOST_MAX_PORTS = 1;

/// Maximum CDC devices tracked simultaneously
constexpr int USB_HOST_MAX_CDC_DEVICES = 4;

/// Default D+ pin for USB port 0 (Pico PIO-USB; ignored on ESP32-S3)
constexpr uint8_t USB_HOST_PORT_0_DP_DEFAULT = 2;

/// USB device connection state
enum class UsbDeviceState : uint8_t {
    Disconnected = 0,
    Connected,
    Mounted,
    Ready
};

/**
 * @brief USB Host port configuration
 *
 * On Pico, dp_pin specifies the D+ GPIO pin for PIO-USB (D- = dp_pin + 1).
 * On ESP32-S3, dp_pin is ignored (native USB-OTG uses fixed GPIO19/GPIO20).
 */
struct UsbPortConfig {
    bool enabled = false;
    uint8_t dp_pin = USB_HOST_PORT_0_DP_DEFAULT;
    char name[32] = "";

    UsbPortConfig() = default;
    UsbPortConfig(bool en, uint8_t pin, const char* n) : enabled(en), dp_pin(pin) {
        strncpy(name, n, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
};

/**
 * @brief CDC device information (per-device status)
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
 * @brief USB Host traffic statistics
 */
struct UsbHostStats {
    uint32_t devices_mounted = 0;
    uint32_t devices_unmounted = 0;
    uint32_t bytes_sent = 0;
    uint32_t bytes_received = 0;
};

// ============================================================================
// Callback Types
// ============================================================================

/// Called when a USB device is mounted (enumerated)
using UsbMountCallback = std::function<void(uint8_t devAddr, uint16_t vid, uint16_t pid)>;

/// Called when a USB device is unmounted (disconnected)
using UsbUnmountCallback = std::function<void(uint8_t devAddr)>;

/// Called when CDC data is received from a device
using UsbCdcRxCallback = std::function<void(uint8_t devAddr, const uint8_t* data, size_t len)>;

// ============================================================================
// UsbHost — Abstract USB Host Interface + Platform Factory
// ============================================================================

/**
 * @brief Abstract USB Host interface for CDC device communication
 *
 * One USB host port exists per board. The concrete implementation is
 * selected at compile time via UsbHost::instance():
 *
 *   - Pico (RP2040/RP2350): PicoUsbHost — PIO-USB + TinyUSB host stack
 *   - ESP32-S3: EspUsbHost — Native USB-OTG (ESP-IDF USB host library)
 *
 * Lifecycle:
 *   1. begin()   — configure port (call from Core 0 / setup)
 *   2. init()    — initialize USB stack (Core 1 on Pico, USB task on ESP32)
 *   3. process() — poll USB tasks + handle events (Core 1 loop / USB task)
 *
 * CDC operations (cdcRead, cdcWrite, etc.) are called from Core 0 via
 * SerialBus, which handles COBS framing and packet routing.
 *
 * Diagnostics: all log output goes through DiagLog (SFX_LOG_* macros).
 */
class UsbHost {
public:
    // --- Platform factory singleton -----------------------------------------

    /**
     * @brief Get the platform-specific USB Host singleton
     *
     * Returns PicoUsbHost on RP2040/RP2350, EspUsbHost on ESP32-S3.
     * Thread-safe (C++11 static local initialization).
     */
    static UsbHost& instance();

    virtual ~UsbHost() = default;

    // Delete copy/move
    UsbHost(const UsbHost&) = delete;
    UsbHost& operator=(const UsbHost&) = delete;
    UsbHost(UsbHost&&) = delete;
    UsbHost& operator=(UsbHost&&) = delete;

    // ========================================================================
    // Lifecycle (pure virtual — implemented per platform)
    // ========================================================================

    /// Configure USB host with default port settings
    virtual bool begin() = 0;

    /// Configure USB host with explicit port settings
    virtual bool begin(const UsbPortConfig* configs, int numPorts) = 0;

    /// Deinitialize USB host
    virtual void end() = 0;

    /// Initialize USB stack (call from Core 1 on Pico, or USB task on ESP32)
    virtual bool init() = 0;

    /// Process USB host tasks and events (call from Core 1 loop)
    virtual void process() = 0;

    // ========================================================================
    // CDC Communication (pure virtual)
    // ========================================================================

    /// Check if any CDC device is connected and ready
    virtual bool cdcConnected() const = 0;

    /// Number of currently tracked CDC devices
    int cdcDeviceCount() const { return _cdcDeviceCount; }

    /// Bytes available to read from a CDC device
    virtual int cdcAvailable(int devIndex) const = 0;

    /// Read data from a CDC device (returns bytes read, -1 on error)
    virtual int cdcRead(int devIndex, uint8_t* buffer, size_t maxLen) = 0;

    /// Read a single byte from a CDC device (returns byte, -1 on error)
    virtual int cdcReadByte(int devIndex) = 0;

    /// Write data to a CDC device (returns bytes written, -1 on error)
    virtual int cdcWrite(int devIndex, const uint8_t* data, size_t len) = 0;

    /// Flush pending write data for a CDC device
    virtual void cdcFlush(int devIndex) = 0;

    // ========================================================================
    // Callbacks (common implementation)
    // ========================================================================

    void onMount(UsbMountCallback callback) { _mountCallback = callback; }
    void onUnmount(UsbUnmountCallback callback) { _unmountCallback = callback; }
    void onCdcReceive(UsbCdcRxCallback callback) { _cdcRxCallback = callback; }

    // ========================================================================
    // Device Info & Diagnostics
    // ========================================================================

    /// Get info for a tracked CDC device (nullptr if invalid index)
    const CdcDeviceInfo* getCdcDevice(int devIndex) const;

    /// Log USB host status via DiagLog
    virtual void printStatus() const = 0;

    // ========================================================================
    // Status (common implementation using shared state)
    // ========================================================================

    bool isReady() const { return _initialized && _taskRunning; }
    bool isInitialized() const { return _initialized; }
    bool isTaskRunning() const { return _taskRunning; }
    const UsbHostStats& stats() const { return _stats; }

    /// Get platform backend name ("PIO-USB" or "HW USB-OTG")
    virtual const char* backendName() const = 0;

protected:
    UsbHost() = default;  // Protected — only subclasses can construct

    // --- Shared helpers (accessible to implementations) ---------------------

    int findDeviceByAddr(uint8_t devAddr) const;
    int addCdcDevice(uint8_t devAddr, uint8_t itfNum, uint16_t vid, uint16_t pid);
    void removeCdcDevice(uint8_t devAddr);

    // --- Shared state -------------------------------------------------------

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

#endif // SFX_USB_HOST_H
