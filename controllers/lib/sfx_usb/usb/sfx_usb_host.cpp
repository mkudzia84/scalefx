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
    // Only match a still-connected slot — a torn-down (connected=false) slot
    // retains its last dev_addr until reused, and must never alias a live device.
    for (int i = 0; i < cdcDeviceCount; i++) {
        if (cdcDevices[i].connected && cdcDevices[i].dev_addr == devAddr) return i;
    }
    return -1;
}

int UsbHostState::addCdcDevice(uint8_t devAddr, uint8_t itfNum, uint16_t vid, uint16_t pid) {
    // Slot-stable allocation (do NOT compact on removal — see removeCdcDevice).
    // Reuse the first torn-down (connected=false) slot so a cached device index
    // held by a surviving BusClient never silently re-points at a different
    // device; only grow cdcDeviceCount when no gap is free. A device index, once
    // handed out, addresses the SAME slot until that device unmounts.
    int idx = -1;
    int count = cdcDeviceCount.load(std::memory_order_relaxed);
    for (int i = 0; i < count; i++) {
        if (!cdcDevices[i].connected) { idx = i; break; }
    }
    if (idx < 0) {
        if (count >= USB_HOST_MAX_CDC_DEVICES) {
            SFX_LOG_WARN("[UsbHost] Max CDC devices (%d) reached", USB_HOST_MAX_CDC_DEVICES);
            return -1;
        }
        idx = count;
    }
    CdcDeviceInfo& dev = cdcDevices[idx];
    dev.dev_addr = devAddr;
    dev.itf_num = itfNum;
    dev.vid = vid;
    dev.pid = pid;
    dev.state = UsbDeviceState::Connected;
    dev.connected = true;   // publish LAST: cdcRead/cdcWrite gate on this
    // Bump the high-water count only when appending a fresh slot; never lower it
    // on removal, so existing indices stay valid (release-publishes the entry).
    if (idx == count) cdcDeviceCount.store(count + 1, std::memory_order_release);
    stats.devices_mounted++;
    SFX_LOG_INFO("[UsbHost] CDC device added: idx=%d addr=%d VID=%04X PID=%04X",
                 idx, devAddr, vid, pid);
    return idx;
}

void UsbHostState::removeCdcDevice(uint8_t devAddr) {
    int idx = findDeviceByAddr(devAddr);
    if (idx < 0) return;
    SFX_LOG_INFO("[UsbHost] CDC device removed: idx=%d addr=%d", idx, devAddr);
    // Do NOT compact — compaction would shift every higher index down and silently
    // re-point a surviving expander's cached device index at the wrong slot (dead
    // wire). Mark the slot torn-down in place; its index is left free for reuse by
    // the next addCdcDevice. cdcDeviceCount is a high-water mark and never shrinks.
    cdcDevices[idx].connected = false;
    cdcDevices[idx].state = UsbDeviceState::Disconnected;
    stats.devices_unmounted++;
}

#endif // !SCALEFX_SERVER
