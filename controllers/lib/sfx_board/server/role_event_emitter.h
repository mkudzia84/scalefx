/*
 * RoleEventEmitter — the role layer's async telemetry-out path.
 *
 * Owns the wire format for EVERY async role event (ROLE_ATTACHED/DETACHED,
 * LED_QUEUE_DONE, SERVO_TARGET_REACHED / MOTION_DONE, motor + bi-motor stall
 * + endstop, and the PPM / SBUS / Jeti channel-frame broadcasts) AND the
 * single master-internal listener slot.
 *
 * Every emit*() does two things: writes the COBS async packet (gated on a
 * listening host for the high-rate channel broadcasts) AND forwards the same
 * bytes through fireLocalAsync() so `TopologyServicePolicy` can hand hub-local
 * role events to subscribing effects without re-parsing the output buffer.
 * The listener is installed once by TopologyService via setListener().
 *
 * One class, one responsibility: it neither looks up bindings nor decodes
 * commands — the command handlers call it; it only serialises + fans out.
 */

#ifndef SFX_ROLE_EVENT_EMITTER_H
#define SFX_ROLE_EVENT_EMITTER_H

#include <cstdint>
#include <cstddef>

#include "port_registry.h"   // RcPwmInputRole / SbusInputRole / JetiExInputRole (frame-broadcast refs)

namespace sfx_core {

class BoardServerBase;

class RoleEventEmitter {
public:
    /// Master-internal hook fired ALONGSIDE the wire packet whenever a role
    /// emits an async event.  Single slot — only TopologyService installs it.
    using LocalAsyncCallback = void (*)(void* ctx, uint8_t innerType,
                                        const uint8_t* payload, size_t len);

    void bind(BoardServerBase* ctx) { _ctx = ctx; }
    void setListener(LocalAsyncCallback fn, void* ctx) { _fn = fn; _listenerCtx = ctx; }

    // ── Discrete role events ─────────────────────────────────────────────
    void emitRoleAttached      (uint8_t portKind, uint8_t portIdx, uint8_t roleKind);
    void emitRoleDetached      (uint8_t portKind, uint8_t portIdx);
    void emitLedQueueDone      (uint8_t portIdx);
    void emitServoTargetReached(uint8_t portIdx, uint16_t pos_us);
    void emitServoMotionDone   (uint8_t portIdx);
    void emitMotorStallEvent   (uint8_t portIdx, uint16_t peak_mA, uint16_t duration_ms);
    void emitBiMotorStallEvent (uint8_t portIdx, uint16_t peak_mA, uint16_t duration_ms);
    void emitBiMotorEndstopResult(uint8_t portIdx, uint8_t outcome,
                                  uint16_t travel_ms, uint16_t peak_mA, uint8_t position);

    // ── High-rate channel-frame broadcasts (gated on a listening host) ───
    void emitPpmFrameBroadcast   (uint8_t portIdx, const RcPwmInputRole& role);
    void emitSbusFrameBroadcast  (uint8_t portIdx, const SbusInputRole& role);
    void emitJetiExFrameBroadcast(uint8_t portIdx, const JetiExInputRole& role);

    /// Forwarded by every emit*() so master-internal listeners can react
    /// without parsing the wire stream.
    void fireLocalAsync(uint8_t innerType, const uint8_t* p, size_t len) {
        if (_fn) _fn(_listenerCtx, innerType, p, len);
    }

private:
    BoardServerBase*   _ctx         = nullptr;
    LocalAsyncCallback _fn          = nullptr;
    void*              _listenerCtx = nullptr;
};

}  // namespace sfx_core

#endif  // SFX_ROLE_EVENT_EMITTER_H
