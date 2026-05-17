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
 * Role-slot types are narrow per port kind:
 *   Servo:    monostate | ServoActuatorRole | RcPwmInputRole
 *   Pwm:      monostate | LedAnimator       | DcMotorRole | HeaterRole
 *   HBridge:  monostate | BiDcMotorRole
 *
 * A non-template `PortRegistryBase` exposes the runtime API the service
 * policies talk to; the templated subclass holds the actual storage.
 */

#ifndef SFX_PORT_REGISTRY_H
#define SFX_PORT_REGISTRY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

#include <ports/pwm_port.h>
#include <ports/servo_port.h>
#include <ports/hbridge_port.h>
#include <ports/input_port.h>
#include <ports/sensors.h>

#include "../roles/led_animator.h"
#include "../roles/dc_motor_role.h"
#include "../roles/bi_dc_motor_role.h"
#include "../roles/heater_role.h"
#include "../roles/servo_actuator_role.h"
#include "../roles/rc_pwm_input_role.h"
#include "../roles/sbus_input_role.h"
#include "../roles/jeti_ex_input_role.h"

namespace sfx_core {

// ============================================================================
// Binding types — one per port kind, used by both the registry and the
// services.
// ============================================================================

struct ServoBinding {
    sfx_peripherals::ServoPort* port = nullptr;

    // Servo ports are output-only (Rule 31) — only the actuator role.
    using Role = std::variant<std::monostate,
                              ServoActuatorRole>;
    Role role;

    bool occupied() const { return port != nullptr; }
    bool hasRole()  const { return role.index() != 0; }
};

struct InputBinding {
    sfx_peripherals::InputPort* port = nullptr;

    // Input-port role pool — every role here drives the port's mode-
    // switch at attach time (PULSE / SBUS / JETI_EX / UART_RAW).
    using Role = std::variant<std::monostate,
                              RcPwmInputRole,
                              SbusInputRole,
                              JetiExInputRole>;
    Role role;

    bool occupied() const { return port != nullptr; }
    bool hasRole()  const { return role.index() != 0; }
};

struct PwmBinding {
    sfx_peripherals::PwmPort*           port    = nullptr;
    sfx_peripherals::VoltageSensor*     vSense  = nullptr;
    sfx_peripherals::CurrentSensor*     iSense  = nullptr;
    sfx_peripherals::TemperatureSensor* tSense  = nullptr;

    using Role = std::variant<std::monostate,
                              LedAnimator,
                              DcMotorRole,
                              HeaterRole>;
    Role role;

    bool occupied() const { return port != nullptr; }
    bool hasRole()  const { return role.index() != 0; }
};

struct HBridgeBinding {
    sfx_peripherals::HBridgePort*       port    = nullptr;
    sfx_peripherals::VoltageSensor*     vSense  = nullptr;
    sfx_peripherals::CurrentSensor*     iSense  = nullptr;
    sfx_peripherals::TemperatureSensor* tSense  = nullptr;

    using Role = std::variant<std::monostate,
                              BiDcMotorRole>;
    Role role;

    bool occupied() const { return port != nullptr; }
    bool hasRole()  const { return role.index() != 0; }
};

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

template <size_t NServos, size_t NPwms, size_t NHBridges, size_t NInputs>
class PortRegistry : public PortRegistryBase {
public:
    static constexpr size_t kNumServos    = NServos;
    static constexpr size_t kNumPwms      = NPwms;
    static constexpr size_t kNumHBridges  = NHBridges;
    static constexpr size_t kNumInputs    = NInputs;

    uint8_t numServoPorts()    const override { return (uint8_t)NServos; }
    uint8_t numPwmPorts()      const override { return (uint8_t)NPwms; }
    uint8_t numHBridgePorts()  const override { return (uint8_t)NHBridges; }
    uint8_t numInputPorts()    const override { return (uint8_t)NInputs; }

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
};

}  // namespace sfx_core

#endif  // SFX_PORT_REGISTRY_H
