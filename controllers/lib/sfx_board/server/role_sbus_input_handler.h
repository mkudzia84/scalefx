/*
 * SbusInputHandler — the SbusInput role (inverted-UART SBUS frame on an Input
 * port).  Owns attach (broadcast-callback wiring) + the frame query +
 * broadcast-rate command.
 */

#ifndef SFX_ROLE_SBUS_INPUT_HANDLER_H
#define SFX_ROLE_SBUS_INPUT_HANDLER_H

#include <cstdint>
#include <cstddef>

#include "port_registry.h"

namespace sfx_core {

class BoardServerBase;
class RoleEventEmitter;

class SbusInputHandler {
public:
    void bind(PortRegistryBase* reg, BoardServerBase* ctx, RoleEventEmitter* emit) {
        _reg = reg; _ctx = ctx; _emit = emit;
    }

    bool attach(InputBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);

    void handleGetFrameReq   (const uint8_t* p, size_t len);
    void handleSetBroadcastHz(const uint8_t* p, size_t len);

private:
    PortRegistryBase* _reg  = nullptr;
    BoardServerBase*  _ctx  = nullptr;
    RoleEventEmitter* _emit = nullptr;
};

}  // namespace sfx_core

#endif  // SFX_ROLE_SBUS_INPUT_HANDLER_H
