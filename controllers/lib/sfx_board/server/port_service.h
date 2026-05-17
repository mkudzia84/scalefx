/*
 * PortServicePolicy — system-service policy for the port layer.
 *
 * Handles the 0x10..0x3F generic-expander slice:
 *   - 0x10..0x17  enumeration (`PORT_LIST_REQ` → `PORT_LIST_RESP`)
 *   - 0x18..0x1F  servo port raw access
 *   - 0x20..0x2F  PWM port raw access
 *   - 0x30..0x37  H-bridge port raw access
 *   - 0x38..0x3F  Input port raw access (PULSE / mode query)
 *
 * Holds a `PortRegistryBase*` bound by `BoardOf<TBoard>` before
 * `begin()` fires.  Raw access is **rejected with `PORT_HAS_ROLE`** if
 * the addressed port currently has a role attached — this avoids races
 * between an LED queue and a master poking duty bytes.
 */

#ifndef SFX_PORT_SERVICE_H
#define SFX_PORT_SERVICE_H

#include <cstdint>
#include <cstddef>

#include <serial/core/core.h>     // CommandHandleResult, CorePacket, SerialError
#include <serial/ports.h>          // PortPacket, PortKind, PortError, ...
#include <serial/wire.h>           // SfxWire::putU16LE / getU16LE / ...

#include "board_server.h"
#include "port_registry.h"

namespace sfx_core {

class PortServicePolicy {
public:
    /// Port enumeration adds no capability bit — it's intrinsic to any
    /// board firmware built on BoardOf<>.
    static constexpr uint32_t kCapabilityBits = 0u;

    PortServicePolicy() = default;

    /// Bound by `BoardOf<TBoard>` post-construction, before `begin()`.
    void bindRegistry(PortRegistryBase* reg) { _reg = reg; }

    // ── SystemServicePolicy surface ───────────────────────────────────

    bool begin(BoardServerBase* ctx) {
        _ctx = ctx;
        return _ctx != nullptr;
    }

    bool ownsType(uint8_t type) const {
        return (type >= 0x10 && type <= 0x3F);
    }

    CommandHandleResult handle(uint8_t type, const uint8_t* payload, size_t len);

    void update() {}

    const char* getErrorMessage(uint8_t code) const {
        return PortError::getMessage(code);
    }

private:
    void handlePortListReq();

    // Servo raw access
    void handleServoSetUs (const uint8_t* p, size_t len);
    void handleServoReadUs(const uint8_t* p, size_t len);

    // PWM raw access
    void handlePwmSetDuty  (const uint8_t* p, size_t len);
    void handlePwmSetFreq  (const uint8_t* p, size_t len);
    void handlePwmReadSense(const uint8_t* p, size_t len);

    // H-bridge raw access
    void handleHBridgeSetSigned(const uint8_t* p, size_t len);
    void handleHBridgeBrake    (const uint8_t* p, size_t len);
    void handleHBridgeCoast    (const uint8_t* p, size_t len);
    void handleHBridgeReadSense(const uint8_t* p, size_t len);

    // Input raw access
    void handleInputReadPulse(const uint8_t* p, size_t len);
    void handleInputGetMode  (const uint8_t* p, size_t len);

    BoardServerBase*  _ctx = nullptr;
    PortRegistryBase* _reg = nullptr;
};

}  // namespace sfx_core

#endif  // SFX_PORT_SERVICE_H
