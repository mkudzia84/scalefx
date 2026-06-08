/*
 * BoardOf<TBoard> — CRTP base that turns a board's static port
 * descriptors into a fully-wired `BoardServer<PortServicePolicy,
 * RoleServicePolicy, ExtraPolicies...>`.
 *
 * A board class lists its drivers as members and its ports as static
 * `kServoPorts` / `kPwmPorts` / `kHBridgePorts` tuples of descriptors.
 * `BoardOf<>`:
 *
 *   1. Derives the per-kind port counts from `std::tuple_size_v` of
 *      the descriptor tuples → sizes the `PortRegistry` exactly.
 *   2. In the constructor, hands the registry pointer to the
 *      port + role policies via their `bindRegistry()` setter.
 *   3. In `begin()`:
 *        a. iterates each descriptor tuple, dereferences the PMD on
 *           the concrete board, and fills the registry's binding
 *           array (port pointer + sensor pointers);
 *        b. calls each port's own `begin()` for hardware init;
 *        c. delegates to `BoardServer::begin()` for Serial / DiagLog /
 *           indicator pins / lifecycle wiring (which itself calls
 *           every policy's `begin()` including the port + role services).
 *   4. `process()` is inherited verbatim from `BoardServer`.
 *
 * Boards typically subclass like:
 *
 *   class MyBoard : public sfx_core::BoardOf<MyBoard> {
 *   public:
 *       Pca9685             pca {Wire, 0x70};
 *       Pca9685PwmPort      pwm0{pca, 0};
 *       static constexpr auto kPwmPorts = ports::list(
 *           ports::pwm<&MyBoard::pwm0>());
 *       // kServoPorts / kHBridgePorts optional (default empty list).
 *       static constexpr const char* kName = "MyBoard";
 *   };
 */

#ifndef SFX_BOARD_OF_H
#define SFX_BOARD_OF_H

#include <cstddef>
#include <tuple>
#include <type_traits>

#include "board_server.h"
#include "port_registry.h"
#include "port_descriptor.h"
#include "port_service.h"
#include "role_service.h"

namespace sfx_core {

// ============================================================================
// Helpers: detect whether TBoard supplies a kXxx list (default = empty)
// ============================================================================

template <typename TBoard, typename = void>
struct ServoListOf  { static constexpr auto value = ports::empty(); };
template <typename TBoard>
struct ServoListOf <TBoard, std::void_t<decltype(TBoard::kServoPorts)>> {
    static constexpr auto value = TBoard::kServoPorts;
};

template <typename TBoard, typename = void>
struct PwmListOf    { static constexpr auto value = ports::empty(); };
template <typename TBoard>
struct PwmListOf   <TBoard, std::void_t<decltype(TBoard::kPwmPorts)>> {
    static constexpr auto value = TBoard::kPwmPorts;
};

template <typename TBoard, typename = void>
struct HBridgeListOf{ static constexpr auto value = ports::empty(); };
template <typename TBoard>
struct HBridgeListOf<TBoard, std::void_t<decltype(TBoard::kHBridgePorts)>> {
    static constexpr auto value = TBoard::kHBridgePorts;
};

template <typename TBoard, typename = void>
struct InputListOf  { static constexpr auto value = ports::empty(); };
template <typename TBoard>
struct InputListOf  <TBoard, std::void_t<decltype(TBoard::kInputPorts)>> {
    static constexpr auto value = TBoard::kInputPorts;
};

// ============================================================================
// PortCapacity<NServo, NPwm, NHBridge, NInput> — registry-sizing tag
// ============================================================================
//
// The PortRegistry holds one fixed-size array of bindings per port kind,
// and each binding embeds a `std::variant<...Role>` whose size is the
// largest role it can hold (motion-profile integrator for servos, SBUS/
// Jeti frame buffers for inputs, LED-animator state for PWM).  At 80-200
// bytes per slot, an over-generous cap is real DRAM .bss waste — sizing
// to the board's actual hub-local hardware reclaimed ~17 KB on HubFX.
//
// A board declares its capacity inline as the `Caps` template argument of
// `BoardOf<>` (right after `TStream`, before the policy pack), so the
// port budget is visible at the board's declaration rather than hidden in
// a separate specialisation block:
//
//   class MyBoard : public sfx_core::BoardOf<
//       MyBoard, MyStream,
//       sfx_core::PortCapacity</*servo*/12, /*pwm*/8, /*hbridge*/0, /*input*/1>,
//       PolicyA, PolicyB, …> { … };
//
// Passing it as a template ARGUMENT (not a trait keyed on TBoard, and not
// a member of TBoard) sidesteps the CRTP timing trap: the registry TYPE
// is needed when `BoardOf<TBoard, …>` is instantiated as TBoard's base,
// at which point TBoard is still incomplete — so `TBoard::kMaxXxx` would
// be unreadable, but a template argument is available immediately.
//
// `DefaultPortCapacity` keeps the historical generous bounds for a board
// that genuinely doesn't want to think about it.  A board addressing
// expander ports does NOT need headroom here — remote ports live on the
// expander's own registry (Topology forwards ROLE_ATTACH over CDC); this
// cap covers HUB-LOCAL ports only.  Caps MUST be >= the board's declared
// kXxxPorts counts or begin() silently clamps (drops trailing ports).
template <size_t NServo, size_t NPwm, size_t NHBridge, size_t NInput>
struct PortCapacity {
    static constexpr size_t servo   = NServo;
    static constexpr size_t pwm     = NPwm;
    static constexpr size_t hbridge = NHBridge;
    static constexpr size_t input   = NInput;
};

/// Historical generous bounds — use when a board hasn't been sized yet.
using DefaultPortCapacity = PortCapacity<32, 32, 16, 8>;

// ============================================================================
// BoardOf<TBoard, TStream, Caps, ExtraPolicies...>
// ============================================================================

/// @tparam TBoard         CRTP-style derived board type (provides
///                        kName + the static kXxxPorts descriptor lists).
/// @tparam TStream        Wire-protocol stream type — MUST derive `sfx::Stream`
///                        (BoardServerBaseT mirrors it into the base
///                        `sfx::Stream* _serial`).  `sfx::NativeUartStream` on
///                        the ESP32 IDF-component path; `sfx::PicoSerialStream<…>`
///                        (the USB-CDC `Serial` adapter, platform/pico_serial_stream.h)
///                        on Pico.  Re-exported as `StreamType` for any helper
///                        parameterised on `TBoard` alone.
/// @tparam Caps           `PortCapacity<NServo,NPwm,NHBridge,NInput>` sizing
///                        the per-kind registry binding arrays.  Use
///                        `DefaultPortCapacity` for the historical bounds.
/// @tparam ExtraPolicies  User policies appended after the built-in
///                        PortServicePolicy + RoleServicePolicy.
template <typename TBoard, typename TStream, typename Caps, typename... ExtraPolicies>
class BoardOf : public BoardServer<TStream,
                                   PortServicePolicy,
                                   RoleServicePolicy,
                                   ExtraPolicies...> {
public:
    /// Stream type carrier-typedef — Rule 33.  Lets downstream helpers
    /// recover the underlying stream type via `TBoard::StreamType`
    /// without re-templating on TStream themselves.
    using StreamType = TStream;

    // Registry max capacities — sourced from the `Caps` template argument
    // (`PortCapacity<…>`).  Actual occupancy is tracked at runtime in the
    // `_numXxx` counters that `begin()` populates from `TBoard::kServoPorts`
    // / `kPwmPorts` / `kHBridgePorts` / `kInputPorts`.  A template argument
    // (not a member of `TBoard`) sidesteps the CRTP timing trap: at
    // base-class instantiation `TBoard` is incomplete, so `TBoard::kMaxXxx`
    // would be unreadable, but `Caps` is available immediately.
    static constexpr size_t kMaxServoPorts   = Caps::servo;
    static constexpr size_t kMaxPwmPorts     = Caps::pwm;
    static constexpr size_t kMaxHBridgePorts = Caps::hbridge;
    static constexpr size_t kMaxInputPorts   = Caps::input;

    using Base     = BoardServer<TStream, PortServicePolicy, RoleServicePolicy, ExtraPolicies...>;
    using Registry = PortRegistry<kMaxServoPorts, kMaxPwmPorts,
                                   kMaxHBridgePorts, kMaxInputPorts>;

    BoardOf() {
        // Hand the registry to the policies before begin() runs.
        this->template policy<PortServicePolicy>().bindRegistry(&_ports);
        this->template policy<RoleServicePolicy>().bindRegistry(&_ports);
    }

    /// Drive the full board lifecycle: bind static port descriptors to
    /// the registry, call each port's `begin()`, then delegate to
    /// `BoardServer::begin()` for Serial / DiagLog / policies.
    void begin(TStream& stream, const char* version, uint32_t buildNumber,
               int connectionPin = 13, int errorPin = 14) {
        auto* self = static_cast<TBoard*>(this);

        // ── Fill registry from static descriptor lists ────────────────
        //
        // `begin()` is parsed when first called, NOT when the base
        // class template is instantiated, so `TBoard` is complete here
        // and `requires { TBoard::kXxxPorts; }` resolves correctly
        // against the user's declarations.  Each branch returns the
        // count of slots actually filled, which we hand to the
        // registry so `numXxxPorts()` reflects the real population.

        uint8_t nServos = 0;
        if constexpr (requires { TBoard::kServoPorts; }) {
            nServos = bindServos(self, _ports._servos, TBoard::kServoPorts);
        }
        uint8_t nPwms = 0;
        if constexpr (requires { TBoard::kPwmPorts; }) {
            nPwms = bindPwms(self, _ports._pwms, TBoard::kPwmPorts);
        }
        uint8_t nHBridges = 0;
        if constexpr (requires { TBoard::kHBridgePorts; }) {
            nHBridges = bindHBridges(self, _ports._hbridges, TBoard::kHBridgePorts);
        }
        uint8_t nInputs = 0;
        if constexpr (requires { TBoard::kInputPorts; }) {
            nInputs = bindInputs(self, _ports._inputs, TBoard::kInputPorts);
        }
        _ports.setNumServoPorts  (nServos);
        _ports.setNumPwmPorts    (nPwms);
        _ports.setNumHBridgePorts(nHBridges);
        _ports.setNumInputPorts  (nInputs);

        // ── Hardware init for every port ──────────────────────────────
        for (uint8_t i = 0; i < nServos; i++)   { if (_ports._servos[i].port)   _ports._servos[i].port->begin(); }
        for (uint8_t i = 0; i < nPwms; i++)     { if (_ports._pwms[i].port)     _ports._pwms[i].port->begin(); }
        for (uint8_t i = 0; i < nHBridges; i++) { if (_ports._hbridges[i].port) _ports._hbridges[i].port->begin(); }
        for (uint8_t i = 0; i < nInputs; i++)   { if (_ports._inputs[i].port)   _ports._inputs[i].port->begin(); }

        // ── Expose the registry through BoardServerBase ───────────────
        // Done before `Base::begin()` so policies walking the pack can
        // resolve `_ctx->portRegistry()` inside their own `begin()`.
        this->_portRegistry = &_ports;

        // ── Delegate to BoardServer lifecycle wiring ──────────────────
        Base::begin(stream, TBoard::kName, version, buildNumber, connectionPin, errorPin);

        // OR the port-kind presence bits (HAS_SERVO_PORTS, ...) into
        // the IDENTIFY capabilities word.  Derived from runtime counts
        // since the registry sizes are now dynamic.
        uint32_t presence = 0;
        if (nServos   > 0) presence |= CoreCapability::HAS_SERVO_PORTS;
        if (nPwms     > 0) presence |= CoreCapability::HAS_PWM_PORTS;
        if (nHBridges > 0) presence |= CoreCapability::HAS_HBRIDGE_PORTS;
        if (nInputs   > 0) presence |= CoreCapability::HAS_INPUT_PORTS;
        this->core().addCapability(presence);
    }

    Registry&       registry()       { return _ports; }
    const Registry& registry() const { return _ports; }

private:
    // For each descriptor in the tuple, walk `[0, kCount)`; for each
    // slot, write the binding into the registry's per-kind array at
    // the running `slotIdx`.  Works uniformly for single-port and
    // array descriptors.
    template <typename Slots, typename... Ds>
    uint8_t bindServos(TBoard* self, Slots& slots, const std::tuple<Ds...>& list) {
        size_t slotIdx = 0;
        auto bindOne = [&](auto& d) {
            using D = std::decay_t<decltype(d)>;
            for (size_t k = 0; k < D::kCount && slotIdx < slots.size(); k++) {
                slots[slotIdx].port = d.template extractAt<TBoard>(self, k);
                d.template fillSensesAt<TBoard>(self, slots[slotIdx], k);
                slotIdx++;
            }
        };
        std::apply([&](auto&... d) { (bindOne(d), ...); }, list);
        return (uint8_t)slotIdx;
    }

    template <typename Slots, typename... Ds>
    uint8_t bindPwms(TBoard* self, Slots& slots, const std::tuple<Ds...>& list) {
        size_t slotIdx = 0;
        auto bindOne = [&](auto& d) {
            using D = std::decay_t<decltype(d)>;
            for (size_t k = 0; k < D::kCount && slotIdx < slots.size(); k++) {
                slots[slotIdx].port = d.template extractAt<TBoard>(self, k);
                d.template fillSensesAt<TBoard>(self, slots[slotIdx], k);
                slotIdx++;
            }
        };
        std::apply([&](auto&... d) { (bindOne(d), ...); }, list);
        return (uint8_t)slotIdx;
    }

    template <typename Slots, typename... Ds>
    uint8_t bindHBridges(TBoard* self, Slots& slots, const std::tuple<Ds...>& list) {
        size_t slotIdx = 0;
        auto bindOne = [&](auto& d) {
            using D = std::decay_t<decltype(d)>;
            for (size_t k = 0; k < D::kCount && slotIdx < slots.size(); k++) {
                slots[slotIdx].port = d.template extractAt<TBoard>(self, k);
                d.template fillSensesAt<TBoard>(self, slots[slotIdx], k);
                slotIdx++;
            }
        };
        std::apply([&](auto&... d) { (bindOne(d), ...); }, list);
        return (uint8_t)slotIdx;
    }

    template <typename Slots, typename... Ds>
    uint8_t bindInputs(TBoard* self, Slots& slots, const std::tuple<Ds...>& list) {
        size_t slotIdx = 0;
        auto bindOne = [&](auto& d) {
            using D = std::decay_t<decltype(d)>;
            for (size_t k = 0; k < D::kCount && slotIdx < slots.size(); k++) {
                slots[slotIdx].port = d.template extractAt<TBoard>(self, k);
                d.template fillSensesAt<TBoard>(self, slots[slotIdx], k);
                slotIdx++;
            }
        };
        std::apply([&](auto&... d) { (bindOne(d), ...); }, list);
        return (uint8_t)slotIdx;
    }

    Registry _ports;
};

}  // namespace sfx_core

#endif  // SFX_BOARD_OF_H
