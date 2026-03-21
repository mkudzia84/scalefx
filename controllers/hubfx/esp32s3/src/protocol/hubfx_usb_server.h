/*
 * HubFX USB Server — USB Device Listing Handler
 *
 * Handles USB host diagnostic commands:
 *   - USB_DEVICES_REQ  (0xA7) → USB_DEVICES_RESP (0xA8)
 *   - USB_RESET_BUS    (0xAD) → ACK (power-cycles root port)
 *
 * Reports USB Host status including all connected CDC devices
 * with their VID, PID, connection state, and device address.
 *
 * This is an ESP32-S3 controller-local class (not shared library)
 * because it depends on UsbHost which is platform-specific.
 */

#ifndef HUBFX_USB_SERVER_H
#define HUBFX_USB_SERVER_H

#include <serial/serial.h>
#include <serial/hubfx/hubfx.h>
#include <usb/sfx_usb_host.h>
#include <usb/usb_registry.h>

class HubFxUsbServer : public BusServer {
public:
    HubFxUsbServer() = default;

    const char* handlerName() const override { return "HubFxUsbServer"; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type,
                                           const uint8_t* payload,
                                           size_t len) override;

    uint8_t moduleRangeLow()  const override { return HubFxPacket::USB_DEVICES_REQ; }
    uint8_t moduleRangeHigh() const override { return HubFxPacket::USB_RESET_BUS; }

    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

private:
    void handleUsbDevicesReq();
    void handleUsbResetBus();
};

#endif // HUBFX_USB_SERVER_H
