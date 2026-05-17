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

    static constexpr size_t kNumServos   = std::tuple_size_v<ServoList>;
    static constexpr size_t kNumPwms     = std::tuple_size_v<PwmList>;
    static constexpr size_t kNumHBridges = std::tuple_size_v<HBridgeList>;

    using Base     = BoardServer<PortServicePolicy, RoleServicePolicy, ExtraPolicies...>;
    using Registry = PortRegistry<kNumServos, kNumPwms, kNumHBridges>;

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
        bindServos  (self, ServoListOf  <TBoard>::value);
        bindPwms    (self, PwmListOf    <TBoard>::value);
        bindHBridges(self, HBridgeListOf<TBoard>::value);

        // ── Hardware init for every port ──────────────────────────────
        for (uint8_t i = 0; i < kNumServos; i++)   { if (_ports._servos[i].port)   _ports._servos[i].port->begin(); }
        for (uint8_t i = 0; i < kNumPwms; i++)     { if (_ports._pwms[i].port)     _ports._pwms[i].port->begin(); }
        for (uint8_t i = 0; i < kNumHBridges; i++) { if (_ports._hbridges[i].port) _ports._hbridges[i].port->begin(); }

        // ── Delegate to BoardServer lifecycle wiring ──────────────────
        Base::begin(TBoard::kName, version, buildNumber, connectionPin, errorPin);
    }

    Registry&       registry()       { return _ports; }
    const Registry& registry() const { return _ports; }

private:
    template <typename... Ds, size_t... Is>
    void bindServosImpl(TBoard* self, const std::tuple<Ds...>& list, std::index_sequence<Is...>) {
        ((_ports._servos[Is].port = std::get<Is>(list).template extract<TBoard>(self),
          std::get<Is>(list).template fillSenses<TBoard>(self, _ports._servos[Is])), ...);
    }
    template <typename... Ds>
    void bindServos(TBoard* self, const std::tuple<Ds...>& list) {
        bindServosImpl(self, list, std::index_sequence_for<Ds...>{});
    }

    template <typename... Ds, size_t... Is>
    void bindPwmsImpl(TBoard* self, const std::tuple<Ds...>& list, std::index_sequence<Is...>) {
        ((_ports._pwms[Is].port = std::get<Is>(list).template extract<TBoard>(self),
          std::get<Is>(list).template fillSenses<TBoard>(self, _ports._pwms[Is])), ...);
    }
    template <typename... Ds>
    void bindPwms(TBoard* self, const std::tuple<Ds...>& list) {
        bindPwmsImpl(self, list, std::index_sequence_for<Ds...>{});
    }

    template <typename... Ds, size_t... Is>
    void bindHBridgesImpl(TBoard* self, const std::tuple<Ds...>& list, std::index_sequence<Is...>) {
        ((_ports._hbridges[Is].port = std::get<Is>(list).template extract<TBoard>(self),
          std::get<Is>(list).template fillSenses<TBoard>(self, _ports._hbridges[Is])), ...);
    }
    template <typename... Ds>
    void bindHBridges(TBoard* self, const std::tuple<Ds...>& list) {
        bindHBridgesImpl(self, list, std::index_sequence_for<Ds...>{});
    }

    Registry _ports;
};

}  // namespace sfx_core

#endif  // SFX_BOARD_OF_H
