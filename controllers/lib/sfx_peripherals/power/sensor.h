/*
 * Sensor — C++20 concept for voltage / current / temperature probes.
 *
 * No polymorphism — concrete sensor types satisfy the concept by
 * duck-typing.  Hardware on a board is FIXED (chip placement is
 * a layout decision), so collections + sense policies bind to the
 * concrete types at compile time.  Per Rule 18 in CLAUDE.md:
 *
 *   > Codebase is C++20 (-std=gnu++20 on every board) — every
 *   > policy/schema template is gated by a `concept` + `requires`-
 *   > clause; when adding a new template, define its concept in the
 *   > same header and gate with `requires`.
 *
 * A type satisfies `Sensor` when it provides:
 *
 *   uint32_t capabilities() const           — SensorCap::* bitmask
 *   void     update()                       — drive periodic sampling (may be no-op)
 *   bool     readVoltage_mV (int32_t& out)  — millivolts (signed)
 *   bool     readCurrent_mA (int32_t& out)  — milliamps  (signed: positive = into load)
 *
 * Optional (presence checked via `HasTemperature<T>` etc. helpers):
 *
 *   bool     readTemperature_cC(int16_t& out)
 *   bool     readPower_mW      (int32_t& out)
 *
 * Existing implementations:
 *   - INA226 (this directory) — I²C high-side power monitor with
 *     simultaneous voltage + current readout.
 *   - AdcDividerBatteryT<MultiplierMilli> (battery_monitor.h) —
 *     ADC + resistor divider; voltage only.
 *
 * Both already expose `readVoltage_mV` / `readCurrent_mA` / `update()`
 * with the right signatures, so they satisfy `Sensor` without any
 * adapter shim.  No virtual dispatch, no pointer-to-base, no v-table.
 *
 * Units (SI-aligned, integer-typed for wire-format friendliness):
 *   voltage      in millivolts        (int32_t — signed for differential probes)
 *   current      in milliamperes      (int32_t — signed: positive = into load)
 *   temperature  in 1/100 °C          (int16_t)
 *   power        in milliwatts        (int32_t)
 */

#ifndef SFX_SENSOR_H
#define SFX_SENSOR_H

#include <concepts>
#include <cstdint>

namespace sfx_peripherals {

/// Capability flags for `Sensor::capabilities()`.  Append-only.
namespace SensorCap {
    constexpr uint32_t VOLTAGE     = 1u << 0;
    constexpr uint32_t CURRENT     = 1u << 1;
    constexpr uint32_t TEMPERATURE = 1u << 2;
    constexpr uint32_t POWER       = 1u << 3;
}

// ── Sensor concept ───────────────────────────────────────────────────

template <typename T>
concept Sensor = requires(T& s, int32_t& v_out) {
    { s.capabilities()             } -> std::convertible_to<uint32_t>;
    { s.update()                   };
    { s.readVoltage_mV(v_out)      } -> std::convertible_to<bool>;
    { s.readCurrent_mA(v_out)      } -> std::convertible_to<bool>;
};

// Optional-capability detectors — used at compile time to check if a
// concrete sensor exposes temperature / power readout.

template <typename T>
concept HasTemperature = requires(T& s, int16_t& t) {
    { s.readTemperature_cC(t) } -> std::convertible_to<bool>;
};

template <typename T>
concept HasPower = requires(T& s, int32_t& p) {
    { s.readPower_mW(p) } -> std::convertible_to<bool>;
};

// ── No-op default ────────────────────────────────────────────────────
//
// `NullSensor` satisfies `Sensor` but reports nothing — useful as a
// default template argument for collections / policies that may or may
// not have sensing wired.  Zero size, zero overhead.
struct NullSensor {
    constexpr uint32_t capabilities() const { return 0; }
    constexpr void     update()             {}
    constexpr bool     readVoltage_mV(int32_t&) { return false; }
    constexpr bool     readCurrent_mA(int32_t&) { return false; }
};
static_assert(Sensor<NullSensor>);

}  // namespace sfx_peripherals

#endif  // SFX_SENSOR_H
