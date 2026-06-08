/*
 * Battery types — chemistry enum + per-cell voltage profiles
 *
 * Extracted so battery_state_machine.h and battery_monitor.h can both depend
 * on these definitions without circular includes. The wire-format values for
 * BatteryChemistry are stable (sent over BATTERY_CONFIG protocol).
 */

#ifndef BATTERY_TYPES_H
#define BATTERY_TYPES_H

#include <cstdint>   // uint8_t / uint16_t — NOT transitively present on the Pico
                     // toolchain (ESP-IDF pulled it in, so HubFX hid this break)
#include <cstring>

/**
 * @brief Battery chemistry type
 *
 * Determines default voltage thresholds per cell:
 *   LiPo:   4.20V full, 3.70V nominal, 3.20V low, 3.00V critical
 *   Li-Ion: 4.20V full, 3.60V nominal, 3.20V low, 2.80V critical
 *   NiMH:   1.40V full, 1.20V nominal, 1.00V low, 0.90V critical
 *
 * Wire-format values (sent over BATTERY_CONFIG protocol) are stable:
 *   0 = LIPO, 1 = LI_ION, 2 = NIMH.
 */
enum class BatteryChemistry : uint8_t {
    LIPO   = 0, ///< Lithium Polymer — 3.0V/cell damage threshold
    LI_ION = 1, ///< Lithium Ion — 2.5V/cell damage threshold
    NIMH   = 2, ///< Nickel Metal Hydride — 0.9V/cell cutoff
};

/**
 * @brief Per-cell voltage thresholds in millivolts.
 */
struct BatteryProfile {
    uint16_t fullCharge_mV;     ///< Full charge voltage (e.g., 4200 mV)
    uint16_t nominal_mV;        ///< Nominal voltage, used for cell count detection
    uint16_t low_mV;            ///< Low voltage warning threshold
    uint16_t critical_mV;       ///< Critical voltage cutoff threshold
};

namespace BatteryProfiles {
    constexpr BatteryProfile LIPO    = { 4200, 3700, 3200, 3000 };
    constexpr BatteryProfile LI_ION  = { 4200, 3600, 3200, 2800 };
    constexpr BatteryProfile NIMH    = { 1400, 1200, 1000,  900 };

    constexpr BatteryProfile forChemistry(BatteryChemistry chem) {
        switch (chem) {
            case BatteryChemistry::LI_ION: return LI_ION;
            case BatteryChemistry::NIMH:   return NIMH;
            case BatteryChemistry::LIPO:
            default:                       return LIPO;
        }
    }
}

/// Parse a canonical YAML chemistry string ("lipo" / "liion" / "nimh") to the
/// enum. Unknown values fall back to LIPO. Mirrors core.ChemistryFromString in Go.
inline BatteryChemistry parseBatteryChemistry(const char* s) {
    if (s == nullptr) return BatteryChemistry::LIPO;
    if (strcmp(s, "liion") == 0) return BatteryChemistry::LI_ION;
    if (strcmp(s, "nimh")  == 0) return BatteryChemistry::NIMH;
    return BatteryChemistry::LIPO;
}

/// Map a chemistry enum back to its canonical YAML/CLI string.
inline const char* batteryChemistryName(BatteryChemistry chem) {
    switch (chem) {
        case BatteryChemistry::LI_ION: return "liion";
        case BatteryChemistry::NIMH:   return "nimh";
        case BatteryChemistry::LIPO:
        default:                       return "lipo";
    }
}

#endif // BATTERY_TYPES_H
