/*
 * Serial USB Host - PIO-USB Host Implementation
 *
 * USB Host manager implementation for ScaleFX client controllers (HubFX).
 */

#include "serial_usb_host.h"

// USB Host functionality is only needed for HubFX (client)
#ifndef SCALEFX_SERVER
#include "pio_usb.h"
#include "tusb.h"
// Explicitly include TinyUSB host CDC driver
#include "class/cdc/cdc_host.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"

// ============================================================================
// Global state for TinyUSB callbacks
// ============================================================================

static UsbHost* g_usbHost = nullptr;
static pio_usb_configuration_t g_pioUsbConfig = PIO_USB_DEFAULT_CONFIG;

// ============================================================================
// UsbHost Implementation
// ============================================================================

UsbHost::~UsbHost() {
    end();
}

bool UsbHost::begin() {
    UsbPortConfig defaultConfig;
    defaultConfig.enabled = true;
    defaultConfig.dp_pin = USB_HOST_PORT_0_DP_DEFAULT;
    strncpy(defaultConfig.name, "USB0", sizeof(defaultConfig.name));
    
    return begin(&defaultConfig, 1);
}

bool UsbHost::begin(const UsbPortConfig* configs, int numPorts) {
    if (_initialized) {
        return true;
    }
    
    Serial.println("[UsbHost] Initializing...");
    
    // Set CPU clock for USB timing
    uint32_t cpuHz = clock_get_hz(clk_sys);
    if (cpuHz != 120000000UL && cpuHz != 240000000UL) {
        Serial.printf("[UsbHost] Adjusting CPU clock from %lu to 120MHz\n", cpuHz);
        set_sys_clock_khz(120000, true);
    }
    
    // Store configurations
    for (int i = 0; i < USB_HOST_MAX_PORTS && i < numPorts; i++) {
        _ports[i] = configs[i];
        if (configs[i].enabled) {
            Serial.printf("[UsbHost] Port %d: D+=%d, D-=%d, name='%s'\n",
                         i, configs[i].dp_pin, configs[i].dp_pin + 1, configs[i].name);
        }
    }
    
    // Configure PIO USB
    for (int i = 0; i < USB_HOST_MAX_PORTS; i++) {
        if (_ports[i].enabled) {
            g_pioUsbConfig.pin_dp = _ports[i].dp_pin;
            Serial.printf("[UsbHost] Primary USB port: D+ pin %d\n", g_pioUsbConfig.pin_dp);
            break;
        }
    }
    
    g_usbHost = this;
    _initialized = true;
    
    Serial.println("[UsbHost] Initialization complete");
    return true;
}

void UsbHost::end() {
    if (!_initialized) return;
    
    _initialized = false;
    _taskRunning = false;
    g_usbHost = nullptr;
    
    Serial.println("[UsbHost] Deinitialized");
}

bool UsbHost::init() {
    if (!_initialized) {
        Serial.println("[UsbHost] Not initialized - call begin() first");
        return false;
    }
    
    Serial.println("[UsbHost] Initializing TinyUSB host...");
    
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &g_pioUsbConfig);
    
    if (!tuh_init(1)) {
        Serial.println("[UsbHost] Core 1: TinyUSB host init failed!");
        return false;
    }
    
    _taskRunning = true;
    Serial.println("[UsbHost] TinyUSB host initialized");
    return true;
}

void UsbHost::process() {
    if (!_initialized || !_taskRunning) return;
    
    // Run TinyUSB host task - handles enumeration, callbacks, etc.
    tuh_task();
    
    // Process CDC receive callbacks
    for (int i = 0; i < _cdcDeviceCount; i++) {
        CdcDeviceInfo& dev = _cdcDevices[i];
        if (dev.connected && dev.state == UsbDeviceState::Ready) {
            if (tuh_cdc_mounted(dev.itf_num) && tuh_cdc_read_available(dev.itf_num)) {
                uint8_t buf[64];
                uint32_t count = tuh_cdc_read(dev.itf_num, buf, sizeof(buf));
                if (count > 0) {
                    _stats.bytes_received += count;
                    if (_cdcRxCallback) {
                        _cdcRxCallback(dev.dev_addr, buf, count);
                    }
                }
            }
        }
    }
}

bool UsbHost::cdcConnected() const {
    for (int i = 0; i < _cdcDeviceCount; i++) {
        if (_cdcDevices[i].connected && _cdcDevices[i].state == UsbDeviceState::Ready) {
            return true;
        }
    }
    return false;
}

int UsbHost::cdcAvailable(int devIndex) const {
    if (devIndex < 0 || devIndex >= _cdcDeviceCount) return 0;
    
    const CdcDeviceInfo& dev = _cdcDevices[devIndex];
    if (!dev.connected || dev.state != UsbDeviceState::Ready) return 0;
    
    if (tuh_cdc_mounted(dev.itf_num)) {
        return tuh_cdc_read_available(dev.itf_num);
    }
    return 0;
}

int UsbHost::cdcRead(int devIndex, uint8_t* buffer, size_t maxLen) {
    if (!buffer || devIndex < 0 || devIndex >= _cdcDeviceCount) return -1;
    
    CdcDeviceInfo& dev = _cdcDevices[devIndex];
    if (!dev.connected || dev.state != UsbDeviceState::Ready) return -1;
    if (!tuh_cdc_mounted(dev.itf_num)) return -1;
    
    uint32_t count = tuh_cdc_read(dev.itf_num, buffer, maxLen);
    if (count > 0) {
        _stats.bytes_received += count;
    }
    return (int)count;
}

int UsbHost::cdcReadByte(int devIndex) {
    uint8_t byte;
    return (cdcRead(devIndex, &byte, 1) == 1) ? byte : -1;
}

int UsbHost::cdcWrite(int devIndex, const uint8_t* data, size_t len) {
    if (!data || devIndex < 0 || devIndex >= _cdcDeviceCount) return -1;
    
    CdcDeviceInfo& dev = _cdcDevices[devIndex];
    if (!dev.connected || dev.state != UsbDeviceState::Ready) return -1;
    if (!tuh_cdc_mounted(dev.itf_num)) return -1;
    
    uint32_t count = tuh_cdc_write(dev.itf_num, data, len);
    if (count > 0) {
        _stats.bytes_sent += count;
        tuh_cdc_write_flush(dev.itf_num);
    }
    return (int)count;
}

int UsbHost::cdcPrint(int devIndex, const char* str) {
    if (!str) return -1;
    return cdcWrite(devIndex, (const uint8_t*)str, strlen(str));
}

int UsbHost::cdcPrintln(int devIndex, const char* str) {
    int written = cdcPrint(devIndex, str);
    if (written < 0) return written;
    
    int nl = cdcWrite(devIndex, (const uint8_t*)"\r\n", 2);
    return (nl < 0) ? nl : written + nl;
}

void UsbHost::cdcFlush(int devIndex) {
    if (devIndex < 0 || devIndex >= _cdcDeviceCount) return;
    
    CdcDeviceInfo& dev = _cdcDevices[devIndex];
    if (dev.connected && tuh_cdc_mounted(dev.itf_num)) {
        tuh_cdc_write_flush(dev.itf_num);
    }
}

const CdcDeviceInfo* UsbHost::getCdcDevice(int devIndex) const {
    if (devIndex < 0 || devIndex >= _cdcDeviceCount) return nullptr;
    return &_cdcDevices[devIndex];
}

void UsbHost::printStatus() const {
    Serial.println("\n=== USB Host Status ===");
    Serial.printf("Initialized: %s\n", _initialized ? "Yes" : "No");
    Serial.printf("Task Running: %s\n", _taskRunning ? "Yes" : "No");
    Serial.printf("CPU Clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    
    Serial.println("\nUSB Port:");
    for (int i = 0; i < USB_HOST_MAX_PORTS; i++) {
        Serial.printf("  Port %d: %s, D+=%d, D-=%d, name='%s'\n",
                     i, _ports[i].enabled ? "Enabled" : "Disabled",
                     _ports[i].dp_pin, _ports[i].dp_pin + 1, _ports[i].name);
    }
    
    Serial.println("\nCDC Devices:");
    if (_cdcDeviceCount == 0) {
        Serial.println("  (none connected)");
    } else {
        for (int i = 0; i < _cdcDeviceCount; i++) {
            const CdcDeviceInfo& dev = _cdcDevices[i];
            const char* stateStr;
            switch (dev.state) {
                case UsbDeviceState::Disconnected: stateStr = "Disconnected"; break;
                case UsbDeviceState::Connected: stateStr = "Connected"; break;
                case UsbDeviceState::Mounted: stateStr = "Mounted"; break;
                case UsbDeviceState::Ready: stateStr = "Ready"; break;
                default: stateStr = "Unknown"; break;
            }
            Serial.printf("  Device %d: addr=%d, VID=%04X, PID=%04X, state=%s\n",
                         i, dev.dev_addr, dev.vid, dev.pid, stateStr);
        }
    }
    
    Serial.println("\nStatistics:");
    Serial.printf("  Devices mounted: %lu\n", _stats.devices_mounted);
    Serial.printf("  Devices unmounted: %lu\n", _stats.devices_unmounted);
    Serial.printf("  Bytes sent: %lu\n", _stats.bytes_sent);
    Serial.printf("  Bytes received: %lu\n", _stats.bytes_received);
    Serial.println("========================\n");
}

int UsbHost::findDeviceByAddr(uint8_t devAddr) const {
    for (int i = 0; i < _cdcDeviceCount; i++) {
        if (_cdcDevices[i].dev_addr == devAddr) {
            return i;
        }
    }
    return -1;
}

int UsbHost::addCdcDevice(uint8_t devAddr, uint8_t itfNum) {
    if (_cdcDeviceCount >= USB_HOST_MAX_CDC_DEVICES) {
        Serial.println("[UsbHost] Max CDC devices reached");
        return -1;
    }
    
    int idx = _cdcDeviceCount;
    CdcDeviceInfo& dev = _cdcDevices[idx];
    
    dev.connected = true;
    dev.dev_addr = devAddr;
    dev.itf_num = itfNum;
    dev.state = UsbDeviceState::Connected;
    
    tuh_vid_pid_get(devAddr, &dev.vid, &dev.pid);
    
    _cdcDeviceCount++;
    _stats.devices_mounted++;
    
    Serial.printf("[UsbHost] CDC device added: addr=%d, itf=%d, VID=%04X, PID=%04X\n",
                  devAddr, itfNum, dev.vid, dev.pid);
    
    return idx;
}

void UsbHost::removeCdcDevice(uint8_t devAddr) {
    int idx = findDeviceByAddr(devAddr);
    if (idx < 0) return;
    
    Serial.printf("[UsbHost] CDC device removed: addr=%d\n", devAddr);
    
    for (int i = idx; i < _cdcDeviceCount - 1; i++) {
        _cdcDevices[i] = _cdcDevices[i + 1];
    }
    _cdcDeviceCount--;
    _stats.devices_unmounted++;
}

// Internal callbacks from TinyUSB
void UsbHost::_onDeviceMount(uint8_t devAddr) {
    Serial.printf("[UsbHost] Device mounted: addr=%d\n", devAddr);
    
    uint16_t vid, pid;
    tuh_vid_pid_get(devAddr, &vid, &pid);
    
    if (_mountCallback) {
        _mountCallback(devAddr, vid, pid);
    }
}

void UsbHost::_onDeviceUnmount(uint8_t devAddr) {
    Serial.printf("[UsbHost] Device unmounted: addr=%d\n", devAddr);
    
    removeCdcDevice(devAddr);
    
    if (_unmountCallback) {
        _unmountCallback(devAddr);
    }
}

void UsbHost::_onCdcMount(uint8_t idx) {
    Serial.printf("[UsbHost] CDC mounted: idx=%d\n", idx);
    
    tuh_itf_info_t info;
    if (tuh_cdc_itf_get_info(idx, &info)) {
        int devIdx = addCdcDevice(info.daddr, idx);
        if (devIdx >= 0) {
            _cdcDevices[devIdx].state = UsbDeviceState::Ready;
        }
    }
    
    cdc_line_coding_t lineCoding = {
        .bit_rate = 1000000,
        .stop_bits = 0,
        .parity = 0,
        .data_bits = 8
    };
    tuh_cdc_set_line_coding(idx, &lineCoding, nullptr, 0);
    tuh_cdc_set_control_line_state(idx, CDC_CONTROL_LINE_STATE_DTR | CDC_CONTROL_LINE_STATE_RTS, nullptr, 0);
}

void UsbHost::_onCdcUnmount(uint8_t idx) {
    Serial.printf("[UsbHost] CDC unmounted: idx=%d\n", idx);
}

void UsbHost::_onCdcRx(uint8_t idx) {
    if (_cdcRxCallback) {
        uint8_t buf[64];
        uint32_t count = tuh_cdc_read(idx, buf, sizeof(buf));
        if (count > 0) {
            for (int i = 0; i < _cdcDeviceCount; i++) {
                if (_cdcDevices[i].itf_num == idx) {
                    _cdcRxCallback(_cdcDevices[i].dev_addr, buf, count);
                    _stats.bytes_received += count;
                    break;
                }
            }
        }
    }
}

// ============================================================================
// TinyUSB Callbacks
// ============================================================================

void tuh_mount_cb(uint8_t dev_addr) {
    if (g_usbHost) {
        g_usbHost->_onDeviceMount(dev_addr);
    }
}

void tuh_umount_cb(uint8_t dev_addr) {
    if (g_usbHost) {
        g_usbHost->_onDeviceUnmount(dev_addr);
    }
}

void tuh_cdc_mount_cb(uint8_t idx) {
    if (g_usbHost) {
        g_usbHost->_onCdcMount(idx);
    }
}

void tuh_cdc_umount_cb(uint8_t idx) {
    if (g_usbHost) {
        g_usbHost->_onCdcUnmount(idx);
    }
}

void tuh_cdc_rx_cb(uint8_t idx) {
    if (g_usbHost) {
        g_usbHost->_onCdcRx(idx);
    }
}

void tuh_cdc_tx_complete_cb(uint8_t idx) {
    (void)idx;
}

#endif // !SCALEFX_SERVER
