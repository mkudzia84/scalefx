/*
 * EspUsbHost — HW USB-OTG Host Implementation (ESP32-S3)
 *
 * Standalone concrete USB Host class for ESP32-S3 using the native
 * hardware USB-OTG peripheral with ESP-IDF USB Host Library and
 * CDC-ACM class driver.
 *
 * Architecture:
 *   - USB daemon task: processes USB Host Library HCD events (Core 0)
 *   - CDC-ACM driver task: handles class-level events and data (Core 0)
 *   - RX data: CDC data callback → FreeRTOS StreamBuffer → cdcRead()
 *   - TX data: cdcWrite() → cdc_acm_host_data_tx_blocking()
 *
 * Fixed pins: GPIO19 (D-), GPIO20 (D+) on ESP32-S3.
 *
 * Auto-selected via `using UsbHost = EspUsbHost;` in sfx_usb_host.h.
 * Do not include this file directly — include <usb/sfx_usb_host.h>.
 *
 * Dependencies (ESP-IDF managed component):
 *   espressif/usb_host_cdc_acm ^2.0.0  (see src/idf_component.yml)
 */

#ifndef SFX_ESP_USB_HOST_H
#define SFX_ESP_USB_HOST_H

#if SFX_PLATFORM_ESP32

#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include <atomic>                    // CdcSlot::open/pendingUnmount (cross-task, Rule 15)

class EspUsbHost {
public:
    // --- Platform factory singleton -----------------------------------------

    static EspUsbHost& instance() {
        static EspUsbHost inst;
        return inst;
    }

    // Delete copy/move
    EspUsbHost(const EspUsbHost&) = delete;
    EspUsbHost& operator=(const EspUsbHost&) = delete;
    EspUsbHost(EspUsbHost&&) = delete;
    EspUsbHost& operator=(EspUsbHost&&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /// Configure USB host with default port settings
    bool begin();

    /// Configure USB host with explicit port settings
    bool begin(const UsbPortConfig* configs, int numPorts);

    /// Deinitialize USB host
    void end();

    /// Initialize USB stack (installs daemon + CDC-ACM driver tasks)
    bool init();

    /// No-op on ESP32 (event-driven via FreeRTOS tasks)
    void process();

    // ========================================================================
    // CDC Communication
    // ========================================================================

    /// Check if any CDC device is connected and ready
    bool cdcConnected() const;

    /// Number of currently tracked CDC devices
    int cdcDeviceCount() const { return _state.cdcDeviceCount; }

    /// Bytes available to read from a CDC device
    int cdcAvailable(int devIndex) const;

    /// Read data from a CDC device (returns bytes read, -1 on error)
    int cdcRead(int devIndex, uint8_t* buffer, size_t maxLen);

    /// Read a single byte from a CDC device (returns byte, -1 on error)
    int cdcReadByte(int devIndex);

    /// Write data to a CDC device (returns bytes written, -1 on error)
    int cdcWrite(int devIndex, const uint8_t* data, size_t len);

    /// Flush pending write data for a CDC device
    void cdcFlush(int devIndex);

    /// Drain deferred mount/unmount events on the LOOP task.  MUST be called
    /// from the main loop (e.g. ExpanderService::update()) — it fires the mount/
    /// unmount callbacks and reclaims disconnected slots (free rxStream, reset)
    /// on the single loop context, so the higher-priority USB driver/open tasks
    /// never race the loop's cdcRead/cdcWrite or emit wire packets (Rule 56).
    void processPendingEvents();

    // ========================================================================
    // Callbacks
    // ========================================================================

    void onMount(UsbMountCallback cb) { _state.mountCallback = cb; }
    void onUnmount(UsbUnmountCallback cb) { _state.unmountCallback = cb; }
    void onCdcReceive(UsbCdcRxCallback cb) { _state.cdcRxCallback = cb; }

    // ========================================================================
    // Device Info & Diagnostics
    // ========================================================================

    /// Get info for a tracked CDC device (nullptr if invalid index)
    const CdcDeviceInfo* getCdcDevice(int devIndex) const {
        return _state.getCdcDevice(devIndex);
    }

    /// Log USB host status via DiagLog
    void printStatus() const;

    /// Get platform backend name
    const char* backendName() const { return "HW USB-OTG"; }

    // ========================================================================
    // Bus Recovery
    // ========================================================================

    /// Power-cycle root port to force re-enumeration of hub and all downstream
    /// devices. This disconnects everything momentarily then re-enumerates.
    /// Safe to call from any task context (uses vTaskDelay internally).
    /// Auto-recovery is always armed when the recovery timer was created: if a
    /// device disconnects and nothing reconnects within RECOVERY_TIMEOUT_MS, the
    /// bus is power-cycled (on the worker task) to recover a hub port the
    /// ESP-IDF ext_port driver disabled after a failed reset.
    void resetBus();

    // ========================================================================
    // Status
    // ========================================================================

    bool isReady() const { return _state.initialized && _state.taskRunning; }
    bool isInitialized() const { return _state.initialized; }
    bool isTaskRunning() const { return _state.taskRunning; }
    const UsbHostStats& stats() const { return _state.stats; }

    // --- Internal callback bridge (not part of public API) ------------------
    // Called from static C callbacks in esp_usb_host.cpp.

    /// Deferred USB work item, processed on the worker task (`usb_worker`) off
    /// the USB-callback and timer-service contexts.  Two jobs share the queue:
    ///   - OpenCdc:  cdc_acm_host_open() (queued from new_dev_cb — open is unsafe
    ///               in USB-callback context).
    ///   - BusReset: resetBus() root-port power-cycle (queued from the recovery
    ///               timer — the deep HCD calls + 500 ms block must NOT run on the
    ///               3120 B timer-service task; see requestBusReset()).
    ///   - SynthDisconnect: _handleCdcEvent(DISCONNECTED) for a slot whose
    ///               device died without delivering the callback (detected by
    ///               consecutive TX failures).  The teardown closes the CDC
    ///               handle = deep USB-host frames — must NOT run on the
    ///               caller of cdcWrite (loopTask overflowed exactly there,
    ///               coredump 2026-08-08: exccause 0x41, 0xa5-poisoned stack).
    struct PendingWork {
        enum class Kind : uint8_t { OpenCdc, BusReset, SynthDisconnect };
        Kind     kind = Kind::OpenCdc;
        uint16_t vid  = 0;   // SynthDisconnect: slot index in `vid`
        uint16_t pid  = 0;
    };

    /// Queue handle for deferred USB work (accessible to the worker task)
    void* _workQueue = nullptr;          // QueueHandle_t for PendingWork

    /// Called from new_dev_cb — only peeks descriptors, queues open request
    void _handleNewDevice(void* usbDevHandle);

    /// Called from worker task — performs the actual CDC-ACM open (outside USB context)
    void _processOpenRequest(uint16_t vid, uint16_t pid);

    /// Queue a deferred bus reset onto the worker task.  Safe to call from the
    /// timer-service task (the recovery timer) — the heavy resetBus() then runs
    /// on `usb_worker` (8 KB stack) instead of the 3120 B timer task.
    void requestBusReset();

    void _handleCdcData(int slotIdx, const uint8_t* data, size_t len);
    void _handleCdcEvent(int slotIdx, int eventType);

    /// Stats accessor for daemon task (internal only)
    UsbHostStats& _stats() { return _state.stats; }

private:
    EspUsbHost() = default;

    UsbHostState _state;

    /// RX buffer size per CDC device (bytes)
    static constexpr size_t CDC_RX_BUFFER_SIZE = 4096;

    /// TX timeout for blocking writes (ms)
    static constexpr uint32_t CDC_TX_TIMEOUT_MS = 100;

    /// Auto-recovery: time to wait after disconnect before bus reset (ms)
    static constexpr uint32_t RECOVERY_TIMEOUT_MS = 5000;

    /// Cooldown after a bus reset — ignore disconnects caused by the reset itself (ms)
    static constexpr uint32_t RESET_COOLDOWN_MS = 10000;

    /// Per-CDC-device slot tracking (opaque handles avoid ESP-IDF includes).
    /// Concurrency (Rule 15 / 56): the CDC-ACM driver task (disconnect) and the
    /// open task (mount) only FLAG state here; all teardown (free rxStream, reset
    /// slot, fire mount/unmount callbacks, removeCdcDevice) is deferred to the
    /// loop task via processPendingEvents().  `open` gates cdcRead/cdcWrite/RX so
    /// it's atomic; `pendingUnmount` marks a slot disconnected-but-not-yet-
    /// reclaimed so _allocateSlot won't hand it out before the loop frees it.
    struct CdcSlot {
        void* cdcHandle = nullptr;   // cdc_acm_dev_hdl_t (opaque)
        void* rxStream  = nullptr;   // StreamBufferHandle_t (opaque)
        uint8_t devAddr = 0;
        std::atomic<bool> open{false};
        std::atomic<bool> pendingUnmount{false};
        // Set (seq_cst) by cdcWrite around the blocking tx; the disconnect
        // callback waits for it to clear before cdc_acm_host_close(), so a close
        // can never free the handle out from under an in-flight TX (Rule 15 /
        // mirrors AudioMixer::destroyChannelSourceSafe's busy-flag handshake).
        std::atomic<bool> txBusy{false};
        // Consecutive TX ESP_ERR_INVALID_STATE count — a device that dies
        // WITHOUT a disconnect callback (bench 2026-08-08: half-dead slot
        // spammed forever) is detected here and the disconnect synthesized.
        std::atomic<uint8_t> txDeadCount{0};
        void reset() {
            cdcHandle = nullptr; rxStream = nullptr; devAddr = 0;
            pendingUnmount.store(false, std::memory_order_release);
            txBusy.store(false, std::memory_order_release);
            txDeadCount.store(0, std::memory_order_release);
            open.store(false, std::memory_order_release);
        }
    };
    CdcSlot _slots[USB_HOST_MAX_CDC_DEVICES] = {};

    /// Deferred mount/unmount event — pushed by the USB tasks, drained by the
    /// loop in processPendingEvents() so callbacks + slot teardown run on the
    /// single loop context (never the higher-priority USB driver/open tasks).
    enum class UsbEventType : uint8_t { Mount, Unmount };
    struct PendingEvent {
        UsbEventType type;
        uint8_t  devAddr;
        uint16_t vid;
        uint16_t pid;
        int      slotIdx;
    };
    void* _eventQueue = nullptr;     // QueueHandle_t for PendingEvent
    void _queueEvent(const PendingEvent& ev);

    void* _daemonTaskHandle = nullptr;   // TaskHandle_t (opaque)
    void* _workTaskHandle = nullptr;     // TaskHandle_t for deferred USB work (open + bus reset)
    bool _driverInstalled = false;
    uint8_t _nextDevAddr = 1;            // Sequential device address counter

    // Bus recovery state
    void* _recoveryTimer = nullptr;      // TimerHandle_t (null = recovery off)
    // Written by resetBus() on the worker task, read by _handleCdcEvent on the CDC
    // driver task (the reset-cooldown guard) — cross-task, so atomic with explicit
    // ordering (release on write / acquire on read) per Rule 15.
    std::atomic<uint32_t> _lastResetTimestamp_ms{0}; // SFX_MILLIS() of last bus reset

    /// Common CDC session open logic — shared by _processOpenRequest and reopenCdcDevice.
    /// Returns assigned devAddr on success, 0 on failure.
    uint8_t _openCdcSession(uint16_t vid, uint16_t pid, uint32_t timeout_ms);

    int _findSlotByHandle(void* cdcHandle) const;
    int _allocateSlot();
};

#endif // SFX_PLATFORM_ESP32
#endif // SFX_ESP_USB_HOST_H
