/*
 * HeaterRoleHandler — the Heater role family (Pwm ports).
 *
 * Owns attach (optional temp-sensor closed-loop bind + rail/element context),
 * the target-temperature command, the element-config live retune, and
 * status / element queries.
 */

#ifndef SFX_ROLE_HEATER_HANDLER_H
#define SFX_ROLE_HEATER_HANDLER_H

#include <cstdint>
#include <cstddef>

#include "port_registry.h"

namespace sfx_core {

class BoardServerBase;
class RoleEventEmitter;

class HeaterRoleHandler {
public:
    void bind(PortRegistryBase* reg, BoardServerBase* ctx, RoleEventEmitter* emit) {
        _reg = reg; _ctx = ctx; _emit = emit;
    }

    bool attach(PwmBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);

    // ── Command surface (0x70..0x77) ─────────────────────────────────────
    void handleSetTarget    (const uint8_t* p, size_t len);
    void handleGetStatus    (const uint8_t* p, size_t len);
    void handleSetElement   (const uint8_t* p, size_t len);
    void handleGetElementReq(const uint8_t* p, size_t len);

private:
    PortRegistryBase* _reg  = nullptr;
    BoardServerBase*  _ctx  = nullptr;
    RoleEventEmitter* _emit = nullptr;
};

}  // namespace sfx_core

#endif  // SFX_ROLE_HEATER_HANDLER_H
