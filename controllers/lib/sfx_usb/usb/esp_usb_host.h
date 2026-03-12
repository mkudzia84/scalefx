/*
 * EspUsbHost — HW USB-OTG Host Implementation (ESP32-S3)
 *
 * Concrete UsbHost implementation for ESP32-S3 using the native hardware
 * USB-OTG peripheral with the ESP-IDF USB Host Library and CDC-ACM class driver.
 *
 * Architecture:
 *   - USB daemon task: processes USB Host Library HCD events (Core 0)
 *   - CDC-ACM driver task: handles class-level events and data (Core 0)
 *   - RX data: CDC data callback → FreeRTOS StreamBuffer → cdcRead() from app
 *   - TX data: cdcWrite() → cdc_acm_host_data_tx_blocking()
 *
 * Key differences from PicoUsbHost:
 *   - Hardware USB-OTG peripheral (no PIO, no CPU clock constraint)
 *   - Event-driven via FreeRTOS tasks (not polled from Core 1 loop)
 *   - Fixed pins: GPIO19 (D-), GPIO20 (D+) on ESP32-S3
 *   - CDC-ACM class driver handles enumeration + line coding
 *
 * IMPORTANT: The ESP32-S3 USB-OTG peripheral is shared between device mode
 * (cdc_on_boot) and host mode. USB Host requires host mode, so cdc_on_boot
 * MUST be disabled. Serial debug uses UART0 via the USB-UART bridge chip.
 *
 * Dependencies (ESP-IDF managed component):
 *   espressif/usb_host_cdc_acm ^2.0.0  (see src/idf_component.yml)
 *
 * Singleton: accessed via UsbHost::instance() which returns EspUsbHost&
 * on ESP32-S3 builds. Do not instantiate directly.
 */

#ifndef SFX_ESP_USB_HOST_H
#define SFX_ESP_USB_HOST_H

#include "usb_host.h"  // Must come first — defines SFX_PLATFORM_ESP32

#if SFX_PLATFORM_ESP32

class EspUsbHost : public UsbHost {
public:
    EspUsbHost() = default;

    // --- Lifecycle ----------------------------------------------------------

    bool begin() override;
    bool begin(const UsbPortConfig* configs, int numPorts) override;
    void end() override;
    bool init() override;
    void process() override;

    // --- CDC Communication --------------------------------------------------

    bool cdcConnected() const override;
    int cdcAvailable(int devIndex) const override;
    int cdcRead(int devIndex, uint8_t* buffer, size_t maxLen) override;
    int cdcReadByte(int devIndex) override;
    int cdcWrite(int devIndex, const uint8_t* data, size_t len) override;
    void cdcFlush(int devIndex) override;

    // --- Diagnostics --------------------------------------------------------

    void printStatus() const override;
    const char* backendName() const override { return "HW USB-OTG"; }

    // --- Internal callback bridge -------------------------------------------
    // Called from static C callbacks in esp_usb_host.cpp. Not part of public API.

    void _handleNewDevice(void* usbDevHandle);
    void _handleCdcData(int slotIdx, const uint8_t* data, size_t len);
    void _handleCdcEvent(int slotIdx, int eventType);

private:
    /// RX buffer size per CDC device (bytes)
    static constexpr size_t CDC_RX_BUFFER_SIZE = 4096;

    /// TX timeout for blocking writes (ms)
    static constexpr uint32_t CDC_TX_TIMEOUT_MS = 100;

    /// Per-CDC-device slot tracking (opaque handles avoid ESP-IDF includes)
    struct CdcSlot {
        void* cdcHandle = nullptr;   // cdc_acm_dev_hdl_t (opaque)
        void* rxStream  = nullptr;   // StreamBufferHandle_t (opaque)
        uint8_t devAddr = 0;
        bool open = false;
    };
    CdcSlot _slots[USB_HOST_MAX_CDC_DEVICES] = {};

    void* _daemonTaskHandle = nullptr;   // TaskHandle_t (opaque)
    bool _driverInstalled = false;
    uint8_t _nextDevAddr = 1;            // Sequential device address counter

    int _findSlotByHandle(void* cdcHandle) const;
    int _allocateSlot();
};

#endif // SFX_PLATFORM_ESP32
#endif // SFX_ESP_USB_HOST_H
