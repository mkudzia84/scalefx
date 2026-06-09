/*
 * PortRegistry — typed storage for declared ports + their (variant) roles.
 *
 * A board declares its ports via the static `kServoPorts` / `kPwmPorts`
 * / `kHBridgePorts` lists on its CRTP subclass.  `BoardOf<TBoard>`
 * sizes a `PortRegistry<NServos, NPwms, NHBridges>` accordingly and
 * binds the descriptors at `begin()`.
 *
 * Each binding carries:
 *   - a pointer to the abstract port (driver-erased)
 *   - optional sensor pointers (PWM / H-bridge only)
 *   - a typed `std::variant` slot holding the currently-attached role
 *
 * Role-slot types are narrow per port kind.  A kind is multi-role only
 * when the SAME hardware genuinely supports DISTINCT behaviours; a kind
 * with a single behavioural shape is fixed to one role (the role still
 * exists as the smart/configurable/stateful layer over the dumb port):
 *   Servo:    monostate | ServoActuatorRole                         (fixed — positioning)
 *   Input:    monostate | RcPwmInputRole | SbusInputRole | JetiExInputRole  (multi-modal)
 *   Pwm:      monostate | LedAnimator    | DcMotorRole   | HeaterRole       (multi-role)
 *   HBridge:  monostate | BiDcMotorRole                             (fixed — signed bidir drive)
 *
 * A non-template `PortRegistryBase` exposes the runtime API the service
 * policies talk to; the templated subclass holds the actual storage.
 */

#ifndef SFX_PORT_REGISTRY_H
#define SFX_PORT_REGISTRY_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "port_bindings.h"   // ServoBinding / InputBinding / PwmBinding / HBridgeBinding

namespace sfx_core {

// ============================================================================
// PortRegistryBase — non-template runtime API
// ============================================================================
//
// The service policies hold a `PortRegistryBase*` so they don't have to
// template on the per-board sizes.  The templated subclass plugs in the
// actual storage arrays.
//
class PortRegistryBase {
public:
    virtual ~PortRegistryBase() = default;

    virtual uint8_t numServoPorts()    const = 0;
    virtual uint8_t numPwmPorts()      const = 0;
    virtual uint8_t numHBridgePorts()  const = 0;
    virtual uint8_t numInputPorts()    const = 0;

    virtual ServoBinding*    servoAt   (uint8_t idx) = 0;
    virtual PwmBinding*      pwmAt     (uint8_t idx) = 0;
    virtual HBridgeBinding*  hbridgeAt (uint8_t idx) = 0;
    virtual InputBinding*    inputAt   (uint8_t idx) = 0;
};

// ============================================================================
// PortRegistry<NS, NP, NH> — sized storage
// ============================================================================

// Template parameters set the maximum capacity per port kind.  The
// actual `numXxxPorts()` returned to wire consumers is the count
// `BoardOf<>::begin()` filled at bind time — see `_numXxx` runtime
// counters below.
//
// Why dynamic rather than tight-fit static: when `BoardOf<HubFxBoard>`
// is instantiated to become the base class of `HubFxBoard`, the latter
// is still incomplete, so the SFINAE detector
//   `decltype(HubFxBoard::kServoPorts)`
// fails and the registry would degenerate to a 0-sized array.  Sizing
// to a generous max + a runtime counter keeps the array statically
// allocated while leaving the actual count to be set inside `begin()`,
// which is parsed lazily (HubFxBoard is complete by then).
template <size_t NServos, size_t NPwms, size_t NHBridges, size_t NInputs>
class PortRegistry : public PortRegistryBase {
public:
    static constexpr size_t kMaxServos    = NServos;
    static constexpr size_t kMaxPwms      = NPwms;
    static constexpr size_t kMaxHBridges  = NHBridges;
    static constexpr size_t kMaxInputs    = NInputs;

    uint8_t numServoPorts()    const override { return _numServos;   }
    uint8_t numPwmPorts()      const override { return _numPwms;     }
    uint8_t numHBridgePorts()  const override { return _numHBridges; }
    uint8_t numInputPorts()    const override { return _numInputs;   }

    /// Set by BoardOf<>::begin() once the descriptor lists have been
    /// walked.  Caps at the slot-array size; passing a larger value is
    /// silently clamped so the registry never returns out-of-bounds.
    void setNumServoPorts   (uint8_t n) { _numServos   = n > NServos   ? NServos   : n; }
    void setNumPwmPorts     (uint8_t n) { _numPwms     = n > NPwms     ? NPwms     : n; }
    void setNumHBridgePorts (uint8_t n) { _numHBridges = n > NHBridges ? NHBridges : n; }
    void setNumInputPorts   (uint8_t n) { _numInputs   = n > NInputs   ? NInputs   : n; }

    ServoBinding*    servoAt   (uint8_t idx) override {
        return (idx < NServos)   ? &_servos[idx]   : nullptr;
    }
    PwmBinding*      pwmAt     (uint8_t idx) override {
        return (idx < NPwms)     ? &_pwms[idx]     : nullptr;
    }
    HBridgeBinding*  hbridgeAt (uint8_t idx) override {
        return (idx < NHBridges) ? &_hbridges[idx] : nullptr;
    }
    InputBinding*    inputAt   (uint8_t idx) override {
        return (idx < NInputs)   ? &_inputs[idx]   : nullptr;
    }

    std::array<ServoBinding,   NServos>    _servos;
    std::array<PwmBinding,     NPwms>      _pwms;
    std::array<HBridgeBinding, NHBridges>  _hbridges;
    std::array<InputBinding,   NInputs>    _inputs;

    uint8_t _numServos   = 0;
    uint8_t _numPwms     = 0;
    uint8_t _numHBridges = 0;
    uint8_t _numInputs   = 0;
};

}  // namespace sfx_core

#endif  // SFX_PORT_REGISTRY_H
