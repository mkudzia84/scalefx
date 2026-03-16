/*
 * EspUsbHost — HW USB-OTG Host Implementation (ESP32-S3)
 *
 * Uses ESP-IDF USB Host Library + CDC-ACM class driver for native USB Host.
 * The ESP32-S3's USB-OTG peripheral operates in Host mode (cannot co-exist
 * with USB CDC serial — UART is used for debug serial instead).
 *
 * Architecture:
 *   - Daemon task: runs usb_host_lib_handle_events() in a loop (HCD events)
 *   - CDC-ACM driver: separate FreeRTOS task for class-level events
 *   - RX data: CDC data callback → StreamBuffer → cdcRead() from app task
 *   - TX data: cdcWrite() → cdc_acm_host_data_tx_blocking()
 *
 * The CDC-ACM class driver handles:
 *   - USB device enumeration and CDC interface claiming
 *   - Line coding configuration (1Mbps 8N1 for ScaleFX protocol)
 *   - Bulk endpoint management for IN/OUT data transfers
 *
 * Dependencies:
 *   espressif/usb_host_cdc_acm managed component (see src/idf_component.yml)
 */

#include <usb/sfx_usb_host.h>

#ifndef SCALEFX_SERVER
#if SFX_PLATFORM_ESP32

#include "platform/diag_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>

// CDC-ACM Class Driver (managed component: espressif/usb_host_cdc_acm)
// If not available, USB Host compiles but init() returns false with a clear error.
#if __has_include("usb/cdc_acm_host.h")
    #include "usb/cdc_acm_host.h"
    #define ESP_USB_HAS_CDC_ACM 1
#else
    #define ESP_USB_HAS_CDC_ACM 0
    #warning "usb/cdc_acm_host.h not found — USB Host CDC-ACM disabled. " \
             "Install espressif/usb_host_cdc_acm managed component."
#endif

#include "esp_log.h"

static const char* TAG = "EspUsbHost";

// ============================================================================
// USB Host Daemon Task
// ============================================================================

#if ESP_USB_HAS_CDC_ACM

/**
 * Required by ESP-IDF USB Host Library — processes low-level HCD events
 * (device connect/disconnect, transfer completions, port events).
 * Must run continuously in its own FreeRTOS task.
 */
static void usbHostDaemonTask(void* arg) {
    SFX_LOG_INFO("[UsbHost] Daemon task started");
    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            SFX_LOG_DEBUG("[UsbHost] All clients deregistered");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            SFX_LOG_DEBUG("[UsbHost] All devices freed");
        }
    }
}

// ============================================================================
// CDC-ACM Static Callbacks → EspUsbHost Singleton Bridge
// ============================================================================

static void cdcNewDeviceCb(usb_device_handle_t usb_dev) {
    EspUsbHost::instance()._handleNewDevice((void*)usb_dev);
}

static bool cdcDataCb(const uint8_t* data, size_t data_len, void* user_arg) {
    int slotIdx = (int)(uintptr_t)user_arg;
    EspUsbHost::instance()._handleCdcData(slotIdx, data, data_len);
    return true;
}

static void cdcEventCb(const cdc_acm_host_dev_event_data_t* event, void* user_ctx) {
    int slotIdx = (int)(uintptr_t)user_ctx;
    EspUsbHost::instance()._handleCdcEvent(slotIdx, (int)event->type);
}

#endif // ESP_USB_HAS_CDC_ACM

// ============================================================================
// Lifecycle
// ============================================================================

bool EspUsbHost::begin() {
    UsbPortConfig defaultConfig;
    defaultConfig.enabled = true;
    defaultConfig.dp_pin = 0;  // Ignored on ESP32-S3 (fixed GPIO19/GPIO20)
    strncpy(defaultConfig.name, "USB0", sizeof(defaultConfig.name));
    return begin(&defaultConfig, 1);
}

bool EspUsbHost::begin(const UsbPortConfig* configs, int numPorts) {
    if (_state.initialized) return true;

    SFX_LOG_INFO("[UsbHost] Initializing HW USB-OTG backend (ESP32-S3)...");

    // Store port configurations (dp_pin ignored — fixed GPIO19 D-, GPIO20 D+)
    for (int i = 0; i < USB_HOST_MAX_PORTS && i < numPorts; i++) {
        _state.ports[i] = configs[i];
    }

    _state.initialized = true;
    SFX_LOG_INFO("[UsbHost] HW USB-OTG configured (Fixed pins: GPIO19 D-, GPIO20 D+)");
    return true;
}

void EspUsbHost::end() {
    if (!_state.initialized) return;

    SFX_LOG_INFO("[UsbHost] Shutting down HW USB-OTG...");

#if ESP_USB_HAS_CDC_ACM
    // Close all open CDC devices
    for (int i = 0; i < USB_HOST_MAX_CDC_DEVICES; i++) {
        if (_slots[i].open && _slots[i].cdcHandle) {
            cdc_acm_host_close((cdc_acm_dev_hdl_t)_slots[i].cdcHandle);
        }
        if (_slots[i].rxStream) {
            vStreamBufferDelete((StreamBufferHandle_t)_slots[i].rxStream);
        }
        _slots[i] = {};
    }

    // Uninstall CDC-ACM class driver (deregisters internal USB Host client)
    if (_driverInstalled) {
        cdc_acm_host_uninstall();
        _driverInstalled = false;
    }

    // Stop daemon task
    if (_daemonTaskHandle) {
        vTaskDelete((TaskHandle_t)_daemonTaskHandle);
        _daemonTaskHandle = nullptr;
    }

    // Process any remaining events before uninstalling
    uint32_t flags;
    usb_host_lib_handle_events(0, &flags);

    // Uninstall USB Host Library
    usb_host_uninstall();
#endif // ESP_USB_HAS_CDC_ACM

    _state.initialized = false;
    _state.taskRunning = false;
    _state.cdcDeviceCount = 0;

    SFX_LOG_INFO("[UsbHost] HW USB-OTG shut down");
}

bool EspUsbHost::init() {
    if (!_state.initialized) {
        SFX_LOG_ERROR("[UsbHost] Not initialized — call begin() first");
        return false;
    }
    if (_state.taskRunning) return true;  // Already running

#if !ESP_USB_HAS_CDC_ACM
    SFX_LOG_ERROR("[UsbHost] CDC-ACM driver not available — install espressif/usb_host_cdc_acm component");
    return false;
#else

    SFX_LOG_INFO("[UsbHost] Installing ESP-IDF USB Host Library...");

    // Step 1: Install USB Host Library (low-level HCD driver)
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        SFX_LOG_ERROR("[UsbHost] usb_host_install failed: %s", esp_err_to_name(err));
        return false;
    }

    // Step 2: Start daemon task (processes HCD events: connect, disconnect, transfers)
    BaseType_t ret = xTaskCreatePinnedToCore(
        usbHostDaemonTask,
        "usb_daemon",
        4096,                     // Stack size (bytes)
        nullptr,                  // No parameters
        2,                        // Low-ish priority — event routing only
        (TaskHandle_t*)&_daemonTaskHandle,
        0                         // Pin to Core 0 (same as protocol handler)
    );
    if (ret != pdPASS) {
        SFX_LOG_ERROR("[UsbHost] Failed to create daemon task");
        usb_host_uninstall();
        return false;
    }

    // Step 3: Install CDC-ACM class driver (handles CDC enumeration + data)
    const cdc_acm_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 5,
        .xCoreID = 0,                     // Core 0 (same as protocol handler)
        .new_dev_cb = cdcNewDeviceCb,      // Notified when any USB device connects
    };
    err = cdc_acm_host_install(&driver_config);
    if (err != ESP_OK) {
        SFX_LOG_ERROR("[UsbHost] cdc_acm_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete((TaskHandle_t)_daemonTaskHandle);
        _daemonTaskHandle = nullptr;
        usb_host_uninstall();
        return false;
    }
    _driverInstalled = true;

    _state.taskRunning = true;
    SFX_LOG_INFO("[UsbHost] HW USB-OTG ready (CDC-ACM driver installed)");
    return true;
#endif // ESP_USB_HAS_CDC_ACM
}

void EspUsbHost::process() {
    // ESP32 USB Host is fully event-driven via FreeRTOS tasks.
    // No polling needed — the daemon task and CDC-ACM driver task handle
    // device connection, data reception, and events asynchronously.
    //
    // This method exists for API compatibility with PicoUsbHost (which
    // polls tuh_task() on each Core 1 loop iteration).
}

// ============================================================================
// Internal Callback Handlers
// ============================================================================

#if ESP_USB_HAS_CDC_ACM

void EspUsbHost::_handleNewDevice(void* usbDevHandle) {
    usb_device_handle_t usb_dev = (usb_device_handle_t)usbDevHandle;

    // Get device descriptor for VID/PID identification
    const usb_device_desc_t* desc;
    esp_err_t err = usb_host_get_device_descriptor(usb_dev, &desc);
    if (err != ESP_OK) {
        SFX_LOG_WARN("[UsbHost] Failed to get device descriptor: %s", esp_err_to_name(err));
        return;
    }

    uint16_t vid = desc->idVendor;
    uint16_t pid = desc->idProduct;
    SFX_LOG_INFO("[UsbHost] New device: VID=%04X PID=%04X", vid, pid);

    // Pre-allocate a tracking slot (used as user_arg in CDC callbacks)
    int slotIdx = _allocateSlot();
    if (slotIdx < 0) {
        SFX_LOG_WARN("[UsbHost] No free CDC slot (max=%d)", USB_HOST_MAX_CDC_DEVICES);
        return;
    }

    // Create RX stream buffer for this device
    StreamBufferHandle_t rxStream = xStreamBufferCreate(CDC_RX_BUFFER_SIZE, 1);
    if (!rxStream) {
        SFX_LOG_ERROR("[UsbHost] Failed to create RX stream buffer for slot %d", slotIdx);
        _slots[slotIdx] = {};
        return;
    }
    _slots[slotIdx].rxStream = (void*)rxStream;

    // Configure CDC device: data callback, event callback, buffer sizes
    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = cdcEventCb,
        .data_cb = cdcDataCb,
        .user_arg = (void*)(uintptr_t)slotIdx,  // Slot index for callback identification
    };

    // Attempt to open the device as CDC-ACM
    cdc_acm_dev_hdl_t cdc_handle = nullptr;
    err = cdc_acm_host_open(vid, pid, 0, &dev_config, &cdc_handle);
    if (err != ESP_OK) {
        SFX_LOG_WARN("[UsbHost] Not a CDC device (VID=%04X PID=%04X): %s",
                     vid, pid, esp_err_to_name(err));
        vStreamBufferDelete(rxStream);
        _slots[slotIdx] = {};
        return;
    }

    // Configure line coding: 1Mbps 8N1 (ScaleFX binary protocol)
    cdc_acm_line_coding_t line_coding = {
        .dwDTERate   = 1000000,   // 1 Mbps baud rate
        .bCharFormat = 0,         // 1 stop bit
        .bParityType = 0,         // No parity
        .bDataBits   = 8,
    };
    err = cdc_acm_host_line_coding_set(cdc_handle, &line_coding);
    if (err != ESP_OK) {
        SFX_LOG_WARN("[UsbHost] Failed to set line coding: %s", esp_err_to_name(err));
        // Non-fatal — some devices may not support SET_LINE_CODING
    }

    // Set DTR + RTS control lines (required by some CDC devices)
    cdc_acm_host_set_control_line_state(cdc_handle, true, true);

    // Assign a sequential device address (matches TinyUSB behavior on Pico)
    uint8_t devAddr = _nextDevAddr++;

    // Finalize slot tracking
    _slots[slotIdx].cdcHandle = (void*)cdc_handle;
    _slots[slotIdx].devAddr = devAddr;
    _slots[slotIdx].open = true;

    // Register in shared device tracker
    int devIdx = _state.addCdcDevice(devAddr, (uint8_t)slotIdx, vid, pid);
    if (devIdx >= 0) {
        _state.cdcDevices[devIdx].state = UsbDeviceState::Ready;
    }

    SFX_LOG_INFO("[UsbHost] CDC device opened: slot=%d addr=%d VID=%04X PID=%04X",
                 slotIdx, devAddr, vid, pid);

    // Fire mount callback (notifies SlaveServer for registration)
    if (_state.mountCallback) _state.mountCallback(devAddr, vid, pid);
}

void EspUsbHost::_handleCdcData(int slotIdx, const uint8_t* data, size_t len) {
    if (slotIdx < 0 || slotIdx >= USB_HOST_MAX_CDC_DEVICES) return;
    CdcSlot& slot = _slots[slotIdx];
    if (!slot.open || !slot.rxStream) return;

    // Push data into the stream buffer (non-blocking — called from USB Host task)
    size_t written = xStreamBufferSend(
        (StreamBufferHandle_t)slot.rxStream,
        data, len,
        0  // Don't block — callback must return quickly
    );
    if (written < len) {
        SFX_LOG_WARN("[UsbHost] RX overflow: slot=%d lost=%d bytes",
                     slotIdx, (int)(len - written));
    }
    _state.stats.bytes_received += written;

    // Fire RX callback if registered
    if (_state.cdcRxCallback) {
        _state.cdcRxCallback(slot.devAddr, data, len);
    }
}

void EspUsbHost::_handleCdcEvent(int slotIdx, int eventType) {
    if (slotIdx < 0 || slotIdx >= USB_HOST_MAX_CDC_DEVICES) return;

    switch (eventType) {
        case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
            CdcSlot& slot = _slots[slotIdx];
            if (!slot.open) break;

            uint8_t devAddr = slot.devAddr;
            SFX_LOG_INFO("[UsbHost] CDC device disconnected: slot=%d addr=%d", slotIdx, devAddr);

            // Remove from shared device tracker
            _state.removeCdcDevice(devAddr);

            // Clean up slot resources
            if (slot.rxStream) {
                vStreamBufferDelete((StreamBufferHandle_t)slot.rxStream);
            }
            // Note: cdc_acm_host_close() is called by the CDC driver on disconnect
            slot = {};

            // Fire unmount callback (notifies SlaveServer)
            if (_state.unmountCallback) _state.unmountCallback(devAddr);
            break;
        }

        case CDC_ACM_HOST_ERROR:
            SFX_LOG_WARN("[UsbHost] CDC error on slot=%d", slotIdx);
            break;

        case CDC_ACM_HOST_SERIAL_STATE:
            SFX_LOG_DEBUG("[UsbHost] Serial state change on slot=%d", slotIdx);
            break;

        default:
            break;
    }
}

#else // !ESP_USB_HAS_CDC_ACM

// Stub implementations when CDC-ACM component is not available
void EspUsbHost::_handleNewDevice(void*) {}
void EspUsbHost::_handleCdcData(int, const uint8_t*, size_t) {}
void EspUsbHost::_handleCdcEvent(int, int) {}

#endif // ESP_USB_HAS_CDC_ACM

// ============================================================================
// CDC Communication
// ============================================================================

bool EspUsbHost::cdcConnected() const {
    for (int i = 0; i < USB_HOST_MAX_CDC_DEVICES; i++) {
        if (_slots[i].open) return true;
    }
    return false;
}

int EspUsbHost::cdcAvailable(int devIndex) const {
    if (devIndex < 0 || devIndex >= _state.cdcDeviceCount) return 0;
    uint8_t slotIdx = _state.cdcDevices[devIndex].itf_num;
    if (slotIdx >= USB_HOST_MAX_CDC_DEVICES) return 0;
    const CdcSlot& slot = _slots[slotIdx];
    if (!slot.open || !slot.rxStream) return 0;

#if ESP_USB_HAS_CDC_ACM
    return (int)xStreamBufferBytesAvailable((StreamBufferHandle_t)slot.rxStream);
#else
    return 0;
#endif
}

int EspUsbHost::cdcRead(int devIndex, uint8_t* buffer, size_t maxLen) {
    if (!buffer || devIndex < 0 || devIndex >= _state.cdcDeviceCount) return -1;
    uint8_t slotIdx = _state.cdcDevices[devIndex].itf_num;
    if (slotIdx >= USB_HOST_MAX_CDC_DEVICES) return -1;
    const CdcSlot& slot = _slots[slotIdx];
    if (!slot.open || !slot.rxStream) return -1;

#if ESP_USB_HAS_CDC_ACM
    // Non-blocking read from the stream buffer
    size_t count = xStreamBufferReceive(
        (StreamBufferHandle_t)slot.rxStream,
        buffer, maxLen,
        0  // Don't block — caller polls in loop
    );
    return (int)count;
#else
    return -1;
#endif
}

int EspUsbHost::cdcReadByte(int devIndex) {
    uint8_t byte;
    return (cdcRead(devIndex, &byte, 1) == 1) ? byte : -1;
}

int EspUsbHost::cdcWrite(int devIndex, const uint8_t* data, size_t len) {
    if (!data || devIndex < 0 || devIndex >= _state.cdcDeviceCount) return -1;
    uint8_t slotIdx = _state.cdcDevices[devIndex].itf_num;
    if (slotIdx >= USB_HOST_MAX_CDC_DEVICES) return -1;
    const CdcSlot& slot = _slots[slotIdx];
    if (!slot.open || !slot.cdcHandle) return -1;

#if ESP_USB_HAS_CDC_ACM
    // Blocking write with timeout
    esp_err_t err = cdc_acm_host_data_tx_blocking(
        (cdc_acm_dev_hdl_t)slot.cdcHandle,
        data, len,
        CDC_TX_TIMEOUT_MS
    );
    if (err == ESP_OK) {
        _state.stats.bytes_sent += len;
        return (int)len;
    }

    SFX_LOG_WARN("[UsbHost] TX failed: slot=%d err=%s", slotIdx, esp_err_to_name(err));
    return -1;
#else
    (void)slotIdx;
    return -1;
#endif
}

void EspUsbHost::cdcFlush(int devIndex) {
    // TX writes are sent immediately via cdc_acm_host_data_tx_blocking() —
    // no additional flush mechanism needed.
    (void)devIndex;
}

// ============================================================================
// Diagnostics
// ============================================================================

void EspUsbHost::printStatus() const {
    SFX_LOG_INFO("=== USB Host Status (%s) ===", backendName());
    SFX_LOG_INFO("Initialized: %s, Task: %s, Driver: %s",
                 _state.initialized ? "Yes" : "No",
                 _state.taskRunning ? "Yes" : "No",
                 _driverInstalled ? "Yes" : "No");
    SFX_LOG_INFO("CDC devices: %d, Mounted: %lu, Unmounted: %lu",
                 _state.cdcDeviceCount,
                 _state.stats.devices_mounted,
                 _state.stats.devices_unmounted);
    SFX_LOG_INFO("TX: %lu bytes, RX: %lu bytes",
                 _state.stats.bytes_sent,
                 _state.stats.bytes_received);
    for (int i = 0; i < _state.cdcDeviceCount; i++) {
        const CdcDeviceInfo& dev = _state.cdcDevices[i];
        const char* stateStr = "Unknown";
        switch (dev.state) {
            case UsbDeviceState::Disconnected: stateStr = "Disconnected"; break;
            case UsbDeviceState::Connected:    stateStr = "Connected"; break;
            case UsbDeviceState::Mounted:      stateStr = "Mounted"; break;
            case UsbDeviceState::Ready:        stateStr = "Ready"; break;
        }
        SFX_LOG_INFO("  [%d] addr=%d VID=%04X PID=%04X %s",
                     i, dev.dev_addr, dev.vid, dev.pid, stateStr);
    }
}

// ============================================================================
// Private Helpers
// ============================================================================

int EspUsbHost::_findSlotByHandle(void* cdcHandle) const {
    for (int i = 0; i < USB_HOST_MAX_CDC_DEVICES; i++) {
        if (_slots[i].open && _slots[i].cdcHandle == cdcHandle) return i;
    }
    return -1;
}

int EspUsbHost::_allocateSlot() {
    for (int i = 0; i < USB_HOST_MAX_CDC_DEVICES; i++) {
        if (!_slots[i].open) return i;
    }
    return -1;
}

#endif // SFX_PLATFORM_ESP32
#endif // !SCALEFX_SERVER
