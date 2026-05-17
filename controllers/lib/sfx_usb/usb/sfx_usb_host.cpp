/*
 * UsbHostState — Shared Device Tracking Methods
 *
 * Platform-independent helpers for CDC device management.
 * Used by both PicoUsbHost and EspUsbHost via composition.
 */

#include "sfx_usb_host.h"

// USB Host is client-only (HubFX). Server controllers skip this entirely.
#ifndef SCALEFX_SERVER

#include "serial/diag_log.h"

// ============================================================================
// Device Tracking Helpers
// ============================================================================

const CdcDeviceInfo* UsbHostState::getCdcDevice(int devIndex) const {
    if (devIndex < 0 || devIndex >= cdcDeviceCount) return nullptr;
    return &cdcDevices[devIndex];
}

int UsbHostState::findDeviceByAddr(uint8_t devAddr) const {
    for (int i = 0; i < cdcDeviceCount; i++) {
        if (cdcDevices[i].dev_addr == devAddr) return i;
    }
    return -1;
}

int UsbHostState::addCdcDevice(uint8_t devAddr, uint8_t itfNum, uint16_t vid, uint16_t pid) {
    if (cdcDeviceCount >= USB_HOST_MAX_CDC_DEVICES) {
        SFX_LOG_WARN("[UsbHost] Max CDC devices (%d) reached", USB_HOST_MAX_CDC_DEVICES);
        return -1;
    }
    int idx = cdcDeviceCount;
    CdcDeviceInfo& dev = cdcDevices[idx];
    dev.connected = true;
    dev.dev_addr = devAddr;
    dev.itf_num = itfNum;
    dev.vid = vid;
    dev.pid = pid;
    dev.state = UsbDeviceState::Connected;
    cdcDeviceCount++;
    stats.devices_mounted++;
    SFX_LOG_INFO("[UsbHost] CDC device added: idx=%d addr=%d VID=%04X PID=%04X",
                 idx, devAddr, vid, pid);
    return idx;
}

void UsbHostState::removeCdcDevice(uint8_t devAddr) {
    int idx = findDeviceByAddr(devAddr);
    if (idx < 0) return;
    SFX_LOG_INFO("[UsbHost] CDC device removed: idx=%d addr=%d", idx, devAddr);
    for (int i = idx; i < cdcDeviceCount - 1; i++) {
        cdcDevices[i] = cdcDevices[i + 1];
    }
    cdcDeviceCount--;
    stats.devices_unmounted++;
}

#endif // !SCALEFX_SERVER
