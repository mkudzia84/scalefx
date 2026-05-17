/*
 * ComponentKind — enumeration of component types exposed by a generic slave.
 *
 * Each slave board exposes three collections:
 *   - servos       (ServoCollection<N>)
 *   - pwm outputs  (PwmCollection<M>)        — mode-mutable per channel
 *   - leds         (LedCollection<K, TGpio>) — with light-program runtime
 *
 * Optional sensing (voltage / current via INA226 or analog) can be attached
 * to PWM channels.  Layout is templatised at compile time per board (the
 * GPIO assignments and counts are hardcoded), but the active mode of a
 * PwmGeneric channel can be flipped at runtime between LED / DC motor /
 * heater / raw PWM via the protocol.
 *
 * The values below are wire-format IDs returned by COMPONENT_LIST_RESP and
 * referenced in protocol payloads.  Append-only (Rule 11).
 */

#ifndef SFX_COMPONENT_KIND_H
#define SFX_COMPONENT_KIND_H

#include <cstdint>

namespace sfx_peripherals {

enum class ComponentKind : uint8_t {
    None        = 0x00,
    Servo       = 0x01,   ///< Hobby servo (50 Hz pulse, 500..2500 µs typical)
    PwmGeneric  = 0x02,   ///< Raw PWM output (duty 0..1.0)
    PwmLed      = 0x03,   ///< PWM channel running the LED-event runtime
    PwmMotor    = 0x04,   ///< DC motor (signed speed; H-bridge if 2 pins)
    PwmHeater   = 0x05,   ///< Heater (duty + optional thermistor feedback)
    LedDigital  = 0x06,   ///< GPIO/expander LED with software event sequencer

    /* values 0x07..0xFE reserved for future kinds (sensors, encoders, etc.) */
};

inline const char* componentKindName(ComponentKind k) {
    switch (k) {
        case ComponentKind::None:       return "none";
        case ComponentKind::Servo:      return "servo";
        case ComponentKind::PwmGeneric: return "pwm";
        case ComponentKind::PwmLed:     return "pwm-led";
        case ComponentKind::PwmMotor:   return "pwm-motor";
        case ComponentKind::PwmHeater:  return "pwm-heater";
        case ComponentKind::LedDigital: return "led-digital";
    }
    return "?";
}

/**
 * Per-component capability flags reported by the slave in COMPONENT_LIST_RESP.
 * Append-only.  Each component has its own flag namespace; the meaning of a
 * given bit depends on the component kind.
 */
namespace ServoFlags {
    constexpr uint8_t HAS_MOTION_PROFILE = 1u << 0;   ///< trapezoidal accel/decel
    constexpr uint8_t HAS_PERSISTED_CFG  = 1u << 1;   ///< calibration in YAML
}

namespace PwmFlags {
    constexpr uint8_t MODE_MUTABLE       = 1u << 0;   ///< runtime mode switch allowed
    constexpr uint8_t HAS_VOLTAGE_SENSE  = 1u << 1;   ///< INA226 / ADC voltage available
    constexpr uint8_t HAS_CURRENT_SENSE  = 1u << 2;   ///< INA226 / ADC current available
    constexpr uint8_t IS_BIDIR_PAIR      = 1u << 3;   ///< channel pairs with neighbour for H-bridge
    constexpr uint8_t HAS_THERMISTOR     = 1u << 4;   ///< heater feedback present
}

namespace LedFlags {
    constexpr uint8_t HAS_HARDWARE_PWM   = 1u << 0;   ///< hardware PWM backing (PCA9685 / native PWM); else direct GPIO write
    constexpr uint8_t SUPPORTS_QUEUE     = 1u << 1;   ///< event-queue runtime active (LED_QUEUE_LOAD/START/STOP)
}

/**
 * Wire-format component descriptor — one entry per component returned in
 * COMPONENT_LIST_RESP.  Total 4 bytes; protocol packs N of these into the
 * response payload after a 1-byte count.
 */
struct ComponentInfo {
    uint8_t       index;   ///< 0-based, board-local within its collection
    ComponentKind kind;
    uint8_t       flags;   ///< per-kind flag bitfield (see *Flags namespaces)
    uint8_t       reserved;
} __attribute__((packed));

static_assert(sizeof(ComponentInfo) == 4, "ComponentInfo wire-size mismatch");

}  // namespace sfx_peripherals

#endif  // SFX_COMPONENT_KIND_H
