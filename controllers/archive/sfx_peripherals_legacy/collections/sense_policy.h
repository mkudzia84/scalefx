/*
 * SensePolicy — per-board sensing layout consumed by PwmCollection.
 *
 * A board firmware writes a small struct that owns its concrete
 * sensor instances (typically INA226s on I²C) and knows which PWM
 * channel index maps to which sensor.  PwmCollection<N, TSense> is
 * templated on this struct — no virtual dispatch, no pointer arrays,
 * the sensing topology is known at compile time.
 *
 * A type satisfies `SensePolicy` when it provides:
 *
 *   void update()
 *   bool readVoltage_mV(uint8_t idx, int32_t& out)
 *   bool readCurrent_mA(uint8_t idx, int32_t& out)
 *
 * Returns false if the channel doesn't have sensing wired.  The
 * collection forwards `update()` to the policy from its own update
 * tick, so the policy can drive periodic sampling work (e.g.,
 * INA226 conversion-ready polling).
 *
 * Example — a board with one current-sensed PWM channel (the
 * smoke-generator heater) wired through an INA226 at 0x40.  Each
 * board firmware names its own struct; the convention is to call it
 * after what it describes — wiring — not after the product:
 *
 *   struct SenseWiring {                // board-local; rename freely
 *       INA226 smokeHeaterSense;        // owns the I²C device
 *       static constexpr uint8_t SMOKE_CH = 1;
 *
 *       void update() { smokeHeaterSense.update(); }
 *
 *       bool readVoltage_mV(uint8_t ch, int32_t& out) {
 *           if (ch != SMOKE_CH) return false;
 *           return smokeHeaterSense.readVoltage_mV(out);
 *       }
 *       bool readCurrent_mA(uint8_t ch, int32_t& out) {
 *           if (ch != SMOKE_CH) return false;
 *           return smokeHeaterSense.readCurrent_mA(out);
 *       }
 *   };
 *
 *   PwmCollection<3, SenseWiring> pwms;
 *
 * The default `NoSensing` is a zero-size struct that always returns
 * false — collections instantiate it for boards without any sensing.
 */

#ifndef SFX_SENSE_POLICY_H
#define SFX_SENSE_POLICY_H

#include <concepts>
#include <cstdint>

namespace sfx_peripherals {

template <typename T>
concept SensePolicy = requires(T& sp, uint8_t idx, int32_t& out) {
    { sp.update()                       };
    { sp.readVoltage_mV(idx, out)       } -> std::convertible_to<bool>;
    { sp.readCurrent_mA(idx, out)       } -> std::convertible_to<bool>;
};

/// Zero-overhead default for boards without per-channel sensing.
struct NoSensing {
    constexpr void update() {}
    constexpr bool readVoltage_mV(uint8_t, int32_t&) { return false; }
    constexpr bool readCurrent_mA(uint8_t, int32_t&) { return false; }
};
static_assert(SensePolicy<NoSensing>);

}  // namespace sfx_peripherals

#endif  // SFX_SENSE_POLICY_H
