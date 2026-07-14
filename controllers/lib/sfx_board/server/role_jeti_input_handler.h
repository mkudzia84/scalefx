/*
 * JetiInputHandler — the Jeti EX Bus roles on an Input port.
 *
 * Covers BOTH co-managed Jeti roles, because one board-unique JetiExpander
 * owns both links and the IN_1 (input) attach pairs the IN_2 (telemetry)
 * marker:
 *   JetiExInput     — the Rx-side EX Bus channel stream (starts the expander)
 *   (the downstream link marker is now EscTelemetryRole protocol=jeti-exbus)
 *
 * Owns each attach + the channel-frame query + the broadcast-rate command.
 */

#ifndef SFX_ROLE_JETI_INPUT_HANDLER_H
#define SFX_ROLE_JETI_INPUT_HANDLER_H

#include <cstdint>
#include <cstddef>

#include "port_registry.h"

namespace sfx_core {

class BoardServerBase;
class RoleEventEmitter;

class JetiInputHandler {
public:
    void bind(PortRegistryBase* reg, BoardServerBase* ctx, RoleEventEmitter* emit) {
        _reg = reg; _ctx = ctx; _emit = emit;
    }

    bool attachInput    (InputBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);

    void handleGetFrameReq   (const uint8_t* p, size_t len);
    void handleSetBroadcastHz(const uint8_t* p, size_t len);

private:
    PortRegistryBase* _reg  = nullptr;
    BoardServerBase*  _ctx  = nullptr;
    RoleEventEmitter* _emit = nullptr;
};

}  // namespace sfx_core

#endif  // SFX_ROLE_JETI_INPUT_HANDLER_H
