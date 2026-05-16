/*
 * HubFX USB Server — Implementation
 *
 * USB_DEVICES_RESP wire format:
 *   [initialized:u8][taskRunning:u8][backendLen:u8][backend:str]
 *   [deviceCount:u8]
 *   per-device: [addr:u8][vid:u16LE][pid:u16LE][state:u8][slaveType:u8]
 *
 * slaveType byte is reserved as 0 — slave-board management was removed
 * 2026-05-16 and will be rebuilt against the generic-expander layer
 * (instructions/15-GENERIC-EXPANDER-REFACTOR.md). When it lands this
 * byte will carry the ExpanderType instead of SlaveType.
 */

#include "hubfx_usb_server.h"
#include <platform/diag_log.h>

#define USB_LOG(fmt, ...) SFX_LOG_DEBUG("[UsbSrv] " fmt, ##__VA_ARGS__)


CommandHandleResult HubFxUsbServer::handleModulePacket(
        uint8_t type, const uint8_t* payload, size_t len) {

    switch (type) {
        case HubFxPacket::USB_DEVICES_REQ:
            handleUsbDevicesReq();
            return CommandHandleResult::Handled;

        case HubFxPacket::USB_RESET_BUS:
            handleUsbResetBus();
            return CommandHandleResult::Handled;

        default:
            return CommandHandleResult::NotMyCommand;
    }
}


void HubFxUsbServer::handleUsbDevicesReq() {
    UsbHost& usb = UsbHost::instance();

    // Build response payload
    // Max: 3 (header) + 32 (backend name) + 1 (count) + 4 * 7 (devices) = 64
    uint8_t resp[128];
    size_t pos = 0;

    resp[pos++] = usb.isInitialized() ? 1 : 0;
    resp[pos++] = usb.isTaskRunning() ? 1 : 0;

    // Backend name (length-prefixed string)
    const char* backend = usb.backendName();
    uint8_t backendLen = backend ? (uint8_t)strlen(backend) : 0;
    if (backendLen > 32) backendLen = 32;
    resp[pos++] = backendLen;
    if (backendLen > 0) {
        memcpy(&resp[pos], backend, backendLen);
        pos += backendLen;
    }

    // Device list
    int count = usb.cdcDeviceCount();
    if (count > USB_HOST_MAX_CDC_DEVICES) count = USB_HOST_MAX_CDC_DEVICES;
    resp[pos++] = (uint8_t)count;

    for (int i = 0; i < count; i++) {
        const CdcDeviceInfo* dev = usb.getCdcDevice(i);
        if (!dev) {
            // Shouldn't happen — fill with zeroes
            memset(&resp[pos], 0, 7);
            pos += 7;
            continue;
        }

        resp[pos++] = dev->dev_addr;
        CoreProtocol::putU16LE(&resp[pos], dev->vid);   pos += 2;
        CoreProtocol::putU16LE(&resp[pos], dev->pid);   pos += 2;
        resp[pos++] = (uint8_t)dev->state;

        // Reserved — was the slave/expander type byte. Slave management
        // was removed 2026-05-16; the new generic-expander layer will
        // repopulate this with ExpanderType.
        resp[pos++] = 0;
    }

    USB_LOG("USB_DEVICES_RESP: init=%d task=%d backend=%s devices=%d",
            usb.isInitialized(), usb.isTaskRunning(), backend, count);

    sendRawPacket(HubFxPacket::USB_DEVICES_RESP, currentTag(), resp, pos);
}


void HubFxUsbServer::handleUsbResetBus() {
    UsbHost& usb = UsbHost::instance();

    if (!usb.isInitialized()) {
        sendNack(SerialError::NOT_INITIALIZED);
        return;
    }

    USB_LOG("USB_RESET_BUS: power-cycling root port");
    usb.resetBus();
    sendAck();
}
