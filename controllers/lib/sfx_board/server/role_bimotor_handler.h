/*
 * BiMotorRoleHandler — the bidirectional BiDcMotor role family (HBridge ports).
 *
 * Owns attach (Fixed / LiveRatio stall-guard config + Strategy A position
 * restore + stall / endstop callbacks), the signed-drive / brake / coast
 * surface, endstop seek + move-to-end, the live stall-guard retune, and
 * status query.
 */

#ifndef SFX_ROLE_BIMOTOR_HANDLER_H
#define SFX_ROLE_BIMOTOR_HANDLER_H

#include <cstdint>
#include <cstddef>

#include "port_registry.h"

namespace sfx_core {

class BoardServerBase;
class RoleEventEmitter;

class BiMotorRoleHandler {
public:
    void bind(PortRegistryBase* reg, BoardServerBase* ctx, RoleEventEmitter* emit) {
        _reg = reg; _ctx = ctx; _emit = emit;
    }

    bool attach(HBridgeBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);

    // ── Command surface (0x68..0x6F + spilled slots) ─────────────────────
    void handleSetSigned   (const uint8_t* p, size_t len);
    void handleBrake       (const uint8_t* p, size_t len);
    void handleCoast       (const uint8_t* p, size_t len);
    void handleGetStatus   (const uint8_t* p, size_t len);
    void handleSeekEndstop (const uint8_t* p, size_t len);
    void handleMoveToEnd   (const uint8_t* p, size_t len);   // 0x5F (Strategy A)
    void handleSetGuard    (const uint8_t* p, size_t len);   // 0x77 (live retune)

private:
    PortRegistryBase* _reg  = nullptr;
    BoardServerBase*  _ctx  = nullptr;
    RoleEventEmitter* _emit = nullptr;
};

}  // namespace sfx_core

#endif  // SFX_ROLE_BIMOTOR_HANDLER_H
