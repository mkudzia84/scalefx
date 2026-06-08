/*
 * LedRoleHandler — the LedAnimator role family (Pwm ports).
 *
 * Owns attach (master-brightness seed + queue-done callback wiring) plus the
 * LED program command surface: queue-load, start/stop, brightness, status.
 */

#ifndef SFX_ROLE_LED_HANDLER_H
#define SFX_ROLE_LED_HANDLER_H

#include <cstdint>
#include <cstddef>

#include "port_registry.h"

namespace sfx_core {

class BoardServerBase;
class RoleEventEmitter;

class LedRoleHandler {
public:
    void bind(PortRegistryBase* reg, BoardServerBase* ctx, RoleEventEmitter* emit) {
        _reg = reg; _ctx = ctx; _emit = emit;
    }

    bool attach(PwmBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);

    // ── Command surface (0x58..0x5F) ─────────────────────────────────────
    void handleQueueLoad     (const uint8_t* p, size_t len);
    void handleStart         (const uint8_t* p, size_t len);
    void handleStop          (const uint8_t* p, size_t len);
    void handleSetBrightness (const uint8_t* p, size_t len);
    void handleGetStatusReq  (const uint8_t* p, size_t len);

private:
    PortRegistryBase* _reg  = nullptr;
    BoardServerBase*  _ctx  = nullptr;
    RoleEventEmitter* _emit = nullptr;
};

}  // namespace sfx_core

#endif  // SFX_ROLE_LED_HANDLER_H
