/*
 * Port bindings — the per-port-kind slot value types.
 *
 * One trivial value type per declared port kind: a pointer to the abstract
 * (driver-erased) port, optional sensor pointers, the declared rail voltage,
 * and a typed `std::variant` holding the currently-attached role.  They are a
 * cohesive family (always used together by both the registry and the role
 * service), kept in one file separate from the registry CONTAINER classes in
 * port_registry.h.
 *
 * Role-slot types are narrow per port kind.  A kind is multi-role only when
 * the SAME hardware genuinely supports DISTINCT behaviours; a kind with a
 * single behavioural shape is fixed to one role (the role still exists as the
 * smart/configurable/stateful layer over the dumb port):
 *   Servo:    monostate | ServoActuatorRole                                  (fixed)
 *   Input:    monostate | RcPwmInputRole | SbusInputRole | JetiExInputRole   (multi-modal)
 *   Pwm:      monostate | LedAnimator    | DcMotorRole    | HeaterRole       (multi-role)
 *   HBridge:  monostate | BiDcMotorRole                                      (fixed)
 */

#ifndef SFX_PORT_BINDINGS_H
#define SFX_PORT_BINDINGS_H

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
#include "../roles/jeti_ex_telemetry_role.h"

namespace sfx_core {

// `voltage_mV` (Phase 0 of the GunFX rollout — instructions/22): the
// declared rail voltage this port is wired to (8 V on HubFX CH1..8,
// 5 V on the servo headers, 3.3 V on input GPIOs, …).  Boards set it
// via `descriptor.with_voltage_mV<N>()`; 0 = unknown (UI shows no
// label, no scaling).  Effects that drive sub-rail elements (e.g. a
// 5 V smoke heater on the 8 V rail) read it to compute the PWM duty
// that delivers the element's rated voltage.

struct ServoBinding {
    sfx_peripherals::ServoPort* port = nullptr;
    uint16_t  voltageMv = 0;

    // Servo ports are output-only (Rule 31) — only the actuator role.
    using Role = std::variant<std::monostate,
                              ServoActuatorRole>;
    Role role;

    bool occupied() const { return port != nullptr; }
    bool hasRole()  const { return role.index() != 0; }
};

struct InputBinding {
    sfx_peripherals::InputPort* port = nullptr;
    uint16_t  voltageMv = 0;

    // Input-port role pool — every role here drives the port's mode-
    // switch at attach time (PULSE / SBUS / JETI_EX / UART_RAW).
    using Role = std::variant<std::monostate,
                              RcPwmInputRole,
                              SbusInputRole,
                              JetiExInputRole,
                              JetiExTelemetryRole>;
    Role role;

    bool occupied() const { return port != nullptr; }
    bool hasRole()  const { return role.index() != 0; }
};

struct PwmBinding {
    sfx_peripherals::PwmPort*           port    = nullptr;
    uint16_t  voltageMv = 0;
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
    uint16_t  voltageMv = 0;
    sfx_peripherals::VoltageSensor*     vSense  = nullptr;
    sfx_peripherals::CurrentSensor*     iSense  = nullptr;
    sfx_peripherals::TemperatureSensor* tSense  = nullptr;

    using Role = std::variant<std::monostate,
                              BiDcMotorRole>;
    Role role;

    bool occupied() const { return port != nullptr; }
    bool hasRole()  const { return role.index() != 0; }
};

}  // namespace sfx_core

#endif  // SFX_PORT_BINDINGS_H
