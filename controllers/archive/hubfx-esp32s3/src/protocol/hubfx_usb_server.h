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
#include <serial/core/system_service.h>     // SystemServicePolicy + ServiceContext
#include <usb/sfx_usb_host.h>

class UsbHostServicePolicy {
public:
    /// USB-host capability bit (and EXPANDER_BUS implicitly enabled by
    /// presence of the HubFX USB-host stack on master boards).
    static constexpr uint32_t kCapabilityBits = CoreCapability::USB_HOST;

    UsbHostServicePolicy() = default;

    // ── SystemServicePolicy surface ───────────────────────────────────

    bool begin(sfx_core::ServiceContext* ctx) {
        _ctx = ctx;
        return _ctx != nullptr;
    }

    bool ownsType(uint8_t type) const {
        return type == HubFxPacket::USB_DEVICES_REQ
            || type == HubFxPacket::USB_DEVICES_RESP
            || type == HubFxPacket::USB_RESET_BUS;
    }

    CommandHandleResult handle(uint8_t type, const uint8_t* payload, size_t len);

    void update() {}

    const char* getErrorMessage(uint8_t code) const {
        return HubFxError::getMessage(code);
    }

protected:
    // ServiceContext wire-helper wrappers — handler bodies call
    // sendAck()/sendNack()/sendRawPacket() through the policy's own _ctx.
    int     sendAck()                                                  { return _ctx->sendAck(); }
    int     sendNack(uint8_t errorCode, const char* reason = nullptr)  { return _ctx->sendNack(errorCode, reason); }
    int     sendRawPacket(uint8_t t, uint8_t tag, const uint8_t* p = nullptr, size_t l = 0)
                                                                       { return _ctx->sendRawPacket(t, tag, p, l); }
    uint8_t currentTag() const                                         { return _ctx->currentTag(); }

private:
    sfx_core::ServiceContext* _ctx = nullptr;

    void handleUsbDevicesReq();
    void handleUsbResetBus();
};

/// @deprecated Alias for in-flight callers.
using HubFxUsbServer = UsbHostServicePolicy;

#endif // HUBFX_USB_SERVER_H
