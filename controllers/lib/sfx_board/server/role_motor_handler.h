/*
 * DcMotorRoleHandler — the uni-directional DcMotor role family (Pwm ports).
 *
 * Owns attach (stall-guard + element scaling + rail context), the duty /
 * brake / pct drive surface, the element-config live retune, and status /
 * element queries.
 */

#ifndef SFX_ROLE_MOTOR_HANDLER_H
#define SFX_ROLE_MOTOR_HANDLER_H

#include <cstdint>
#include <cstddef>

#include "port_registry.h"

namespace sfx_core {

class BoardServerBase;
class RoleEventEmitter;

class DcMotorRoleHandler {
public:
    void bind(PortRegistryBase* reg, BoardServerBase* ctx, RoleEventEmitter* emit) {
        _reg = reg; _ctx = ctx; _emit = emit;
    }

    bool attach(PwmBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);

    // ── Command surface (0x60..0x67) ─────────────────────────────────────
    void handleSetDuty      (const uint8_t* p, size_t len);
    void handleBrake        (const uint8_t* p, size_t len);
    void handleGetStatusReq (const uint8_t* p, size_t len);
    void handleSetElement   (const uint8_t* p, size_t len);
    void handleGetElementReq(const uint8_t* p, size_t len);
    void handleSetPct       (const uint8_t* p, size_t len);

private:
    PortRegistryBase* _reg  = nullptr;
    BoardServerBase*  _ctx  = nullptr;
    RoleEventEmitter* _emit = nullptr;
};

}  // namespace sfx_core

#endif  // SFX_ROLE_MOTOR_HANDLER_H
