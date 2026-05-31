/*
 * RoleServicePolicy — system-service policy for the role layer.
 *
 * Handles the 0x40..0x7F generic-expander slice:
 *   - 0x40..0x47  attach / detach / enumeration
 *   - 0x48..0x4F  servo actuator role commands
 *   - 0x50..0x57  RC PWM input role commands (single-channel pulse on InputPort)
 *   - 0x58..0x5F  LED animator role commands
 *   - 0x60..0x67  DC motor role commands
 *   - 0x68..0x6F  Bi-directional DC motor role commands
 *   - 0x70..0x77  Heater role commands
 *   - 0x78..0x7B  SBUS input role commands
 *   - 0x7C..0x7F  Jeti EX input role commands
 *
 * The policy holds a `PortRegistryBase*` bound by `BoardOf<TBoard>`
 * before `begin()`.  Per-port role storage lives inside the registry's
 * `std::variant` slots — ROLE_ATTACH emplaces, ROLE_DETACH replaces
 * with `std::monostate`.
 *
 * Per-tick `update()` walks every port binding and dispatches
 * `tick()` to whichever role variant is currently active.  Async
 * events from roles (LED queue done, motor stall, servo target reached,
 * RC input broadcast) route through the policy's callback installation
 * at attach time — each role gets a small lambda that emits the right
 * packet via `_ctx`.
 */

#ifndef SFX_ROLE_SERVICE_H
#define SFX_ROLE_SERVICE_H

#include <cstdint>
#include <cstddef>

#include <serial/core/core.h>
#include <serial/roles.h>
#include <serial/ports.h>          // PortKind
#include <serial/wire.h>

#include "board_server.h"
#include "port_registry.h"

namespace sfx_core {

class RoleServicePolicy {
public:
    /// Advertises `CoreCapability::ROLES` — every BoardOf-built
    /// firmware speaks the generic-expander role wire surface.
    static constexpr uint32_t kCapabilityBits = CoreCapability::ROLES;

    RoleServicePolicy() = default;

    void bindRegistry(PortRegistryBase* reg) { _reg = reg; }

    // ── SystemServicePolicy surface ───────────────────────────────────

    bool begin(BoardServerBase* ctx) {
        _ctx = ctx;
        return _ctx != nullptr;
    }

    bool ownsType(uint8_t type) const {
        return (type >= 0x40 && type <= 0x7F);
    }

    CommandHandleResult handle(uint8_t type, const uint8_t* payload, size_t len);

    /// Tick every attached role.  Called from BoardServer::process().
    void update();

    const char* getErrorMessage(uint8_t code) const {
        return RoleError::getMessage(code);
    }

    /// Master-internal hook fired ALONGSIDE the wire packet whenever a
    /// role emits an async event (SERVO_TARGET_REACHED, LED_QUEUE_DONE,
    /// ROLE_ATTACHED/DETACHED, stall events, RC/SBUS/Jeti broadcasts).
    /// Used by `TopologyServicePolicy` so effects can subscribe to
    /// hub-local role events the same way they subscribe to remote
    /// ones — without parsing the COBS output buffer.  Single slot —
    /// only TopologyService should install it.
    using LocalAsyncCallback = void (*)(void* ctx, uint8_t innerType,
                                        const uint8_t* payload, size_t len);
    void setLocalAsyncListener(LocalAsyncCallback fn, void* ctx) {
        _localAsyncFn  = fn;
        _localAsyncCtx = ctx;
    }

private:
    // ── Attach / detach / list ────────────────────────────────────────
    void handleAttach (const uint8_t* p, size_t len);
    void handleDetach (const uint8_t* p, size_t len);
    void handleList   ();

    // ── Servo actuator ────────────────────────────────────────────────
    void handleServoSetTarget    (const uint8_t* p, size_t len);
    void handleServoSetImmediate (const uint8_t* p, size_t len);
    void handleServoGetStatusReq (const uint8_t* p, size_t len);
    void handleServoSetProfile   (const uint8_t* p, size_t len);
    void handleServoGetProfileReq(const uint8_t* p, size_t len);
    void handleServoSetBroadcastHz(const uint8_t* p, size_t len);
    /// Emit the batched SERVO_MOTION_UPDATE snapshot (all active servos).
    void emitServoBroadcast(uint32_t now);

    // ── RC PWM input ──────────────────────────────────────────────────
    void handleRcInGetValueReq    (const uint8_t* p, size_t len);
    void handleRcInSetBroadcastHz (const uint8_t* p, size_t len);

    // ── SBUS input ────────────────────────────────────────────────────
    void handleSbusGetFrameReq    (const uint8_t* p, size_t len);
    void handleSbusSetBroadcastHz (const uint8_t* p, size_t len);

    // ── Jeti EX input ─────────────────────────────────────────────────
    void handleJetiExGetFrameReq    (const uint8_t* p, size_t len);
    void handleJetiExSetBroadcastHz (const uint8_t* p, size_t len);

    // ── LED animator ──────────────────────────────────────────────────
    void handleLedQueueLoad     (const uint8_t* p, size_t len);
    void handleLedStart         (const uint8_t* p, size_t len);
    void handleLedStop          (const uint8_t* p, size_t len);
    void handleLedSetBrightness (const uint8_t* p, size_t len);
    void handleLedGetStatusReq  (const uint8_t* p, size_t len);

    // ── DC motor (uni-directional) ────────────────────────────────────
    void handleMotorSetDuty     (const uint8_t* p, size_t len);
    void handleMotorBrake       (const uint8_t* p, size_t len);
    void handleMotorGetStatusReq(const uint8_t* p, size_t len);
    void handleMotorSetElement  (const uint8_t* p, size_t len);
    void handleMotorGetElementReq(const uint8_t* p, size_t len);
    void handleMotorSetPct      (const uint8_t* p, size_t len);

    // ── Bi-directional motor ──────────────────────────────────────────
    void handleBiMotorSetSigned   (const uint8_t* p, size_t len);
    void handleBiMotorBrake       (const uint8_t* p, size_t len);
    void handleBiMotorCoast       (const uint8_t* p, size_t len);
    void handleBiMotorGetStatus   (const uint8_t* p, size_t len);
    void handleBiMotorSeekEndstop (const uint8_t* p, size_t len);
    void handleBiMotorMoveToEnd   (const uint8_t* p, size_t len);   // 0x5F (Strategy A)
    void handleBiMotorSetGuard    (const uint8_t* p, size_t len);   // 0x77 (live retune)

    // ── Heater ────────────────────────────────────────────────────────
    void handleHeaterSetTarget   (const uint8_t* p, size_t len);
    void handleHeaterGetStatus   (const uint8_t* p, size_t len);
    void handleHeaterSetElement  (const uint8_t* p, size_t len);
    void handleHeaterGetElementReq(const uint8_t* p, size_t len);

    // ── Role emplacement helpers (build role from binding + config bytes) ──
    bool attachServoActuator (ServoBinding&  b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachRcPwmInput    (InputBinding&  b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachSbusInput     (InputBinding&  b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachJetiExInput   (InputBinding&  b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachJetiExTelemetry(InputBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachLedAnimator   (PwmBinding&    b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachDcMotor       (PwmBinding&    b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachHeater        (PwmBinding&    b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);
    bool attachBiDcMotor     (HBridgeBinding& b, uint8_t portIdx, const uint8_t* cfg, size_t cfgLen);

    // ── Async event emitters ─────────────────────────────────────────
    void emitRoleAttached         (uint8_t portKind, uint8_t portIdx, uint8_t roleKind);
    void emitRoleDetached         (uint8_t portKind, uint8_t portIdx);
    void emitLedQueueDone         (uint8_t portIdx);
    void emitServoTargetReached   (uint8_t portIdx, uint16_t pos_us);
    void emitMotorStallEvent      (uint8_t portIdx, uint16_t peak_mA, uint16_t duration_ms);
    void emitBiMotorStallEvent    (uint8_t portIdx, uint16_t peak_mA, uint16_t duration_ms);
    void emitBiMotorEndstopResult (uint8_t portIdx, uint8_t outcome,
                                   uint16_t travel_ms, uint16_t peak_mA,
                                   uint8_t position);
    void emitPpmFrameBroadcast    (uint8_t portIdx, const RcPwmInputRole& role);
    void emitSbusFrameBroadcast   (uint8_t portIdx, const SbusInputRole& role);
    void emitJetiExFrameBroadcast (uint8_t portIdx, const JetiExInputRole& role);

    /// Forwarded by every `emit*()` helper so master-internal
    /// listeners can react without parsing the wire stream.
    void fireLocalAsync(uint8_t innerType, const uint8_t* p, size_t len) {
        if (_localAsyncFn) _localAsyncFn(_localAsyncCtx, innerType, p, len);
    }

    BoardServerBase*    _ctx = nullptr;
    PortRegistryBase*   _reg = nullptr;
    LocalAsyncCallback  _localAsyncFn  = nullptr;
    void*               _localAsyncCtx = nullptr;

    // Generic servo telemetry broadcast (SERVO_MOTION_UPDATE). Global rate set
    // by the host via SERVO_SET_BROADCAST_HZ; 0 = off. Gated additionally on
    // hostVerboseActive() at emit time. Naturally silent during uploads because
    // update() isn't ticked while the loop is upload-exclusive.
    uint8_t  _servoBroadcastHz     = 0;
    uint32_t _servoBroadcastNextMs = 0;
    static constexpr uint8_t kServoBroadcastMaxHz = 50;  // clamp host requests
};

}  // namespace sfx_core

#endif  // SFX_ROLE_SERVICE_H
