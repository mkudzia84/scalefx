/*
 * NoBattery — null battery policy for boards without a battery sensor.
 *
 * CoreServer is templated on a `TBattery` policy that defaults to
 * `NoBattery` so boards that don't have an onboard battery sensor get
 * the right wire-protocol behaviour for free:
 *   - BATTERY_INFO_RESP returns `present = 0`
 *   - BATTERY_RECONFIGURE NACKs with `BATTERY_NOT_PRESENT`
 *   - COMPONENT_STATUS_BROADCAST emits a single `0` byte for the battery
 *     section (and no further per-battery bytes)
 *   - INIT_READY does NOT advertise `CoreCapability::BATTERY`
 *
 * The stub satisfies the `BatteryPolicy` concept (battery_server.h) plus
 * the extended alert / threshold / cell-detect surface the CoreServer
 * uses to populate BATTERY_INFO_RESP and the unified status section.
 *
 * Boards with a real battery use one of the live policies instead:
 *   AdcDividerBatteryT<MultiplierMilli>  — ADC + resistor divider
 *   Ina226Battery                        — INA226 channel
 */

#ifndef SFX_NO_BATTERY_H
#define SFX_NO_BATTERY_H

#include <cstdint>
#include "battery_types.h"

class NoBattery {
public:
    using AlertCallback = void(*)(uint16_t, uint8_t);   ///< unused — kept for ABI compatibility

    /// Compile-time tag — CoreServer detects "no battery" via
    /// `std::is_same_v<TBattery, NoBattery>` rather than a runtime
    /// `kPresent` check, so this serves as documentation only.
    static constexpr bool kPresent = false;

    // ── BatteryPolicy concept (battery_server.h) ────────────────────
    void     update()                                    {}
    uint16_t voltage_mV()       const                    { return 0; }
    uint8_t  cellCount()        const                    { return 0; }
    bool     isLow()            const                    { return false; }
    bool     isCritical()       const                    { return false; }
    void     setChemistry(BatteryChemistry)              {}
    void     setCellCount(uint8_t)                       {}

    // ── Extended surface used by CoreServer ────────────────────────
    uint16_t cellVoltage_mV()   const                    { return 0; }
    uint8_t  percentage()       const                    { return 0; }
    BatteryChemistry chemistry() const                   { return BatteryChemistry::LIPO; }
    bool     isLowTriggered()      const                 { return false; }
    bool     isCriticalTriggered() const                 { return false; }
    bool     isCellCountManual()   const                 { return false; }
    bool     isUsbPowered()        const                 { return false; }
    bool     isPresent()           const                 { return false; }

    void setLowThreshold_mV(uint16_t)                    {}
    void setCriticalThreshold_mV(uint16_t)               {}

    template <typename Fn> void onLowVoltage(Fn)         {}
    template <typename Fn> void onCriticalVoltage(Fn)    {}
};

#endif  // SFX_NO_BATTERY_H
