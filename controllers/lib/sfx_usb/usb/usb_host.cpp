/*
 * USB Host — Base Class Implementation + Platform Factory
 *
 * Shared helpers (findDeviceByAddr, addCdcDevice, removeCdcDevice,
 * getCdcDevice) and the platform factory singleton.
 *
 * Platform-specific implementations live in:
 *   - pico_usb_host.cpp (PIO-USB + TinyUSB)
 *   - esp_usb_host.cpp  (HW USB-OTG + ESP-IDF)
 */

#include "usb_host.h"

// USB Host is client-only (HubFX). Server controllers skip this entirely.
#ifndef SCALEFX_SERVER

#include "platform/diag_log.h"

// Include the platform-specific subclass header for the factory
#if SFX_PLATFORM_PICO
#include "pico_usb_host.h"
#elif SFX_PLATFORM_ESP32
#include "esp_usb_host.h"
#endif

// ============================================================================
// Platform Factory Singleton
// ============================================================================

UsbHost& UsbHost::instance() {
#if SFX_PLATFORM_PICO
    static PicoUsbHost inst;
#elif SFX_PLATFORM_ESP32
    static EspUsbHost inst;
#else
    #error "Unsupported platform for UsbHost — need SFX_PLATFORM_PICO or SFX_PLATFORM_ESP32"
#endif
    return inst;
}

// ============================================================================
// Shared Base Class Implementation
// ============================================================================

const CdcDeviceInfo* UsbHost::getCdcDevice(int devIndex) const {
    if (devIndex < 0 || devIndex >= _cdcDeviceCount) return nullptr;
    return &_cdcDevices[devIndex];
}

int UsbHost::findDeviceByAddr(uint8_t devAddr) const {
    for (int i = 0; i < _cdcDeviceCount; i++) {
        if (_cdcDevices[i].dev_addr == devAddr) return i;
    }
    return -1;
}

int UsbHost::addCdcDevice(uint8_t devAddr, uint8_t itfNum, uint16_t vid, uint16_t pid) {
    if (_cdcDeviceCount >= USB_HOST_MAX_CDC_DEVICES) {
        SFX_LOG_WARN("[UsbHost] Max CDC devices (%d) reached", USB_HOST_MAX_CDC_DEVICES);
        return -1;
    }
    int idx = _cdcDeviceCount;
    CdcDeviceInfo& dev = _cdcDevices[idx];
    dev.connected = true;
    dev.dev_addr = devAddr;
    dev.itf_num = itfNum;
    dev.vid = vid;
    dev.pid = pid;
    dev.state = UsbDeviceState::Connected;
    _cdcDeviceCount++;
    _stats.devices_mounted++;
    SFX_LOG_INFO("[UsbHost] CDC device added: idx=%d addr=%d VID=%04X PID=%04X",
                 idx, devAddr, vid, pid);
    return idx;
}

void UsbHost::removeCdcDevice(uint8_t devAddr) {
    int idx = findDeviceByAddr(devAddr);
    if (idx < 0) return;
    SFX_LOG_INFO("[UsbHost] CDC device removed: idx=%d addr=%d", idx, devAddr);
    for (int i = idx; i < _cdcDeviceCount - 1; i++) {
        _cdcDevices[i] = _cdcDevices[i + 1];
    }
    _cdcDeviceCount--;
    _stats.devices_unmounted++;
}

#endif // !SCALEFX_SERVER
