/*
 * BatteryServerT — Generic command handler for battery monitoring
 *
 * Templated on a battery policy (TBattery). Handles the core-range
 * BATTERY_CONFIG packet (0xEE) generically: any board that constructs a
 * BatteryServerT with its chosen policy and registers it via
 * SfxServer::addModuleHandler() automatically gets the BATTERY_CONFIG
 * protocol — chemistry and cellCount adjustments — without hand-rolling
 * the same dispatch in every board.
 *
 * TBattery is a duck-typed concept (no virtual base) — pick whichever
 * implementation matches the board's sensing hardware:
 *   AdcDividerBatteryT<MultiplierMilli>  (battery_monitor.h) — ADC + divider
 *   Ina226Battery                        (ina226_battery.h)  — INA226 channel
 *
 * Required surface (compile-time checked when the template is instantiated):
 *   void update();                          // Poll once per main loop.
 *   uint16_t voltage_mV() const;            // Total pack voltage in mV.
 *   uint8_t  cellCount() const;             // Detected/forced count, 0 = unknown.
 *   bool     isLow() const;                 // Below configured low threshold.
 *   bool     isCritical() const;            // Below critical threshold.
 *   void     setChemistry(BatteryChemistry);
 *   void     setCellCount(uint8_t);         // 0 = re-arm auto-detect.
 *
 * Why a separate policy (not part of BoardServicePolicy): battery
 * monitoring requires a per-board hardware choice (ADC divider vs INA226
 * vs none), so it lives in its own template policy that the board owns
 * and parameterises on its battery sensor type.
 *
 * Usage (slotted into SfxServer's UserPolicies):
 *   using HubFxBatteryPolicy = BatteryServicePolicy<Ina226Battery>;
 *   using HubFxServer = SfxServer<..., HubFxBatteryPolicy, ...>;
 *
 *   server.board().policy<HubFxBatteryPolicy>().bindBattery(batteryMonitor);
 *
 * The policy claims a single packet type (BATTERY_CONFIG).
 */

#ifndef BATTERY_SERVER_H
#define BATTERY_SERVER_H

#include <concepts>
#include <serial/core/system_service.h>   // SystemServicePolicy + ServiceContext
#include <serial/core/core.h>             // CorePacket, SerialError, CommandHandleResult
#include "battery_types.h"

// ============================================================================
// Concept: BatteryPolicy
// ============================================================================
//
// Documents the duck-typed surface BatteryServerT requires. Backends
// implementing this concept can plug straight in (current implementations:
// AdcDividerBatteryT<MultiplierMilli>, Ina226Battery). Adding a new backend
// (e.g. a fuel-gauge IC) that omits a method now fails at template
// instantiation with the missing method clearly named, instead of as a
// link-time error from inside the .ino file.
template <typename T>
concept BatteryPolicy = requires(T t, BatteryChemistry chem, uint8_t cells) {
    { t.update() }                  -> std::same_as<void>;
    { t.voltage_mV() }              -> std::convertible_to<uint16_t>;
    { t.cellCount() }               -> std::convertible_to<uint8_t>;
    { t.isLow() }                   -> std::convertible_to<bool>;
    { t.isCritical() }              -> std::convertible_to<bool>;
    { t.setChemistry(chem) }        -> std::same_as<void>;
    { t.setCellCount(cells) }       -> std::same_as<void>;
};

/**
 * @brief BatteryServicePolicy — SystemServicePolicy for BATTERY_CONFIG.
 *
 * Plugs into `BoardServer<...>` alongside other policies.  Owns the
 * single `CorePacket::BATTERY_CONFIG` (0xEE) wire ID — chemistry +
 * cellCount adjustments — and contributes `CoreCapability::BATTERY`
 * to the board's capability bitmask.
 */
template<typename TBattery>
    requires BatteryPolicy<TBattery>
class BatteryServicePolicy {
public:
    /// Battery presence is advertised in IDENTIFY via this capability bit.
    static constexpr uint32_t kCapabilityBits = CoreCapability::BATTERY;

    BatteryServicePolicy() = default;
    explicit BatteryServicePolicy(TBattery& battery) : _battery(&battery) {}

    BatteryServicePolicy(const BatteryServicePolicy&) = delete;
    BatteryServicePolicy& operator=(const BatteryServicePolicy&) = delete;

    /// Bind the battery sensor after default construction.  Called from
    /// board firmware via `board.policy<BatteryServicePolicy<...>>().bindBattery(...)`.
    void bindBattery(TBattery& battery) { _battery = &battery; }

    /// Access the bound battery policy (e.g. for status broadcasts).
    TBattery& battery() { return *_battery; }
    const TBattery& battery() const { return *_battery; }

    // ── SystemServicePolicy surface ───────────────────────────────────

    bool begin(sfx_core::ServiceContext* ctx) {
        _ctx = ctx;
        return _ctx != nullptr && _battery != nullptr;
    }

    bool ownsType(uint8_t type) const {
        return type == CorePacket::BATTERY_CONFIG;
    }

    CommandHandleResult handle(uint8_t /*type*/,
                               const uint8_t* payload, size_t len) {
        if (len < 2) {
            _ctx->sendNack(SerialError::MISSING_PARAMETER);
            return CommandHandleResult::Handled;
        }
        const uint8_t chemistry = payload[0];
        const uint8_t cellCount = payload[1];

        // Chemistry must be a known enum value; cellCount 0 is "re-arm auto".
        if (chemistry > static_cast<uint8_t>(BatteryChemistry::NIMH)) {
            _ctx->sendNack(SerialError::INVALID_PARAM);
            return CommandHandleResult::Handled;
        }

        _battery->setChemistry(static_cast<BatteryChemistry>(chemistry));
        _battery->setCellCount(cellCount);

        _ctx->sendAck();
        return CommandHandleResult::Handled;
    }

    void update() { /* battery's own update() is called from the board's loop */ }

private:
    sfx_core::ServiceContext* _ctx     = nullptr;
    TBattery*                 _battery = nullptr;
};

/// @deprecated Use `BatteryServicePolicy<TBattery>` and instantiate via
///             `BoardServer<...>`.  Alias kept for in-flight callers.
template<typename TBattery>
using BatteryServerT = BatteryServicePolicy<TBattery>;

#endif // BATTERY_SERVER_H
