/*
 * InputRoleHandler — the input-port role families (Input ports).
 *
 * One handler for every inbound RC modality the input port can carry:
 *   RcPwmInput   — single-channel PPM pulse capture
 *   SbusInput    — inverted-UART SBUS frame
 *   JetiExInput  — Jeti EX Bus (starts the board-unique JetiExpander on ESP32)
 *   JetiExTelemetry — downstream ESC telemetry monitor (paired marker role)
 *
 * Owns each attach (broadcast-callback wiring + the Jeti IN_1/IN_2 pairing on
 * ESP32) plus the value/frame query + per-role broadcast-rate commands.  All
 * three live-channel feeds drive the on-board InputDispatcher locally and
 * broadcast to a listening host through the RoleEventEmitter.
 */

#ifndef SFX_ROLE_INPUT_HANDLER_H
#define SFX_ROLE_INPUT_HANDLER_H

#include <cstdint>
#include <cstddef>

#include "port_registry.h"

namespace sfx_core {

class BoardServerBase;
class RoleEventEmitter;

class InputRoleHandler {
public:
    void bind(PortRegistryBase* reg, BoardServerBase* ctx, RoleEventEmitter* emit) {
        _reg = reg; _ctx = ctx; _emit = emit;
    }

    // ── Attach (build role + wire broadcast callbacks) ───────────────────
    bool attachRcPwm     (InputBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachSbus      (InputBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachJetiEx    (InputBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachJetiExTelemetry(InputBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);

    // ── Command surface ──────────────────────────────────────────────────
    void handleRcInGetValueReq   (const uint8_t* p, size_t len);
    void handleRcInSetBroadcastHz(const uint8_t* p, size_t len);
    void handleSbusGetFrameReq   (const uint8_t* p, size_t len);
    void handleSbusSetBroadcastHz(const uint8_t* p, size_t len);
    void handleJetiExGetFrameReq   (const uint8_t* p, size_t len);
    void handleJetiExSetBroadcastHz(const uint8_t* p, size_t len);

private:
    PortRegistryBase* _reg  = nullptr;
    BoardServerBase*  _ctx  = nullptr;
    RoleEventEmitter* _emit = nullptr;
};

}  // namespace sfx_core

#endif  // SFX_ROLE_INPUT_HANDLER_H
