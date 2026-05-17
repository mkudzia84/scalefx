/*
 * port_descriptor.h — compile-time descriptors used in a board's
 * static `kServoPorts` / `kPwmPorts` / `kHBridgePorts` lists.
 *
 * Each descriptor captures:
 *   - the pointer-to-data-member (PMD) of the port object on the board
 *   - optional PMDs of sensor objects on the board
 *
 * PMDs are valid non-type template parameters in C++20.  The
 * descriptors are zero-size; the descriptor list is a `std::tuple`
 * whose `std::tuple_size_v` gives the per-kind port count at compile
 * time.  At `begin()`, `BoardOf<TBoard>` iterates each list, extracts
 * the typed port from `(board->*PMD)`, upcasts to the abstract
 * interface, and writes the binding into the registry.
 *
 * Usage in a board class:
 *
 *   class MyBoard : public sfx_core::BoardOf<MyBoard> {
 *   public:
 *       Pca9685             pca       {Wire, 0x70};
 *       Pca9685PwmPort      pwm0      {pca,  0};
 *       Ina226CurrentSensor iSense0   {...};
 *
 *       static constexpr auto kPwmPorts = ports::list(
 *           ports::pwm<&MyBoard::pwm0>().with_iSense<&MyBoard::iSense0>());
 *   };
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
// ServoDescriptor
// ============================================================================

template <auto PortMember>
struct ServoDescriptor {
    template <typename Board>
    static sfx_peripherals::ServoPort* extract(Board* b) {
        // PortMember is a PMD on Board's concrete subclass.  Dereference,
        // then upcast to the abstract interface.
        return static_cast<sfx_peripherals::ServoPort*>(&(b->*PortMember));
    }

    template <typename Board>
    static void fillSenses(Board*, ServoBinding&) {}   // servo ports have no sense channels
};

// Factory: `ports::servo<&Board::member>()` → ServoDescriptor.
template <auto PortMember>
constexpr auto servo() { return ServoDescriptor<PortMember>{}; }

// ============================================================================
// PwmDescriptor (with optional sensor PMDs)
// ============================================================================

template <auto PortMember,
          auto VSenseMember = static_cast<void*>(nullptr),
          auto ISenseMember = static_cast<void*>(nullptr),
          auto TSenseMember = static_cast<void*>(nullptr)>
struct PwmDescriptor {
    template <typename Board>
    static sfx_peripherals::PwmPort* extract(Board* b) {
        return static_cast<sfx_peripherals::PwmPort*>(&(b->*PortMember));
    }

    template <typename Board>
    static void fillSenses(Board* b, PwmBinding& binding) {
        if constexpr (!std::is_same_v<decltype(VSenseMember), void*>) {
            binding.vSense = static_cast<sfx_peripherals::VoltageSensor*>(&(b->*VSenseMember));
        }
        if constexpr (!std::is_same_v<decltype(ISenseMember), void*>) {
            binding.iSense = static_cast<sfx_peripherals::CurrentSensor*>(&(b->*ISenseMember));
        }
        if constexpr (!std::is_same_v<decltype(TSenseMember), void*>) {
            binding.tSense = static_cast<sfx_peripherals::TemperatureSensor*>(&(b->*TSenseMember));
        }
    }

    // Builders — refine descriptor with sensor PMDs.
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

// ============================================================================
// HBridgeDescriptor (with optional sensor PMDs)
// ============================================================================

template <auto PortMember,
          auto VSenseMember = static_cast<void*>(nullptr),
          auto ISenseMember = static_cast<void*>(nullptr),
          auto TSenseMember = static_cast<void*>(nullptr)>
struct HBridgeDescriptor {
    template <typename Board>
    static sfx_peripherals::HBridgePort* extract(Board* b) {
        return static_cast<sfx_peripherals::HBridgePort*>(&(b->*PortMember));
    }

    template <typename Board>
    static void fillSenses(Board* b, HBridgeBinding& binding) {
        if constexpr (!std::is_same_v<decltype(VSenseMember), void*>) {
            binding.vSense = static_cast<sfx_peripherals::VoltageSensor*>(&(b->*VSenseMember));
        }
        if constexpr (!std::is_same_v<decltype(ISenseMember), void*>) {
            binding.iSense = static_cast<sfx_peripherals::CurrentSensor*>(&(b->*ISenseMember));
        }
        if constexpr (!std::is_same_v<decltype(TSenseMember), void*>) {
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

// ============================================================================
// List builder
// ============================================================================

/// Aggregate descriptors into a tuple usable in `static constexpr auto`.
template <typename... Ds>
constexpr auto list(Ds... ds) { return std::tuple{ds...}; }

/// Empty list — used when a board declares zero ports of a kind.
constexpr auto empty() { return std::tuple<>{}; }

}  // namespace ports
}  // namespace sfx_core

#endif  // SFX_PORT_DESCRIPTOR_H
