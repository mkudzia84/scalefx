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
 * Why a separate handler (not part of CoreCommandServer): CoreCommandServer
 * is non-templated by design (one shared instance per binary). Battery
 * monitoring requires a per-board policy choice (ADC divider vs INA226 vs
 * none), so it lives in its own templated handler that the board owns.
 *
 * Usage:
 *   AdcDividerBatteryT<6000> battery;                    // ÷6.0 divider
 *   BatteryServerT<decltype(battery)> batteryServer(battery);
 *
 *   void setup() {
 *       battery.begin(29, BatteryChemistry::LIPO);
 *       server.begin("LightFX", FIRMWARE_VERSION, BUILD_NUMBER);
 *       server.addModuleHandler(&batteryServer);   // before module handler
 *       server.addModuleHandler(&lightfxServer);
 *   }
 *
 *   void loop() {
 *       server.loop();
 *       battery.update();
 *       lightfxServer.update();
 *   }
 *
 * The handler claims a single packet type (0xEE), so it can sit anywhere
 * in the handler chain after CoreCommandServer.
 */

#ifndef BATTERY_SERVER_H
#define BATTERY_SERVER_H

#include <concepts>
#include <serial/core/bus_server.h>
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

template<typename TBattery>
    requires BatteryPolicy<TBattery>
class BatteryServerT : public BusServer {
public:
    explicit BatteryServerT(TBattery& battery) : _battery(&battery) {}
    ~BatteryServerT() override = default;

    BatteryServerT(const BatteryServerT&) = delete;
    BatteryServerT& operator=(const BatteryServerT&) = delete;

    const char* handlerName() const override { return "BatteryServer"; }

    /// Access the bound battery policy (e.g. for status broadcasts).
    TBattery& battery() { return *_battery; }
    const TBattery& battery() const { return *_battery; }

protected:
    uint8_t moduleRangeLow()  const override { return CorePacket::BATTERY_CONFIG; }
    uint8_t moduleRangeHigh() const override { return CorePacket::BATTERY_CONFIG; }

    CommandHandleResult handleModulePacket(uint8_t type,
                                           const uint8_t* payload,
                                           size_t len) override {
        if (type != CorePacket::BATTERY_CONFIG) {
            return CommandHandleResult::NotMyCommand;
        }
        SFX_REQUIRE_LEN(2);

        uint8_t chemistry = payload[0];
        uint8_t cellCount = payload[1];

        // Chemistry must be a known enum value; cellCount 0 is "re-arm auto".
        SFX_VALIDATE(chemistry <= static_cast<uint8_t>(BatteryChemistry::NIMH),
                     SerialError::INVALID_PARAM);

        _battery->setChemistry(static_cast<BatteryChemistry>(chemistry));
        _battery->setCellCount(cellCount);

        sendAck();
        return CommandHandleResult::Handled;
    }

private:
    TBattery* _battery;
};

#endif // BATTERY_SERVER_H
