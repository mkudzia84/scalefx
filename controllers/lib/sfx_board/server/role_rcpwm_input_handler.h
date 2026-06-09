/*
 * RcPwmInputHandler — the RcPwmInput role (single-channel PPM pulse capture
 * on an Input port).  Owns attach (broadcast-callback wiring) + the value
 * query + broadcast-rate command.
 */

#ifndef SFX_ROLE_RCPWM_INPUT_HANDLER_H
#define SFX_ROLE_RCPWM_INPUT_HANDLER_H

#include <cstdint>
#include <cstddef>

#include "port_registry.h"

namespace sfx_core {

class BoardServerBase;
class RoleEventEmitter;

class RcPwmInputHandler {
public:
    void bind(PortRegistryBase* reg, BoardServerBase* ctx, RoleEventEmitter* emit) {
        _reg = reg; _ctx = ctx; _emit = emit;
    }

    bool attach(InputBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);

    void handleGetValueReq   (const uint8_t* p, size_t len);
    void handleSetBroadcastHz(const uint8_t* p, size_t len);

private:
    PortRegistryBase* _reg  = nullptr;
    BoardServerBase*  _ctx  = nullptr;
    RoleEventEmitter* _emit = nullptr;
};

}  // namespace sfx_core

#endif  // SFX_ROLE_RCPWM_INPUT_HANDLER_H
