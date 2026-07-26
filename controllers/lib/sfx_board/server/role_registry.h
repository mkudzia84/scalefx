/*
 * RoleRegistry — the ONE place that knows the role-type ⇄ RoleKind
 * correspondence and how to walk a board's attached roles.
 *
 * Before this header, three enumerators each hand-rolled the same
 * `std::holds_alternative<>` ladder over the per-port-kind variants:
 *   - RoleServicePolicy::handleList()        (ROLE_LIST_RESP)
 *   - TopologyService::appendHubRoleBlock()  (ROLE_LIST forward block)
 *   - TopologyService::portsByRole()         (effect port lookup)
 * Adding a role kind meant editing all three — which is exactly the
 * Rule 58 leak ("add a role = add its codec, the hub gets no per-role
 * switch") in enumeration form.  Now every enumerator defers here, so a
 * new role kind is ONE `else if constexpr` line in `roleKindFor<>()`.
 *
 * Compile-time only: `if constexpr` over the variant alternatives — no
 * RTTI, no virtual, no runtime role table.
 */

#ifndef SFX_ROLE_REGISTRY_H
#define SFX_ROLE_REGISTRY_H

#include <cstdint>
#include <type_traits>
#include <variant>

#include <serial/roles.h>   // RoleKind
#include <serial/ports.h>   // PortKind

#include "port_registry.h"  // binding types + the role classes they hold

namespace sfx_core {

// Compile-time map: a role class → its wire RoleKind.  THIS ladder is the
// single source of truth; `std::monostate` (and anything unlisted) → None.
template <class TRole>
inline constexpr uint8_t roleKindFor() {
    if      constexpr (std::is_same_v<TRole, ServoActuatorRole>)   return RoleKind::ServoActuator;
    else if constexpr (std::is_same_v<TRole, RcPwmInputRole>)      return RoleKind::RcPwmInput;
    else if constexpr (std::is_same_v<TRole, SbusInputRole>)       return RoleKind::SbusInput;
    else if constexpr (std::is_same_v<TRole, JetiExInputRole>)     return RoleKind::JetiExInput;
    else if constexpr (std::is_same_v<TRole, EscTelemetryRole>) return RoleKind::EscTelemetry;
    else if constexpr (std::is_same_v<TRole, LedAnimator>)         return RoleKind::LedAnimator;
    else if constexpr (std::is_same_v<TRole, DcMotorRole>)         return RoleKind::DcMotor;
    else if constexpr (std::is_same_v<TRole, HeaterRole>)          return RoleKind::Heater;
    else if constexpr (std::is_same_v<TRole, BiDcMotorRole>)       return RoleKind::BiDcMotor;
    else                                                           return RoleKind::None;
}

// Runtime: the RoleKind currently held by a binding's variant slot.
template <class TRoleVariant>
inline uint8_t roleKindOf(const TRoleVariant& v) {
    return std::visit([](auto const& r) -> uint8_t {
        return roleKindFor<std::decay_t<decltype(r)>>();
    }, v);
}

// Walk every ATTACHED role on a registry, calling fn(portKind, portIdx,
// roleKind) for each occupied binding whose slot is non-monostate.  The one
// loop every role enumerator shares.
template <class Fn>
inline void forEachAttachedRole(PortRegistryBase& reg, Fn&& fn) {
    for (uint8_t i = 0; i < reg.numServoPorts(); ++i)
        if (auto* b = reg.servoAt(i); b && b->hasRole())
            fn(PortKind::Servo, i, roleKindOf(b->role));
    for (uint8_t i = 0; i < reg.numPwmPorts(); ++i)
        if (auto* b = reg.pwmAt(i); b && b->hasRole())
            fn(PortKind::Pwm, i, roleKindOf(b->role));
    for (uint8_t i = 0; i < reg.numHBridgePorts(); ++i)
        if (auto* b = reg.hbridgeAt(i); b && b->hasRole())
            fn(PortKind::HBridge, i, roleKindOf(b->role));
    for (uint8_t i = 0; i < reg.numInputPorts(); ++i)
        if (auto* b = reg.inputAt(i); b && b->hasRole())
            fn(PortKind::Input, i, roleKindOf(b->role));
}

}  // namespace sfx_core

#endif  // SFX_ROLE_REGISTRY_H
