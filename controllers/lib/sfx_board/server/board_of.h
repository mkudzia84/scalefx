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
// BoardOf<TBoard, ExtraPolicies...>
// ============================================================================

template <typename TBoard, typename... ExtraPolicies>
class BoardOf : public BoardServer<PortServicePolicy,
                                   RoleServicePolicy,
                                   ExtraPolicies...> {
public:
    using ServoList   = decltype(ServoListOf<TBoard>::value);
    using PwmList     = decltype(PwmListOf<TBoard>::value);
    using HBridgeList = decltype(HBridgeListOf<TBoard>::value);
    using InputList   = decltype(InputListOf<TBoard>::value);

    // Sum of every descriptor's `kCount` — array descriptors contribute
    // N slots, single-port descriptors contribute 1.  Registry is sized
    // exactly to the total.
    static constexpr size_t kNumServos   = ports::descriptorCount_v<ServoList>;
    static constexpr size_t kNumPwms     = ports::descriptorCount_v<PwmList>;
    static constexpr size_t kNumHBridges = ports::descriptorCount_v<HBridgeList>;
    static constexpr size_t kNumInputs   = ports::descriptorCount_v<InputList>;

    /// Compile-time OR of the port-kind presence bits this board
    /// advertises in its IDENTIFY capabilities mask.  Computed from the
    /// registry sizes so no runtime work is needed; `BoardOf<>::begin()`
    /// just OR's this into the policy-aggregated capabilities word.
    static constexpr uint32_t kPortPresenceBits =
        (kNumServos   > 0 ? CoreCapability::HAS_SERVO_PORTS   : 0u) |
        (kNumPwms     > 0 ? CoreCapability::HAS_PWM_PORTS     : 0u) |
        (kNumHBridges > 0 ? CoreCapability::HAS_HBRIDGE_PORTS : 0u) |
        (kNumInputs   > 0 ? CoreCapability::HAS_INPUT_PORTS   : 0u);

    using Base     = BoardServer<PortServicePolicy, RoleServicePolicy, ExtraPolicies...>;
    using Registry = PortRegistry<kNumServos, kNumPwms, kNumHBridges, kNumInputs>;

    BoardOf() {
        // Hand the registry to the policies before begin() runs.
        this->template policy<PortServicePolicy>().bindRegistry(&_ports);
        this->template policy<RoleServicePolicy>().bindRegistry(&_ports);
    }

    /// Drive the full board lifecycle: bind static port descriptors to
    /// the registry, call each port's `begin()`, then delegate to
    /// `BoardServer::begin()` for Serial / DiagLog / policies.
    void begin(const char* version, uint32_t buildNumber,
               int connectionPin = 13, int errorPin = 14) {
        auto* self = static_cast<TBoard*>(this);

        // ── Fill registry from static descriptor lists ────────────────
        bindServos  (self, _ports._servos,   ServoListOf  <TBoard>::value);
        bindPwms    (self, _ports._pwms,     PwmListOf    <TBoard>::value);
        bindHBridges(self, _ports._hbridges, HBridgeListOf<TBoard>::value);
        bindInputs  (self, _ports._inputs,   InputListOf  <TBoard>::value);

        // ── Hardware init for every port ──────────────────────────────
        for (uint8_t i = 0; i < kNumServos; i++)   { if (_ports._servos[i].port)   _ports._servos[i].port->begin(); }
        for (uint8_t i = 0; i < kNumPwms; i++)     { if (_ports._pwms[i].port)     _ports._pwms[i].port->begin(); }
        for (uint8_t i = 0; i < kNumHBridges; i++) { if (_ports._hbridges[i].port) _ports._hbridges[i].port->begin(); }
        for (uint8_t i = 0; i < kNumInputs; i++)   { if (_ports._inputs[i].port)   _ports._inputs[i].port->begin(); }

        // ── Expose the registry through BoardServerBase ───────────────
        // Done before `Base::begin()` so policies walking the pack can
        // resolve `_ctx->portRegistry()` inside their own `begin()`.
        this->_portRegistry = &_ports;

        // ── Delegate to BoardServer lifecycle wiring ──────────────────
        Base::begin(TBoard::kName, version, buildNumber, connectionPin, errorPin);

        // OR the port-kind presence bits (HAS_SERVO_PORTS, ...) into
        // the IDENTIFY capabilities word.  Computed at compile time
        // from the registry sizes — see `kPortPresenceBits` above.
        this->core().addCapability(kPortPresenceBits);
    }

    Registry&       registry()       { return _ports; }
    const Registry& registry() const { return _ports; }

private:
    // For each descriptor in the tuple, walk `[0, kCount)`; for each
    // slot, write the binding into the registry's per-kind array at
    // the running `slotIdx`.  Works uniformly for single-port and
    // array descriptors.
    template <typename Slots, typename... Ds>
    void bindServos(TBoard* self, Slots& slots, const std::tuple<Ds...>& list) {
        size_t slotIdx = 0;
        auto bindOne = [&](auto& d) {
            using D = std::decay_t<decltype(d)>;
            for (size_t k = 0; k < D::kCount; k++) {
                slots[slotIdx].port = d.template extractAt<TBoard>(self, k);
                d.template fillSensesAt<TBoard>(self, slots[slotIdx], k);
                slotIdx++;
            }
        };
        std::apply([&](auto&... d) { (bindOne(d), ...); }, list);
    }

    template <typename Slots, typename... Ds>
    void bindPwms(TBoard* self, Slots& slots, const std::tuple<Ds...>& list) {
        size_t slotIdx = 0;
        auto bindOne = [&](auto& d) {
            using D = std::decay_t<decltype(d)>;
            for (size_t k = 0; k < D::kCount; k++) {
                slots[slotIdx].port = d.template extractAt<TBoard>(self, k);
                d.template fillSensesAt<TBoard>(self, slots[slotIdx], k);
                slotIdx++;
            }
        };
        std::apply([&](auto&... d) { (bindOne(d), ...); }, list);
    }

    template <typename Slots, typename... Ds>
    void bindHBridges(TBoard* self, Slots& slots, const std::tuple<Ds...>& list) {
        size_t slotIdx = 0;
        auto bindOne = [&](auto& d) {
            using D = std::decay_t<decltype(d)>;
            for (size_t k = 0; k < D::kCount; k++) {
                slots[slotIdx].port = d.template extractAt<TBoard>(self, k);
                d.template fillSensesAt<TBoard>(self, slots[slotIdx], k);
                slotIdx++;
            }
        };
        std::apply([&](auto&... d) { (bindOne(d), ...); }, list);
    }

    template <typename Slots, typename... Ds>
    void bindInputs(TBoard* self, Slots& slots, const std::tuple<Ds...>& list) {
        size_t slotIdx = 0;
        auto bindOne = [&](auto& d) {
            using D = std::decay_t<decltype(d)>;
            for (size_t k = 0; k < D::kCount; k++) {
                slots[slotIdx].port = d.template extractAt<TBoard>(self, k);
                d.template fillSensesAt<TBoard>(self, slots[slotIdx], k);
                slotIdx++;
            }
        };
        std::apply([&](auto&... d) { (bindOne(d), ...); }, list);
    }

    Registry _ports;
};

}  // namespace sfx_core

#endif  // SFX_BOARD_OF_H
