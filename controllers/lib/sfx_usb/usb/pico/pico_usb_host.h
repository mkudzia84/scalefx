/*
 * PicoUsbHost — PIO-USB + TinyUSB Host Implementation (RP2040/RP2350)
 *
 * Standalone concrete USB Host class for Raspberry Pi Pico controllers
 * using PIO-USB (software USB via PIO state machine) and TinyUSB host stack.
 *
 * Requirements:
 *   - CPU clock must be 120 MHz or 240 MHz (PIO timing constraint)
 *   - PIO1 is dedicated to USB host (PIO0 available for other uses)
 *   - Core 1 runs init() + process() loop
 *   - Core 0 calls begin() + CDC read/write via SerialBus
 *
 * Auto-selected via `using UsbHost = PicoUsbHost;` in sfx_usb_host.h.
 * Do not include this file directly — include <usb/sfx_usb_host.h>.
 */

#ifndef SFX_PICO_USB_HOST_H
#define SFX_PICO_USB_HOST_H

#if SFX_PLATFORM_PICO

class PicoUsbHost {
public:
    // --- Platform factory singleton -----------------------------------------

    static PicoUsbHost& instance() {
        static PicoUsbHost inst;
        return inst;
    }

    // Delete copy/move
    PicoUsbHost(const PicoUsbHost&) = delete;
    PicoUsbHost& operator=(const PicoUsbHost&) = delete;
    PicoUsbHost(PicoUsbHost&&) = delete;
    PicoUsbHost& operator=(PicoUsbHost&&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /// Configure USB host with default port settings
    bool begin();

    /// Configure USB host with explicit port settings
    bool begin(const UsbPortConfig* configs, int numPorts);

    /// Deinitialize USB host
    void end();

    /// Initialize TinyUSB host stack (call from Core 1)
    bool init();

    /// Poll TinyUSB tasks + handle CDC events (call from Core 1 loop)
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
    const char* backendName() const { return "PIO-USB"; }

    // ========================================================================
    // Status
    // ========================================================================

    bool isReady() const { return _state.initialized && _state.taskRunning; }
    bool isInitialized() const { return _state.initialized; }
    bool isTaskRunning() const { return _state.taskRunning; }
    const UsbHostStats& stats() const { return _state.stats; }

    // --- Internal TinyUSB callback bridge (not part of public API) ----------

    void _onDeviceMount(uint8_t devAddr);
    void _onDeviceUnmount(uint8_t devAddr);
    void _onCdcMount(uint8_t idx);
    void _onCdcUnmount(uint8_t idx);
    void _onCdcRx(uint8_t idx);

private:
    PicoUsbHost() = default;

    UsbHostState _state;
};

#endif // SFX_PLATFORM_PICO
#endif // SFX_PICO_USB_HOST_H
