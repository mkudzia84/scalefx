/*
 * USB Host — Types, Shared State, and Platform-Selected Alias
 *
 * Common USB Host CDC types and UsbHostState for ScaleFX client
 * controllers (HubFX). Each platform provides a standalone concrete
 * USB Host class with compile-time dispatch via `using` alias:
 *
 *   - RP2040/RP2350 (Pico): PicoUsbHost — PIO-USB software USB
 *   - ESP32-S3:             EspUsbHost  — native HW USB-OTG
 *
 * Consumers use the `UsbHost` alias (auto-selected at the bottom of
 * this header). No virtual dispatch — each platform class implements
 * the same interface as a standalone singleton with UsbHostState
 * composition for shared device tracking.
 *
 * Design rationale (pure arch implementations):
 *   - Unlike StorageServer (large shared protocol layer, thin hooks),
 *     UsbHost has MINIMAL shared logic and LARGE platform-specific
 *     implementations (PIO-USB vs HW USB-OTG are fundamentally different).
 *   - A policy template would just forward every call — no value added.
 *   - Pure arch classes with a `using` alias give zero-cost dispatch
 *     while keeping each implementation self-contained and readable.
 *   - Shared device-tracking logic lives in UsbHostState (composition).
 *
 * NOTE: Client-only (HubFX). Servers define SCALEFX_SERVER to exclude.
 *
 * Usage:
 *   #include <usb/sfx_usb_host.h>
 *   UsbHost& usb = UsbHost::instance();
 *   usb.begin();
 *   usb.onMount([](uint8_t addr, uint16_t vid, uint16_t pid) { ... });
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
// UsbHostState — Shared state composed into platform USB Host classes
//
// Contains device-tracking arrays, statistics, and callback slots.
// Each platform-specific USB Host class (PicoUsbHost, EspUsbHost)
// composes this as a member (_state). Helper methods provide common
// device management logic shared across platforms.
// ============================================================================

struct UsbHostState {
    bool initialized = false;
    bool taskRunning = false;

    UsbPortConfig ports[USB_HOST_MAX_PORTS];
    CdcDeviceInfo cdcDevices[USB_HOST_MAX_CDC_DEVICES];
    int cdcDeviceCount = 0;

    UsbHostStats stats;

    UsbMountCallback mountCallback;
    UsbUnmountCallback unmountCallback;
    UsbCdcRxCallback cdcRxCallback;

    // --- Shared device-tracking helpers ---

    /// Get info for a tracked CDC device (nullptr if invalid index)
    const CdcDeviceInfo* getCdcDevice(int devIndex) const;

    /// Find device array index by USB address (-1 if not found)
    int findDeviceByAddr(uint8_t devAddr) const;

    /// Add a CDC device to the tracking array (returns index, -1 if full)
    int addCdcDevice(uint8_t devAddr, uint8_t itfNum, uint16_t vid, uint16_t pid);

    /// Remove a CDC device by USB address
    void removeCdcDevice(uint8_t devAddr);
};

// ============================================================================
// Platform-specific USB Host (auto-selected by build target)
// ============================================================================

#if SFX_PLATFORM_ESP32
#include "esp32/esp_usb_host.h"
using UsbHost = EspUsbHost;
#elif SFX_PLATFORM_PICO
#include "pico/pico_usb_host.h"
using UsbHost = PicoUsbHost;
#endif

#endif // SFX_USB_HOST_H
