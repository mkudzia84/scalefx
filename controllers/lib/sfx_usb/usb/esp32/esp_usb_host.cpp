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
#include <platform/sfx_platform.h>   // SFX_MILLIS()

#ifndef SCALEFX_SERVER
#if SFX_PLATFORM_ESP32

#include "serial/diag_log.h"

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
 * FreeRTOS timer callback — fires when no device reconnects within
 * RECOVERY_TIMEOUT_MS after a disconnect. Triggers a root port power cycle
 * to recover disabled hub ports (the ESP-IDF ext_port driver disables ports
 * after a single failed reset attempt with no retry).
 */
static void recoveryTimerCb(TimerHandle_t timer) {
    SFX_LOG_WARN("[UsbHost] Recovery timeout — no reconnect detected, resetting bus");
    EspUsbHost::instance().resetBus();
}

/**
 * Required by ESP-IDF USB Host Library — processes low-level HCD events
 * (device connect/disconnect, transfer completions, port events).
 * Must run continuously in its own FreeRTOS task.
 */
static void usbHostDaemonTask(void* arg) {
    SFX_LOG_INFO("[UsbHost] Daemon task started");
    while (true) {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (err != ESP_OK) {
            EspUsbHost::instance()._stats().hcd_errors++;
            SFX_LOG_ERROR("[UsbHost] usb_host_lib_handle_events error: %s (0x%X)",
                          esp_err_to_name(err), err);
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            SFX_LOG_WARN("[UsbHost] All USB Host clients deregistered — driver may have crashed");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            SFX_LOG_INFO("[UsbHost] All USB device objects freed — ready for new connections");
        }

        // Log non-zero event flags at DEBUG level for instrumentation
        if (event_flags != 0) {
            SFX_LOG_DEBUG("[UsbHost] Daemon event flags: 0x%08lX", (unsigned long)event_flags);
        }
    }
}

// ============================================================================
// CDC-ACM Static Callbacks → EspUsbHost Singleton Bridge
// ============================================================================

static void cdcNewDeviceCb(usb_device_handle_t usb_dev) {
    // IMPORTANT: This callback runs in USB Host Library context.
    // The CDC device CANNOT be opened here (per esp_cdc_acm docs).
    // We only peek descriptors and queue an open request for the open task.
    EspUsbHost::instance()._handleNewDevice((void*)usb_dev);
}

/**
 * Deferred CDC-ACM open task — waits for PendingOpen requests from
 * the new_dev_cb and performs cdc_acm_host_open() outside USB context.
 */
static void usbHostOpenTask(void* arg) {
    SFX_LOG_INFO("[UsbHost] CDC open task started");
    EspUsbHost::PendingOpen req;
    while (true) {
        // Block until a new device needs opening
        if (xQueueReceive((QueueHandle_t)EspUsbHost::instance()._openQueue,
                          &req, portMAX_DELAY) == pdTRUE) {
            SFX_LOG_INFO("[UsbHost] Open task: processing VID=%04X PID=%04X", req.vid, req.pid);
            EspUsbHost::instance()._processOpenRequest(req.vid, req.pid);
        }
    }
}

static bool cdcDataCb(const uint8_t* data, size_t data_len, void* user_arg) {
    int slotIdx = (int)(uintptr_t)user_arg;
    EspUsbHost::instance()._handleCdcData(slotIdx, data, data_len);
    return true;
}

static void cdcEventCb(const cdc_acm_host_dev_event_data_t* event, void* user_ctx) {
    int slotIdx = (int)(uintptr_t)user_ctx;
    SFX_LOG_DEBUG("[UsbHost] CDC event callback: slot=%d type=%d cdc_hdl=%p",
                  slotIdx, (int)event->type, event->data.cdc_hdl);
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
    // Stop recovery timer
    if (_recoveryTimer) {
        xTimerStop((TimerHandle_t)_recoveryTimer, 0);
        xTimerDelete((TimerHandle_t)_recoveryTimer, 0);
        _recoveryTimer = nullptr;
    }

    // Close all open CDC devices
    for (int i = 0; i < USB_HOST_MAX_CDC_DEVICES; i++) {
        if (_slots[i].open && _slots[i].cdcHandle) {
            SFX_LOG_DEBUG("[UsbHost] Closing CDC slot %d in end()", i);
            cdc_acm_host_close((cdc_acm_dev_hdl_t)_slots[i].cdcHandle);
        }
        if (_slots[i].rxStream) {
            vStreamBufferDelete((StreamBufferHandle_t)_slots[i].rxStream);
        }
        _slots[i].reset();
    }

    // Uninstall CDC-ACM class driver (deregisters internal USB Host client)
    if (_driverInstalled) {
        cdc_acm_host_uninstall();
        _driverInstalled = false;
    }

    // Stop open task and delete queue
    if (_openTaskHandle) {
        vTaskDelete((TaskHandle_t)_openTaskHandle);
        _openTaskHandle = nullptr;
    }
    if (_openQueue) {
        vQueueDelete((QueueHandle_t)_openQueue);
        _openQueue = nullptr;
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

    // Set USB component log levels to INFO — ERROR/WARN are always enabled
    // via CORE_DEBUG_LEVEL. All ESP-IDF logs are now captured by DiagLog
    // (via esp_log_set_vprintf redirect) instead of raw-text UART output,
    // so they won't corrupt the COBS binary stream.
    // Temporarily set to ESP_LOG_DEBUG for deep USB stack diagnostics.
    esp_log_level_set("USB_HOST", ESP_LOG_INFO);
    esp_log_level_set("USB_HUB", ESP_LOG_INFO);
    esp_log_level_set("USB_HCDC", ESP_LOG_INFO);
    esp_log_level_set("CDC_ACM", ESP_LOG_INFO);

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

    // Step 4: Create queue + task for deferred CDC-ACM opens
    // (new_dev_cb runs in USB context where cdc_acm_host_open is NOT safe)
    _openQueue = (void*)xQueueCreate(USB_HOST_MAX_CDC_DEVICES, sizeof(PendingOpen));
    // Deferred mount/unmount event queue drained by the loop (processPendingEvents).
    // Sized 2x slots so a quick disconnect+reconnect burst can't drop an event.
    if (_eventQueue == nullptr) {
        _eventQueue = (void*)xQueueCreate(USB_HOST_MAX_CDC_DEVICES * 2, sizeof(PendingEvent));
    }
    if (!_openQueue || !_eventQueue) {
        SFX_LOG_ERROR("[UsbHost] Failed to create open queue");
        cdc_acm_host_uninstall();
        _driverInstalled = false;
        vTaskDelete((TaskHandle_t)_daemonTaskHandle);
        _daemonTaskHandle = nullptr;
        usb_host_uninstall();
        return false;
    }

    ret = xTaskCreatePinnedToCore(
        usbHostOpenTask,
        "usb_open",
        4096,                     // Stack size (bytes)
        nullptr,                  // No parameters
        3,                        // Between daemon (2) and CDC driver (5)
        (TaskHandle_t*)&_openTaskHandle,
        0                         // Pin to Core 0 (same as protocol handler)
    );
    if (ret != pdPASS) {
        SFX_LOG_ERROR("[UsbHost] Failed to create open task");
        vQueueDelete((QueueHandle_t)_openQueue);
        _openQueue = nullptr;
        cdc_acm_host_uninstall();
        _driverInstalled = false;
        vTaskDelete((TaskHandle_t)_daemonTaskHandle);
        _daemonTaskHandle = nullptr;
        usb_host_uninstall();
        return false;
    }

    _state.taskRunning = true;

    // Step 5: Create recovery timer for auto bus reset after failed re-enumeration
    _recoveryTimer = (void*)xTimerCreate(
        "usb_recovery",
        pdMS_TO_TICKS(RECOVERY_TIMEOUT_MS),
        pdFALSE,       // One-shot timer
        nullptr,
        recoveryTimerCb
    );
    if (!_recoveryTimer) {
        SFX_LOG_WARN("[UsbHost] Failed to create recovery timer — auto-recovery disabled");
    }

    SFX_LOG_INFO("[UsbHost] HW USB-OTG ready (CDC-ACM driver + open task installed, recovery=%s)",
                 (_recoveryTimer && _autoRecovery) ? "on" : "off");
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
// Bus Recovery
// ============================================================================

void EspUsbHost::resetBus() {
    if (!_state.taskRunning) {
        SFX_LOG_WARN("[UsbHost] Cannot reset bus — USB host not running");
        return;
    }

    SFX_LOG_WARN("[UsbHost] Bus reset — power cycling root port...");

    // Cancel any pending recovery timer to prevent cascading resets
    if (_recoveryTimer) {
        xTimerStop((TimerHandle_t)_recoveryTimer, 0);
    }

    _lastResetTimestamp_ms = SFX_MILLIS();
    _state.stats.bus_resets++;

#if ESP_USB_HAS_CDC_ACM
    // Power off root port — disconnects hub and ALL downstream devices.
    // The CDC-ACM driver will fire DEVICE_DISCONNECTED for each open device,
    // calling our _handleCdcEvent which cleans up slots and fires unmount callbacks.
    // The cooldown guard (_lastResetTimestamp_ms) prevents those disconnects
    // from starting another recovery timer.
    esp_err_t err = usb_host_lib_set_root_port_power(false);
    if (err != ESP_OK) {
        SFX_LOG_ERROR("[UsbHost] Failed to power off root port: %s", esp_err_to_name(err));
        return;
    }

    // Wait for hub and devices to fully disconnect
    vTaskDelay(pdMS_TO_TICKS(500));

    // Power on root port — hub re-enumerates, then downstream devices
    err = usb_host_lib_set_root_port_power(true);
    if (err != ESP_OK) {
        SFX_LOG_ERROR("[UsbHost] Failed to power on root port: %s", esp_err_to_name(err));
        return;
    }

    SFX_LOG_INFO("[UsbHost] Root port re-powered — hub and devices will re-enumerate");
#endif
}

void EspUsbHost::setAutoRecovery(bool enabled) {
    _autoRecovery = enabled;
    SFX_LOG_INFO("[UsbHost] Auto-recovery %s", enabled ? "enabled" : "disabled");

    // If disabling, cancel any pending recovery timer
    if (!enabled && _recoveryTimer) {
        xTimerStop((TimerHandle_t)_recoveryTimer, 0);
    }
}

// ============================================================================
// Internal Callback Handlers
// ============================================================================

#if ESP_USB_HAS_CDC_ACM

/**
 * Called from new_dev_cb (USB Host context).
 * MUST NOT call cdc_acm_host_open() here — only peek descriptors
 * and queue an open request for the dedicated open task.
 */
void EspUsbHost::_handleNewDevice(void* usbDevHandle) {
    usb_device_handle_t usb_dev = (usb_device_handle_t)usbDevHandle;
    _state.stats.enum_attempts++;

    // Cancel recovery timer — a device enumerated successfully
    if (_recoveryTimer) {
        xTimerStop((TimerHandle_t)_recoveryTimer, 0);
    }

    SFX_LOG_INFO("[UsbHost] >>> New device callback fired (attempt #%lu, current CDC count=%d)",
                 (unsigned long)_state.stats.enum_attempts, _state.cdcDeviceCount.load());

    // Log slot availability for debugging re-connection issues
    int freeSlots = 0;
    for (int i = 0; i < USB_HOST_MAX_CDC_DEVICES; i++) {
        if (!_slots[i].open) freeSlots++;
    }
    SFX_LOG_DEBUG("[UsbHost] Free CDC slots: %d/%d", freeSlots, USB_HOST_MAX_CDC_DEVICES);

    SFX_LOG_INFO("[UsbHost] New device callback fired (attempt #%lu)",
                 (unsigned long)_state.stats.enum_attempts);

    // Peek device descriptor for VID/PID (this IS safe in USB context)
    const usb_device_desc_t* desc;
    esp_err_t err = usb_host_get_device_descriptor(usb_dev, &desc);
    if (err != ESP_OK) {
        _state.stats.enum_desc_failures++;
        SFX_LOG_ERROR("[UsbHost] Failed to get device descriptor: %s (attempt #%lu, desc_failures=%lu)",
                     esp_err_to_name(err),
                     (unsigned long)_state.stats.enum_attempts,
                     (unsigned long)_state.stats.enum_desc_failures);
        return;
    }

    uint16_t vid = desc->idVendor;
    uint16_t pid = desc->idProduct;
    const char* devName = knownDeviceName(vid, pid);
    SFX_LOG_INFO("[UsbHost] New device: VID=%04X PID=%04X class=%02X sub=%02X proto=%02X bcdUSB=%04X%s%s",
                 vid, pid, desc->bDeviceClass, desc->bDeviceSubClass,
                 desc->bDeviceProtocol, desc->bcdUSB,
                 devName ? " — " : "", devName ? devName : "");

    // Queue the open request — cdc_acm_host_open() CANNOT be called from USB context
    if (_openQueue) {
        PendingOpen req = { vid, pid };
        if (xQueueSend((QueueHandle_t)_openQueue, &req, 0) != pdTRUE) {
            SFX_LOG_ERROR("[UsbHost] Open queue full — dropping device VID=%04X PID=%04X", vid, pid);
            _state.stats.enum_failures++;
        } else {
            SFX_LOG_INFO("[UsbHost] Queued open request for VID=%04X PID=%04X", vid, pid);
        }
    } else {
        SFX_LOG_ERROR("[UsbHost] Open queue not created — cannot open device");
        _state.stats.enum_failures++;
    }
}

/**
 * Common CDC session open logic.
 *
 * Allocates a tracking slot, creates an RX stream buffer, calls
 * cdc_acm_host_open() on the data interface (interface 1), configures
 * line coding, and registers the device in the shared state tracker.
 *
 * @param vid         USB Vendor ID
 * @param pid         USB Product ID
 * @param timeout_ms  Connection timeout for cdc_acm_host_open()
 * @return Assigned device address on success, 0 on failure
 */
uint8_t EspUsbHost::_openCdcSession(uint16_t vid, uint16_t pid, uint32_t timeout_ms) {
    // Pre-allocate a tracking slot (used as user_arg in CDC callbacks)
    int slotIdx = _allocateSlot();
    if (slotIdx < 0) {
        SFX_LOG_WARN("[UsbHost] No free CDC slot (max=%d)", USB_HOST_MAX_CDC_DEVICES);
        return 0;
    }

    // Create RX stream buffer for this device
    StreamBufferHandle_t rxStream = xStreamBufferCreate(CDC_RX_BUFFER_SIZE, 1);
    if (!rxStream) {
        SFX_LOG_ERROR("[UsbHost] Failed to create RX stream buffer for slot %d", slotIdx);
        _slots[slotIdx].reset();
        return 0;
    }
    _slots[slotIdx].rxStream = (void*)rxStream;

    // Configure CDC device: data callback, event callback, buffer sizes
    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = timeout_ms,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = cdcEventCb,
        .data_cb = cdcDataCb,
        .user_arg = (void*)(uintptr_t)slotIdx,  // Slot index for callback identification
    };

    // Open the Data Interface (interface 1) directly instead of the Communication
    // Interface (interface 0).  Standard CDC-ACM open on interface 0 claims the
    // notification interrupt-IN endpoint in addition to the two bulk endpoints,
    // consuming 4 HCD channels per device (EP0 + notif IN + bulk IN + bulk OUT).
    // The ESP32-S3 DWC2 OTG only has 8 hardware channels total.  Through a hub
    // (2 channels: EP0 + hub status INT), two standard CDC devices would need
    // 2 + 2×4 = 10 channels — exceeding the hardware limit.
    //
    // By opening interface 1 (Data class, not CDC Communication class), the
    // CDC-ACM driver falls through to the "find 2 bulk endpoints" path and
    // opens only bulk IN + bulk OUT — 3 channels per device (EP0 + bulk×2).
    // Total for hub + 2 devices: 2 + 2×3 = 8 channels — fits exactly.
    //
    // Trade-off: CDC class requests (SET_LINE_CODING, SET_CONTROL_LINE_STATE)
    // return ESP_ERR_NOT_SUPPORTED since CDC functional descriptors are absent
    // on the data interface.  This is harmless — Pico CDC is USB-native (no
    // physical UART), so baud rate and modem control lines are meaningless.
    cdc_acm_dev_hdl_t cdc_handle = nullptr;
    esp_err_t err = cdc_acm_host_open(vid, pid, 1, &dev_config, &cdc_handle);
    if (err != ESP_OK) {
        SFX_LOG_WARN("[UsbHost] CDC open failed VID=%04X PID=%04X iface=1: %s",
                     vid, pid, esp_err_to_name(err));
        vStreamBufferDelete(rxStream);
        _slots[slotIdx].reset();
        return 0;
    }

    // Drive CDC class requests directly via the shared EP0 control pipe.  When
    // the device is opened on interface 1 (data class) the CDC-ACM driver never
    // populates its `intf_func` vtable — the pointers registered by the ACM-
    // compliant path are only wired when the Communication interface is parsed.
    // Calling cdc_acm_host_set_control_line_state() / _line_coding_set() on
    // such a handle dereferences a NULL function pointer or returns
    // ESP_ERR_NOT_SUPPORTED.  cdc_acm_host_send_custom_request() uses the EP0
    // ctrl_transfer directly and works regardless of which interface was opened.
    //
    // Without DTR, the Pico slave's TinyUSB stack treats the CDC link as
    // disconnected (tud_cdc_connected() returns false), so `Serial` never
    // becomes truthy and BoardServer never transmits its IDENTIFY
    // response — SlaveManager then times out every 5s with "No IDENTIFY".

    constexpr uint8_t CDC_REQTYPE_HOST_TO_DEVICE_CLASS_INTERFACE = 0x21;
    constexpr uint8_t CDC_REQ_SET_LINE_CODING         = 0x20;
    constexpr uint8_t CDC_REQ_SET_CONTROL_LINE_STATE  = 0x22;
    constexpr uint16_t CDC_CTRL_LINE_DTR_RTS          = 0x0003;  // bit0=DTR | bit1=RTS
    // Standard CDC-ACM devices place the Communication interface at index 0
    // and the Data interface at index 1 (Pico TinyUSB, STM32 virtual COM, …).
    constexpr uint16_t CDC_COMM_INTERFACE_INDEX       = 0x0000;

    // SET_LINE_CODING — 7-byte payload, little-endian
    uint8_t lineCodingBuf[7] = {
        0x40, 0x42, 0x0F, 0x00,  // dwDTERate = 1,000,000
        0x00,                     // bCharFormat = 1 stop bit
        0x00,                     // bParityType = none
        0x08,                     // bDataBits = 8
    };
    err = cdc_acm_host_send_custom_request(
        cdc_handle,
        CDC_REQTYPE_HOST_TO_DEVICE_CLASS_INTERFACE,
        CDC_REQ_SET_LINE_CODING,
        0,
        CDC_COMM_INTERFACE_INDEX,
        sizeof(lineCodingBuf),
        lineCodingBuf);
    if (err != ESP_OK) {
        SFX_LOG_DEBUG("[UsbHost] SET_LINE_CODING: %s (Pico CDC ignores baud)", esp_err_to_name(err));
    }

    // SET_CONTROL_LINE_STATE — assert DTR+RTS so tud_cdc_connected() becomes true
    err = cdc_acm_host_send_custom_request(
        cdc_handle,
        CDC_REQTYPE_HOST_TO_DEVICE_CLASS_INTERFACE,
        CDC_REQ_SET_CONTROL_LINE_STATE,
        CDC_CTRL_LINE_DTR_RTS,
        CDC_COMM_INTERFACE_INDEX,
        0,
        nullptr);
    if (err != ESP_OK) {
        SFX_LOG_WARN("[UsbHost] SET_CONTROL_LINE_STATE(DTR+RTS) failed: %s", esp_err_to_name(err));
    } else {
        SFX_LOG_DEBUG("[UsbHost] DTR+RTS asserted via EP0 class request (iface=%u)", CDC_COMM_INTERFACE_INDEX);
    }

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

    const char* devName = knownDeviceName(vid, pid);
    SFX_LOG_INFO("[UsbHost] CDC session opened: slot=%d addr=%d VID=%04X PID=%04X%s%s",
                 slotIdx, devAddr, vid, pid,
                 devName ? " — " : "", devName ? devName : "");

    return devAddr;
}

/**
 * Called from the dedicated open task (NOT USB context).
 * Performs the actual cdc_acm_host_open() which is safe here.
 */
void EspUsbHost::_processOpenRequest(uint16_t vid, uint16_t pid) {
    SFX_LOG_INFO("[UsbHost] Processing CDC open: VID=%04X PID=%04X", vid, pid);

    uint8_t devAddr = _openCdcSession(vid, pid, 5000);
    if (devAddr == 0) {
        _state.stats.enum_cdc_open_failures++;
        SFX_LOG_WARN("[UsbHost] CDC-ACM open failed VID=%04X PID=%04X (cdc_open_failures=%lu)",
                     vid, pid, (unsigned long)_state.stats.enum_cdc_open_failures);
        return;
    }

    // DEFER the mount callback to the loop (Rule 56): it does IDENTIFY + wire
    // I/O + mutates the ExpanderService _live[] table; running it here on the
    // open task would race the loop.  The slot is already fully set up, so the
    // loop can fire mountCallback safely on its next processPendingEvents().
    _queueEvent(PendingEvent{UsbEventType::Mount, devAddr, vid, pid, -1});
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
            if (!slot.open.load(std::memory_order_acquire)) break;

            uint8_t devAddr = slot.devAddr;
            SFX_LOG_INFO("[UsbHost] CDC device disconnected: slot=%d addr=%d", slotIdx, devAddr);

            // Stop the wire FIRST (Rule 56): clearing `open` makes the loop's
            // cdcRead/cdcWrite/cdcAvailable bail immediately, and marking
            // pendingUnmount keeps _allocateSlot from re-handing this slot out
            // before the loop reclaims its rxStream.  No further cdcDataCb fires
            // after close (same driver task), so the loop is the only remaining
            // toucher of rxStream.
            slot.open.store(false, std::memory_order_release);
            slot.pendingUnmount.store(true, std::memory_order_release);

            // Close the CDC-ACM driver handle HERE — the driver requires close to
            // run from this disconnect callback (SLIST_FOREACH_SAFE in the
            // DEV_GONE handler, esp_cdc_acm v2.3.0); deferring it would leave the
            // stale device in cdc_devices_list and block re-enumeration.
            if (slot.cdcHandle) {
                SFX_LOG_DEBUG("[UsbHost] Closing CDC handle for slot=%d", slotIdx);
                esp_err_t err = cdc_acm_host_close((cdc_acm_dev_hdl_t)slot.cdcHandle);
                if (err != ESP_OK) {
                    SFX_LOG_WARN("[UsbHost] cdc_acm_host_close failed: %s (slot=%d)",
                                 esp_err_to_name(err), slotIdx);
                }
                slot.cdcHandle = nullptr;   // closed — don't let anything reuse it
            }

            // DEFER the rest to the loop: removeCdcDevice (compacts the shared
            // table the loop reads), vStreamBufferDelete (raced by the loop's
            // cdcRead), slot reset, and the unmount callback (wire I/O + _live[]
            // mutation). processPendingEvents() does them on the loop context.
            _queueEvent(PendingEvent{UsbEventType::Unmount, devAddr, 0, 0, slotIdx});

            // Start auto-recovery timer — if no new device connects within
            // RECOVERY_TIMEOUT_MS, power-cycle root port to recover disabled
            // hub ports. Skip if this disconnect was caused by our own bus reset.
            if (_autoRecovery && _recoveryTimer) {
                uint32_t now = SFX_MILLIS();
                if (now - _lastResetTimestamp_ms > RESET_COOLDOWN_MS) {
                    SFX_LOG_INFO("[UsbHost] Starting recovery timer (%lums) for auto bus reset",
                                 (unsigned long)RECOVERY_TIMEOUT_MS);
                    xTimerReset((TimerHandle_t)_recoveryTimer, 0);
                } else {
                    SFX_LOG_DEBUG("[UsbHost] Skipping recovery timer — within reset cooldown");
                }
            }
            break;
        }

        case CDC_ACM_HOST_ERROR:
            _state.stats.hcd_errors++;
            SFX_LOG_ERROR("[UsbHost] CDC error on slot=%d (hcd_errors=%lu)",
                         slotIdx, (unsigned long)_state.stats.hcd_errors);
            break;

        case CDC_ACM_HOST_SERIAL_STATE:
            SFX_LOG_DEBUG("[UsbHost] Serial state change on slot=%d", slotIdx);
            break;

        default:
            SFX_LOG_WARN("[UsbHost] Unknown CDC event type=%d on slot=%d", eventType, slotIdx);
            break;
    }
}

void EspUsbHost::_queueEvent(const PendingEvent& ev) {
    if (!_eventQueue) return;
    // Non-blocking send from the USB driver/open task context.  If the queue is
    // somehow full the loop is wedged anyway; drop + count rather than block a
    // USB callback (it must return promptly).
    if (xQueueSend((QueueHandle_t)_eventQueue, &ev, 0) != pdTRUE) {
        _state.stats.hcd_errors++;
        SFX_LOG_WARN("[UsbHost] event queue full — dropped %s for addr=%d",
                     ev.type == UsbEventType::Mount ? "MOUNT" : "UNMOUNT", ev.devAddr);
    }
}

void EspUsbHost::processPendingEvents() {
    if (!_eventQueue) return;
    PendingEvent ev;
    // Drain everything queued since the last loop pass.  Runs ONLY here (loop
    // task), so the callbacks + slot reclaim below never race cdcRead/cdcWrite.
    while (xQueueReceive((QueueHandle_t)_eventQueue, &ev, 0) == pdTRUE) {
        if (ev.type == UsbEventType::Mount) {
            if (_state.mountCallback) _state.mountCallback(ev.devAddr, ev.vid, ev.pid);
        } else {  // Unmount
            // Fire the unmount callback FIRST so the ExpanderService tears down
            // its BusClient/_live[] entry (and any wire emit) before we free the
            // slot's RX buffer out from under a stale reference.
            if (_state.unmountCallback) _state.unmountCallback(ev.devAddr);
            _state.removeCdcDevice(ev.devAddr);
            if (ev.slotIdx >= 0 && ev.slotIdx < USB_HOST_MAX_CDC_DEVICES) {
                CdcSlot& slot = _slots[ev.slotIdx];
                if (slot.rxStream) {
                    vStreamBufferDelete((StreamBufferHandle_t)slot.rxStream);
                    slot.rxStream = nullptr;
                }
                slot.reset();   // clears pendingUnmount → slot becomes allocatable
            }
        }
    }
}

#else // !ESP_USB_HAS_CDC_ACM

// Stub implementations when CDC-ACM component is not available
void EspUsbHost::_handleNewDevice(void*) {}
void EspUsbHost::_handleCdcData(int, const uint8_t*, size_t) {}
void EspUsbHost::_handleCdcEvent(int, int) {}
uint8_t EspUsbHost::_openCdcSession(uint16_t, uint16_t, uint32_t) { return 0; }
void EspUsbHost::_queueEvent(const PendingEvent&) {}
void EspUsbHost::processPendingEvents() {}

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
                 _state.cdcDeviceCount.load(),
                 _state.stats.devices_mounted,
                 _state.stats.devices_unmounted);
    SFX_LOG_INFO("TX: %lu bytes, RX: %lu bytes",
                 _state.stats.bytes_sent,
                 _state.stats.bytes_received);
    SFX_LOG_INFO("Enum: attempts=%lu failures=%lu desc_fail=%lu cdc_open_fail=%lu",
                 (unsigned long)_state.stats.enum_attempts,
                 (unsigned long)_state.stats.enum_failures,
                 (unsigned long)_state.stats.enum_desc_failures,
                 (unsigned long)_state.stats.enum_cdc_open_failures);
    SFX_LOG_INFO("Errors: hcd=%lu port=%lu bus_resets=%lu",
                 (unsigned long)_state.stats.hcd_errors,
                 (unsigned long)_state.stats.port_errors,
                 (unsigned long)_state.stats.bus_resets);
    for (int i = 0; i < _state.cdcDeviceCount; i++) {
        const CdcDeviceInfo& dev = _state.cdcDevices[i];
        const char* stateStr = "Unknown";
        switch (dev.state) {
            case UsbDeviceState::Disconnected: stateStr = "Disconnected"; break;
            case UsbDeviceState::Connected:    stateStr = "Connected"; break;
            case UsbDeviceState::Mounted:      stateStr = "Mounted"; break;
            case UsbDeviceState::Ready:        stateStr = "Ready"; break;
        }
        const char* devName = knownDeviceName(dev.vid, dev.pid);
        if (devName) {
            SFX_LOG_INFO("  [%d] addr=%d VID=%04X PID=%04X %s — %s",
                         i, dev.dev_addr, dev.vid, dev.pid, stateStr, devName);
        } else {
            SFX_LOG_INFO("  [%d] addr=%d VID=%04X PID=%04X %s",
                         i, dev.dev_addr, dev.vid, dev.pid, stateStr);
        }
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
        // Skip slots awaiting loop reclaim (disconnected but rxStream not yet
        // freed) so a fresh mount can't stomp a buffer the loop is about to free.
        if (!_slots[i].open.load(std::memory_order_acquire) &&
            !_slots[i].pendingUnmount.load(std::memory_order_acquire)) {
            return i;
        }
    }
    return -1;
}

#endif // SFX_PLATFORM_ESP32
#endif // !SCALEFX_SERVER
