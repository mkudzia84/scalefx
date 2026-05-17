/*
 * PicoUsbHost — PIO-USB + TinyUSB Host Implementation (RP2040/RP2350)
 *
 * All PIO-USB and TinyUSB host stack interactions are isolated here.
 * Shared device-tracking helpers live in UsbHostState (sfx_usb_host.cpp).
 *
 * TinyUSB C callbacks bridge into PicoUsbHost via PicoUsbHost::instance().
 */

#include <usb/sfx_usb_host.h>

#ifndef SCALEFX_SERVER
#if SFX_PLATFORM_PICO

#include "serial/diag_log.h"
#include "pio_usb.h"
#include "tusb.h"
#include "class/cdc/cdc_host.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"

// PIO-USB configuration — static since there's only one USB host (singleton)
static pio_usb_configuration_t g_pioUsbConfig = PIO_USB_DEFAULT_CONFIG;

// ============================================================================
// Lifecycle
// ============================================================================

bool PicoUsbHost::begin() {
    UsbPortConfig defaultConfig;
    defaultConfig.enabled = true;
    defaultConfig.dp_pin = USB_HOST_PORT_0_DP_DEFAULT;
    strncpy(defaultConfig.name, "USB0", sizeof(defaultConfig.name));
    return begin(&defaultConfig, 1);
}

bool PicoUsbHost::begin(const UsbPortConfig* configs, int numPorts) {
    if (_state.initialized) return true;

    SFX_LOG_INFO("[UsbHost] Initializing PIO-USB backend...");

    // PIO-USB requires 120 MHz or 240 MHz CPU clock for precise timing
    uint32_t cpuHz = clock_get_hz(clk_sys);
    if (cpuHz != 120000000UL && cpuHz != 240000000UL) {
        SFX_LOG_INFO("[UsbHost] Adjusting CPU clock from %lu to 120MHz", cpuHz);
        set_sys_clock_khz(120000, true);
    }

    // Store port configurations
    for (int i = 0; i < USB_HOST_MAX_PORTS && i < numPorts; i++) {
        _state.ports[i] = configs[i];
        if (configs[i].enabled) {
            SFX_LOG_DEBUG("[UsbHost] Port %d: D+=%d D-=%d name='%s'",
                          i, configs[i].dp_pin, configs[i].dp_pin + 1, configs[i].name);
        }
    }

    // Configure PIO USB — use first enabled port's D+ pin
    for (int i = 0; i < USB_HOST_MAX_PORTS; i++) {
        if (_state.ports[i].enabled) {
            g_pioUsbConfig.pin_dp = _state.ports[i].dp_pin;
            break;
        }
    }

    _state.initialized = true;
    SFX_LOG_INFO("[UsbHost] PIO-USB configured (D+ pin %d)", g_pioUsbConfig.pin_dp);
    return true;
}

void PicoUsbHost::end() {
    if (!_state.initialized) return;
    _state.initialized = false;
    _state.taskRunning = false;
    SFX_LOG_INFO("[UsbHost] PIO-USB deinitialized");
}

bool PicoUsbHost::init() {
    // Must be called from Core 1 (PIO runs on PIO1 / Core 1)
    if (!_state.initialized) {
        SFX_LOG_ERROR("[UsbHost] Not initialized — call begin() first");
        return false;
    }

    SFX_LOG_INFO("[UsbHost] Initializing TinyUSB host on Core 1...");

    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &g_pioUsbConfig);

    if (!tuh_init(1)) {
        SFX_LOG_ERROR("[UsbHost] TinyUSB host init failed");
        return false;
    }

    _state.taskRunning = true;
    SFX_LOG_INFO("[UsbHost] TinyUSB host ready");
    return true;
}

void PicoUsbHost::process() {
    if (!_state.initialized || !_state.taskRunning) return;

    // Run TinyUSB host task — handles enumeration, callbacks, etc.
    tuh_task();

    // Process CDC receive for each device
    for (int i = 0; i < _state.cdcDeviceCount; i++) {
        CdcDeviceInfo& dev = _state.cdcDevices[i];
        if (dev.connected && dev.state == UsbDeviceState::Ready) {
            if (tuh_cdc_mounted(dev.itf_num) && tuh_cdc_read_available(dev.itf_num)) {
                uint8_t buf[64];
                uint32_t count = tuh_cdc_read(dev.itf_num, buf, sizeof(buf));
                if (count > 0) {
                    _state.stats.bytes_received += count;
                    if (_state.cdcRxCallback) {
                        _state.cdcRxCallback(dev.dev_addr, buf, count);
                    }
                }
            }
        }
    }
}

// ============================================================================
// CDC Communication
// ============================================================================

bool PicoUsbHost::cdcConnected() const {
    for (int i = 0; i < _state.cdcDeviceCount; i++) {
        if (_state.cdcDevices[i].connected &&
            _state.cdcDevices[i].state == UsbDeviceState::Ready) {
            return true;
        }
    }
    return false;
}

int PicoUsbHost::cdcAvailable(int devIndex) const {
    if (devIndex < 0 || devIndex >= _state.cdcDeviceCount) return 0;
    const CdcDeviceInfo& dev = _state.cdcDevices[devIndex];
    if (!dev.connected || dev.state != UsbDeviceState::Ready) return 0;
    if (tuh_cdc_mounted(dev.itf_num)) {
        return tuh_cdc_read_available(dev.itf_num);
    }
    return 0;
}

int PicoUsbHost::cdcRead(int devIndex, uint8_t* buffer, size_t maxLen) {
    if (!buffer || devIndex < 0 || devIndex >= _state.cdcDeviceCount) return -1;
    CdcDeviceInfo& dev = _state.cdcDevices[devIndex];
    if (!dev.connected || dev.state != UsbDeviceState::Ready) return -1;
    if (!tuh_cdc_mounted(dev.itf_num)) return -1;

    uint32_t count = tuh_cdc_read(dev.itf_num, buffer, maxLen);
    if (count > 0) _state.stats.bytes_received += count;
    return (int)count;
}

int PicoUsbHost::cdcReadByte(int devIndex) {
    uint8_t byte;
    return (cdcRead(devIndex, &byte, 1) == 1) ? byte : -1;
}

int PicoUsbHost::cdcWrite(int devIndex, const uint8_t* data, size_t len) {
    if (!data || devIndex < 0 || devIndex >= _state.cdcDeviceCount) return -1;
    CdcDeviceInfo& dev = _state.cdcDevices[devIndex];
    if (!dev.connected || dev.state != UsbDeviceState::Ready) return -1;
    if (!tuh_cdc_mounted(dev.itf_num)) return -1;

    uint32_t count = tuh_cdc_write(dev.itf_num, data, len);
    if (count > 0) {
        _state.stats.bytes_sent += count;
        tuh_cdc_write_flush(dev.itf_num);
    }
    return (int)count;
}

void PicoUsbHost::cdcFlush(int devIndex) {
    if (devIndex < 0 || devIndex >= _state.cdcDeviceCount) return;
    CdcDeviceInfo& dev = _state.cdcDevices[devIndex];
    if (dev.connected && tuh_cdc_mounted(dev.itf_num)) {
        tuh_cdc_write_flush(dev.itf_num);
    }
}

// ============================================================================
// Diagnostics
// ============================================================================

void PicoUsbHost::printStatus() const {
    SFX_LOG_INFO("=== USB Host Status (%s) ===", backendName());
    SFX_LOG_INFO("Initialized: %s, Task: %s",
                 _state.initialized ? "Yes" : "No",
                 _state.taskRunning ? "Yes" : "No");
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
// Internal TinyUSB Callback Bridge
// ============================================================================

void PicoUsbHost::_onDeviceMount(uint8_t devAddr) {
    uint16_t vid, pid;
    tuh_vid_pid_get(devAddr, &vid, &pid);
    SFX_LOG_INFO("[UsbHost] Device mounted: addr=%d VID=%04X PID=%04X", devAddr, vid, pid);
    if (_state.mountCallback) _state.mountCallback(devAddr, vid, pid);
}

void PicoUsbHost::_onDeviceUnmount(uint8_t devAddr) {
    SFX_LOG_INFO("[UsbHost] Device unmounted: addr=%d", devAddr);
    _state.removeCdcDevice(devAddr);
    if (_state.unmountCallback) _state.unmountCallback(devAddr);
}

void PicoUsbHost::_onCdcMount(uint8_t idx) {
    SFX_LOG_DEBUG("[UsbHost] CDC interface mounted: idx=%d", idx);
    tuh_itf_info_t info;
    if (tuh_cdc_itf_get_info(idx, &info)) {
        uint16_t vid, pid;
        tuh_vid_pid_get(info.daddr, &vid, &pid);
        int devIdx = _state.addCdcDevice(info.daddr, idx, vid, pid);
        if (devIdx >= 0) {
            _state.cdcDevices[devIdx].state = UsbDeviceState::Ready;
        }
    }
    // Set CDC line coding for ScaleFX protocol (1 Mbps, 8N1)
    cdc_line_coding_t lineCoding = {
        .bit_rate = 1000000,
        .stop_bits = 0,
        .parity = 0,
        .data_bits = 8
    };
    tuh_cdc_set_line_coding(idx, &lineCoding, nullptr, 0);
    tuh_cdc_set_control_line_state(idx, CDC_CONTROL_LINE_STATE_DTR | CDC_CONTROL_LINE_STATE_RTS, nullptr, 0);
    SFX_LOG_DEBUG("[UsbHost] CDC line coding set: 1Mbps 8N1, DTR+RTS");
}

void PicoUsbHost::_onCdcUnmount(uint8_t idx) {
    SFX_LOG_DEBUG("[UsbHost] CDC interface unmounted: idx=%d", idx);
}

void PicoUsbHost::_onCdcRx(uint8_t idx) {
    if (!_state.cdcRxCallback) return;
    uint8_t buf[64];
    uint32_t count = tuh_cdc_read(idx, buf, sizeof(buf));
    if (count > 0) {
        for (int i = 0; i < _state.cdcDeviceCount; i++) {
            if (_state.cdcDevices[i].itf_num == idx) {
                _state.cdcRxCallback(_state.cdcDevices[i].dev_addr, buf, count);
                _state.stats.bytes_received += count;
                break;
            }
        }
    }
}

// ============================================================================
// TinyUSB C Callbacks → PicoUsbHost Singleton Bridge
// ============================================================================

void tuh_mount_cb(uint8_t dev_addr) {
    PicoUsbHost::instance()._onDeviceMount(dev_addr);
}

void tuh_umount_cb(uint8_t dev_addr) {
    PicoUsbHost::instance()._onDeviceUnmount(dev_addr);
}

void tuh_cdc_mount_cb(uint8_t idx) {
    PicoUsbHost::instance()._onCdcMount(idx);
}

void tuh_cdc_umount_cb(uint8_t idx) {
    PicoUsbHost::instance()._onCdcUnmount(idx);
}

void tuh_cdc_rx_cb(uint8_t idx) {
    PicoUsbHost::instance()._onCdcRx(idx);
}

void tuh_cdc_tx_complete_cb(uint8_t idx) {
    (void)idx;
}

#endif // SFX_PLATFORM_PICO
#endif // !SCALEFX_SERVER
