/*
 * ServoRoleHandler — the ServoActuator role family (Servo ports).
 *
 * Owns the full lifecycle of a servo role: build-from-config (attach), the
 * positioning / recoil / profile command surface, and the batched
 * SERVO_MOTION_UPDATE telemetry broadcast (its rate + scheduling live here,
 * ticked once per pass from RoleServicePolicy::update()).
 *
 * Bound with the registry + board context + event emitter; holds no role
 * state of its own beyond the broadcast cadence — the role instances live in
 * the registry's per-port variant slots.
 */

#ifndef SFX_ROLE_SERVO_HANDLER_H
#define SFX_ROLE_SERVO_HANDLER_H

#include <cstdint>
#include <cstddef>

#include "port_registry.h"

namespace sfx_core {

class BoardServerBase;
class RoleEventEmitter;

class ServoRoleHandler {
public:
    void bind(PortRegistryBase* reg, BoardServerBase* ctx, RoleEventEmitter* emit) {
        _reg = reg; _ctx = ctx; _emit = emit;
    }

    /// Build a ServoActuatorRole into the binding from the attach config
    /// bytes (Rule 42 profile travels in the payload).  No wire ACK.
    bool attach(ServoBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);

    // ── Command surface (0x48..0x4F) ─────────────────────────────────────
    void handleSetTarget    (const uint8_t* p, size_t len);
    void handleSetPosNorm   (const uint8_t* p, size_t len);
    void handleRecoil       (const uint8_t* p, size_t len);
    void handleGetStatusReq (const uint8_t* p, size_t len);
    void handleSetProfile   (const uint8_t* p, size_t len);
    void handleGetProfileReq(const uint8_t* p, size_t len);
    void handleSetBroadcastHz(const uint8_t* p, size_t len);

    /// Scheduling + emit of the batched SERVO_MOTION_UPDATE snapshot.  Called
    /// once per pass from RoleServicePolicy::update() with the shared clock.
    void maybeBroadcast(uint32_t now);

private:
    void emitBroadcast(uint32_t now);

    PortRegistryBase* _reg  = nullptr;
    BoardServerBase*  _ctx  = nullptr;
    RoleEventEmitter* _emit = nullptr;

    // Generic servo telemetry broadcast (SERVO_MOTION_UPDATE).  Global rate set
    // by the host via SERVO_SET_BROADCAST_HZ; 0 = off.  Gated additionally on
    // hostVerboseActive() at emit time.  Naturally silent during uploads
    // because update() isn't ticked while the loop is upload-exclusive.
    uint8_t  _broadcastHz     = 0;
    uint32_t _broadcastNextMs = 0;
    static constexpr uint8_t kBroadcastMaxHz = 50;  // clamp host requests
};

}  // namespace sfx_core

#endif  // SFX_ROLE_SERVO_HANDLER_H
