/*
 * port_descriptor.h — compile-time descriptors used in a board's static
 * `kServoPorts` / `kPwmPorts` / `kHBridgePorts` lists.
 *
 * Each descriptor captures:
 *   - the pointer-to-data-member (PMD) of the port object on the board
 *     (or, for array variants, the PMD of a port array + its element count)
 *   - optional PMDs of sensor objects on the board
 *
 * PMDs are valid non-type template parameters in C++20.  Descriptors
 * are zero-size; the descriptor list is a `std::tuple` whose elements
 * each contribute `kCount` slots to the registry.  At `begin()`,
 * `BoardOf<TBoard>` iterates each descriptor, walks `[0, kCount)`,
 * extracts the typed port from `(board->*PMD)[i]`, upcasts to the
 * abstract interface, and writes the binding into the registry.
 *
 * Single-port form (one PMD per port):
 *
 *   static constexpr auto kPwmPorts = ports::list(
 *       ports::pwm<&MyBoard::pwm0>().with_iSense<&MyBoard::iSense0>());
 *
 * Array form (one PMD per port array — collapses N declarations):
 *
 *   sfx_peripherals::Pca9685PwmPort      pwm    [8] = { ...8 elements... };
 *   sfx_peripherals::Ina226VoltageSensor vSense [8] = { ...8 elements... };
 *   sfx_peripherals::Ina226CurrentSensor iSense [8] = { ...8 elements... };
 *
 *   static constexpr auto kPwmPorts = ports::list(
 *       ports::pwm_array<&MyBoard::pwm, 8>()
 *           .with_vSense_array<&MyBoard::vSense>()
 *           .with_iSense_array<&MyBoard::iSense>());
 *
 * Both forms can coexist in the same `ports::list(...)` call — the
 * registry binds them in declaration order.
 */

#ifndef SFX_PORT_DESCRIPTOR_H
#define SFX_PORT_DESCRIPTOR_H

#include <cstddef>
#include <tuple>
#include <type_traits>

#include "port_registry.h"

namespace sfx_core {
namespace ports {

// ============================================================================
// Detection helper — distinguishes "PMD supplied" from "default sentinel"
// ============================================================================
//
// Default sentinel is `void*` so the auto NTTP has a concrete structural
// type; if a real PMD is passed in, `decltype(M)` is the PMD type instead.
namespace detail {
    template <auto M>
    inline constexpr bool kIsSet = !std::is_same_v<decltype(M), void*>;
}  // namespace detail

// ============================================================================
// Single-port descriptors  (kCount = 1)
// ============================================================================

template <auto PortMember>
struct ServoDescriptor {
    static constexpr size_t kCount = 1;

    template <typename Board>
    static sfx_peripherals::ServoPort* extractAt(Board* b, size_t /*i*/) {
        return static_cast<sfx_peripherals::ServoPort*>(&(b->*PortMember));
    }

    template <typename Board>
    static void fillSensesAt(Board*, ServoBinding&, size_t /*i*/) {}  // no sense channels on servo
};

template <auto PortMember>
constexpr auto servo() { return ServoDescriptor<PortMember>{}; }

template <auto PortMember,
          auto VSenseMember = static_cast<void*>(nullptr),
          auto ISenseMember = static_cast<void*>(nullptr),
          auto TSenseMember = static_cast<void*>(nullptr)>
struct PwmDescriptor {
    static constexpr size_t kCount = 1;

    template <typename Board>
    static sfx_peripherals::PwmPort* extractAt(Board* b, size_t /*i*/) {
        return static_cast<sfx_peripherals::PwmPort*>(&(b->*PortMember));
    }

    template <typename Board>
    static void fillSensesAt(Board* b, PwmBinding& binding, size_t /*i*/) {
        if constexpr (detail::kIsSet<VSenseMember>) {
            binding.vSense = static_cast<sfx_peripherals::VoltageSensor*>(&(b->*VSenseMember));
        }
        if constexpr (detail::kIsSet<ISenseMember>) {
            binding.iSense = static_cast<sfx_peripherals::CurrentSensor*>(&(b->*ISenseMember));
        }
        if constexpr (detail::kIsSet<TSenseMember>) {
            binding.tSense = static_cast<sfx_peripherals::TemperatureSensor*>(&(b->*TSenseMember));
        }
    }

    template <auto M> constexpr auto with_vSense() const {
        return PwmDescriptor<PortMember, M, ISenseMember, TSenseMember>{};
    }
    template <auto M> constexpr auto with_iSense() const {
        return PwmDescriptor<PortMember, VSenseMember, M, TSenseMember>{};
    }
    template <auto M> constexpr auto with_tSense() const {
        return PwmDescriptor<PortMember, VSenseMember, ISenseMember, M>{};
    }
};

template <auto PortMember>
constexpr auto pwm() { return PwmDescriptor<PortMember>{}; }

template <auto PortMember,
          auto VSenseMember = static_cast<void*>(nullptr),
          auto ISenseMember = static_cast<void*>(nullptr),
          auto TSenseMember = static_cast<void*>(nullptr)>
struct HBridgeDescriptor {
    static constexpr size_t kCount = 1;

    template <typename Board>
    static sfx_peripherals::HBridgePort* extractAt(Board* b, size_t /*i*/) {
        return static_cast<sfx_peripherals::HBridgePort*>(&(b->*PortMember));
    }

    template <typename Board>
    static void fillSensesAt(Board* b, HBridgeBinding& binding, size_t /*i*/) {
        if constexpr (detail::kIsSet<VSenseMember>) {
            binding.vSense = static_cast<sfx_peripherals::VoltageSensor*>(&(b->*VSenseMember));
        }
        if constexpr (detail::kIsSet<ISenseMember>) {
            binding.iSense = static_cast<sfx_peripherals::CurrentSensor*>(&(b->*ISenseMember));
        }
        if constexpr (detail::kIsSet<TSenseMember>) {
            binding.tSense = static_cast<sfx_peripherals::TemperatureSensor*>(&(b->*TSenseMember));
        }
    }

    template <auto M> constexpr auto with_vSense() const {
        return HBridgeDescriptor<PortMember, M, ISenseMember, TSenseMember>{};
    }
    template <auto M> constexpr auto with_iSense() const {
        return HBridgeDescriptor<PortMember, VSenseMember, M, TSenseMember>{};
    }
    template <auto M> constexpr auto with_tSense() const {
        return HBridgeDescriptor<PortMember, VSenseMember, ISenseMember, M>{};
    }
};

template <auto PortMember>
constexpr auto hbridge() { return HBridgeDescriptor<PortMember>{}; }

// Input ports — no per-port sensor channels (UART or pulse-capture
// don't pair with V/I/T sense the way PWM rails do).
template <auto PortMember>
struct InputDescriptor {
    static constexpr size_t kCount = 1;

    template <typename Board>
    static sfx_peripherals::InputPort* extractAt(Board* b, size_t /*i*/) {
        return static_cast<sfx_peripherals::InputPort*>(&(b->*PortMember));
    }

    template <typename Board>
    static void fillSensesAt(Board*, InputBinding&, size_t /*i*/) {}
};

template <auto PortMember>
constexpr auto input() { return InputDescriptor<PortMember>{}; }

// ============================================================================
// Array descriptors  (kCount = N)
// ============================================================================
//
// One descriptor entry per **port array** instead of per element.  PMDs
// point to whole arrays of port / sensor objects on the board; the
// descriptor walks `[0, N)` at bind time, dereferencing
// `(board->*PMD)[i]` to get each element.
//
// Sense PMDs are themselves array PMDs — element `i`'s sense object is
// at `(board->*SensePMD)[i]`, matching index-for-index with the port
// array.
//

template <auto ArrayPortMember, size_t N>
struct ServoArrayDescriptor {
    static constexpr size_t kCount = N;

    template <typename Board>
    static sfx_peripherals::ServoPort* extractAt(Board* b, size_t i) {
        return static_cast<sfx_peripherals::ServoPort*>(&((b->*ArrayPortMember)[i]));
    }

    template <typename Board>
    static void fillSensesAt(Board*, ServoBinding&, size_t /*i*/) {}
};

template <auto ArrayPortMember, size_t N>
constexpr auto servo_array() { return ServoArrayDescriptor<ArrayPortMember, N>{}; }

template <auto ArrayPortMember, size_t N,
          auto ArrayVSenseMember = static_cast<void*>(nullptr),
          auto ArrayISenseMember = static_cast<void*>(nullptr),
          auto ArrayTSenseMember = static_cast<void*>(nullptr)>
struct PwmArrayDescriptor {
    static constexpr size_t kCount = N;

    template <typename Board>
    static sfx_peripherals::PwmPort* extractAt(Board* b, size_t i) {
        return static_cast<sfx_peripherals::PwmPort*>(&((b->*ArrayPortMember)[i]));
    }

    template <typename Board>
    static void fillSensesAt(Board* b, PwmBinding& binding, size_t i) {
        if constexpr (detail::kIsSet<ArrayVSenseMember>) {
            binding.vSense = static_cast<sfx_peripherals::VoltageSensor*>(&((b->*ArrayVSenseMember)[i]));
        }
        if constexpr (detail::kIsSet<ArrayISenseMember>) {
            binding.iSense = static_cast<sfx_peripherals::CurrentSensor*>(&((b->*ArrayISenseMember)[i]));
        }
        if constexpr (detail::kIsSet<ArrayTSenseMember>) {
            binding.tSense = static_cast<sfx_peripherals::TemperatureSensor*>(&((b->*ArrayTSenseMember)[i]));
        }
    }

    template <auto M> constexpr auto with_vSense_array() const {
        return PwmArrayDescriptor<ArrayPortMember, N, M, ArrayISenseMember, ArrayTSenseMember>{};
    }
    template <auto M> constexpr auto with_iSense_array() const {
        return PwmArrayDescriptor<ArrayPortMember, N, ArrayVSenseMember, M, ArrayTSenseMember>{};
    }
    template <auto M> constexpr auto with_tSense_array() const {
        return PwmArrayDescriptor<ArrayPortMember, N, ArrayVSenseMember, ArrayISenseMember, M>{};
    }
};

template <auto ArrayPortMember, size_t N>
constexpr auto pwm_array() { return PwmArrayDescriptor<ArrayPortMember, N>{}; }

template <auto ArrayPortMember, size_t N,
          auto ArrayVSenseMember = static_cast<void*>(nullptr),
          auto ArrayISenseMember = static_cast<void*>(nullptr),
          auto ArrayTSenseMember = static_cast<void*>(nullptr)>
struct HBridgeArrayDescriptor {
    static constexpr size_t kCount = N;

    template <typename Board>
    static sfx_peripherals::HBridgePort* extractAt(Board* b, size_t i) {
        return static_cast<sfx_peripherals::HBridgePort*>(&((b->*ArrayPortMember)[i]));
    }

    template <typename Board>
    static void fillSensesAt(Board* b, HBridgeBinding& binding, size_t i) {
        if constexpr (detail::kIsSet<ArrayVSenseMember>) {
            binding.vSense = static_cast<sfx_peripherals::VoltageSensor*>(&((b->*ArrayVSenseMember)[i]));
        }
        if constexpr (detail::kIsSet<ArrayISenseMember>) {
            binding.iSense = static_cast<sfx_peripherals::CurrentSensor*>(&((b->*ArrayISenseMember)[i]));
        }
        if constexpr (detail::kIsSet<ArrayTSenseMember>) {
            binding.tSense = static_cast<sfx_peripherals::TemperatureSensor*>(&((b->*ArrayTSenseMember)[i]));
        }
    }

    template <auto M> constexpr auto with_vSense_array() const {
        return HBridgeArrayDescriptor<ArrayPortMember, N, M, ArrayISenseMember, ArrayTSenseMember>{};
    }
    template <auto M> constexpr auto with_iSense_array() const {
        return HBridgeArrayDescriptor<ArrayPortMember, N, ArrayVSenseMember, M, ArrayTSenseMember>{};
    }
    template <auto M> constexpr auto with_tSense_array() const {
        return HBridgeArrayDescriptor<ArrayPortMember, N, ArrayVSenseMember, ArrayISenseMember, M>{};
    }
};

template <auto ArrayPortMember, size_t N>
constexpr auto hbridge_array() { return HBridgeArrayDescriptor<ArrayPortMember, N>{}; }

template <auto ArrayPortMember, size_t N>
struct InputArrayDescriptor {
    static constexpr size_t kCount = N;

    template <typename Board>
    static sfx_peripherals::InputPort* extractAt(Board* b, size_t i) {
        return static_cast<sfx_peripherals::InputPort*>(&((b->*ArrayPortMember)[i]));
    }

    template <typename Board>
    static void fillSensesAt(Board*, InputBinding&, size_t /*i*/) {}
};

template <auto ArrayPortMember, size_t N>
constexpr auto input_array() { return InputArrayDescriptor<ArrayPortMember, N>{}; }

// ============================================================================
// List builder + total-count helper
// ============================================================================

/// Aggregate descriptors into a tuple usable in `static constexpr auto`.
template <typename... Ds>
constexpr auto list(Ds... ds) { return std::tuple{ds...}; }

/// Empty list — used when a board declares zero ports of a kind.
constexpr auto empty() { return std::tuple<>{}; }

/// Sum of `kCount` across a tuple of descriptors.  Used by BoardOf to
/// size the PortRegistry exactly.
template <typename Tuple>
struct DescriptorCount;

template <typename... Ds>
struct DescriptorCount<std::tuple<Ds...>> {
    static constexpr size_t value = (Ds::kCount + ... + 0u);
};

template <typename Tuple>
inline constexpr size_t descriptorCount_v =
    DescriptorCount<std::remove_cv_t<std::remove_reference_t<Tuple>>>::value;

}  // namespace ports
}  // namespace sfx_core

#endif  // SFX_PORT_DESCRIPTOR_H
