/*
 * RoleServicePolicy — system-service policy for the role layer.
 *
 * The policy is the role layer's DISPATCH + LIFECYCLE core.  It owns:
 *   - the 0x40..0x7F packet router (handle)
 *   - the per-loop role tick (update)
 *   - attach / detach / list / bulk-attach (the registry-mutation surface)
 *   - the shared dependencies (registry, board context) + the sub-objects
 *
 * Per-family command/query handling is delegated to one focused handler class
 * per role family (servo / led / dc-motor / bi-motor / heater / input), each
 * bound to the same registry + context + event emitter.  Async telemetry-out
 * is owned by RoleEventEmitter.  The policy holds these by value and wires
 * them in begin(); the wire surface (kCapabilityBits / begin / ownsType /
 * handle / update / setLocalAsyncListener / bindRegistry) is unchanged.
 *
 * The policy holds a `PortRegistryBase*` bound by `BoardOf<TBoard>` before
 * `begin()`.  Per-port role storage lives inside the registry's
 * `std::variant` slots — ROLE_ATTACH emplaces, ROLE_DETACH replaces with
 * `std::monostate`.
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
#include "role_event_emitter.h"
#include "role_servo_handler.h"
#include "role_led_handler.h"
#include "role_motor_handler.h"
#include "role_bimotor_handler.h"
#include "role_heater_handler.h"
#include "role_input_handler.h"

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
        if (!_ctx) return false;
        // Wire the sub-objects to the shared dependencies once the context
        // is known (the registry is bound earlier by BoardOf<>).
        _emit.bind(_ctx);
        _servo.bind  (_reg, _ctx, &_emit);
        _led.bind    (_reg, _ctx, &_emit);
        _motor.bind  (_reg, _ctx, &_emit);
        _bimotor.bind(_reg, _ctx, &_emit);
        _heater.bind (_reg, _ctx, &_emit);
        _input.bind  (_reg, _ctx, &_emit);
        return true;
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
    /// role emits an async event.  Used by `TopologyServicePolicy` so
    /// effects subscribe to hub-local role events the same way they
    /// subscribe to remote ones.  Single slot — only TopologyService
    /// installs it.  Forwarded to the RoleEventEmitter.
    using LocalAsyncCallback = RoleEventEmitter::LocalAsyncCallback;
    void setLocalAsyncListener(LocalAsyncCallback fn, void* ctx) {
        _emit.setListener(fn, ctx);
    }

private:
    // ── Attach / detach / list (registry-mutation router) ─────────────
    void handleAttach     (const uint8_t* p, size_t len);
    void handleBulkAttach (const uint8_t* p, size_t len);  ///< declarative full-set apply (ROLE_BULK_ATTACH)
    void handleDetach     (const uint8_t* p, size_t len);
    void handleList       ();
    /// Apply one role WITHOUT touching the wire; returns 0 (ok) or a wire
    /// error code.  Routes to the right family handler's attach().  Shared
    /// by handleAttach + handleBulkAttach.
    uint8_t applyAttach(uint8_t portKind, uint8_t portIdx, uint8_t roleKind,
                        const uint8_t* cfg, uint8_t cfgLen);

    BoardServerBase*  _ctx = nullptr;
    PortRegistryBase* _reg = nullptr;

    RoleEventEmitter   _emit;
    ServoRoleHandler   _servo;
    LedRoleHandler     _led;
    DcMotorRoleHandler _motor;
    BiMotorRoleHandler _bimotor;
    HeaterRoleHandler  _heater;
    InputRoleHandler   _input;
};

}  // namespace sfx_core

#endif  // SFX_ROLE_SERVICE_H
