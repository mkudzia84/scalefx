/*
 * Battery Monitor Library - Header
 *
 * ADC-based battery voltage monitor for LiPo and Li-Ion packs.
 * Reads voltage through a resistor divider, auto-detects cell count (1S–6S),
 * and monitors for low/critical voltage to prevent excessive discharge.
 *
 * Features:
 *   - ADC reading with configurable resistor divider ratio
 *   - LiPo and Li-Ion chemistry profiles (configurable thresholds)
 *   - Auto-detect cell count from measured voltage (lockable)
 *   - Low and critical voltage callbacks with hysteresis
 *   - Estimated state-of-charge percentage
 *   - Oversampled ADC reads for noise reduction
 *
 * Hardware assumptions:
 *   - RP2040 ADC: 12-bit, 3.3V reference
 *   - Resistive voltage divider between battery and ADC pin
 *   - Divider multiplier = (R_top + R_bottom) / R_bottom
 *     Example: 50kΩ/10kΩ divider → multiplier = 6.0
 *
 * Usage:
 *   BatteryMonitor battery;
 *   battery.begin(29, 6.0f);                 // GP29, ÷6 divider, default LiPo
 *   battery.begin(29, 6.0f, BatteryChemistry::LI_ION);  // Li-Ion chemistry
 *
 *   // Optional: manual cell count override
 *   battery.setCellCount(3);                 // Force 3S (skips auto-detect)
 *
 *   // Optional: custom thresholds (per cell)
 *   battery.setLowThreshold_mV(3400);        // Override default low warning
 *   battery.setCriticalThreshold_mV(3200);   // Override default critical cutoff
 *
 *   // Optional: voltage alert callbacks
 *   battery.onLowVoltage([](uint16_t v, uint8_t cells) {
 *       Serial.printf("LOW: %dmV (%dS)\n", v, cells);
 *   });
 *   battery.onCriticalVoltage([](uint16_t v, uint8_t cells) {
 *       Serial.printf("CRITICAL: %dmV (%dS)\n", v, cells);
 *   });
 *
 *   // In loop():
 *   battery.update();                        // Reads ADC at configured interval
 *   uint16_t v = battery.voltage_mV();       // Total pack voltage
 *   uint16_t cv = battery.cellVoltage_mV();  // Per-cell average
 *   uint8_t pct = battery.percentage();      // Estimated SOC (0-100%)
 *   bool low = battery.isLow();              // Below low threshold?
 *   bool crit = battery.isCritical();        // Below critical threshold?
 */

#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <Arduino.h>
#include <functional>

// ============================================================================
// Battery Chemistry Enum
// ============================================================================

/**
 * @brief Battery chemistry type
 *
 * Determines default voltage thresholds per cell:
 *   LiPo:   4.20V full, 3.70V nominal, 3.20V low, 3.00V critical
 *   Li-Ion: 4.20V full, 3.60V nominal, 3.20V low, 2.80V critical
 */
enum class BatteryChemistry : uint8_t {
    LIPO,       ///< Lithium Polymer — 3.0V/cell damage threshold
    LI_ION,     ///< Lithium Ion — 2.5V/cell damage threshold
};

// ============================================================================
// Battery Profile (per-cell voltage thresholds in mV)
// ============================================================================

/**
 * @brief Per-cell voltage thresholds for a battery chemistry
 *
 * All values are in millivolts per cell.
 */
struct BatteryProfile {
    uint16_t fullCharge_mV;     ///< Full charge voltage (e.g., 4200 mV)
    uint16_t nominal_mV;        ///< Nominal voltage, used for cell count detection
    uint16_t low_mV;            ///< Low voltage warning threshold
    uint16_t critical_mV;       ///< Critical voltage cutoff threshold
};

// ============================================================================
// Built-in Chemistry Profiles
// ============================================================================

namespace BatteryProfiles {
    /// LiPo: 4.2V/cell full, 3.7V nominal, 3.2V low warning, 3.0V critical
    constexpr BatteryProfile LIPO    = { 4200, 3700, 3200, 3000 };

    /// Li-Ion: 4.2V/cell full, 3.6V nominal, 3.2V low warning, 2.8V critical
    constexpr BatteryProfile LI_ION  = { 4200, 3600, 3200, 2800 };

    /// Get profile for a chemistry enum value
    constexpr BatteryProfile forChemistry(BatteryChemistry chem) {
        return (chem == BatteryChemistry::LI_ION) ? LI_ION : LIPO;
    }
}

// ============================================================================
// BatteryMonitor Class
// ============================================================================

/**
 * @brief ADC-based battery voltage monitor with cell detection and low-voltage alerts
 *
 * Reads battery voltage through a resistor divider on an ADC pin, auto-detects
 * the number of series cells, and fires callbacks when voltage drops below
 * configurable thresholds.
 *
 * Cell count auto-detection uses the nominal voltage per cell for the configured
 * chemistry: `cells = round(voltage / nominal)`. For reliable detection, the
 * battery should be above ~3.3V/cell when begin() or the first update() runs.
 * If the battery may be deeply discharged, use setCellCount() to set manually.
 */
class BatteryMonitor {
public:
    BatteryMonitor() = default;

    /// Callback signature: (total pack voltage in mV, detected cell count)
    using AlertCallback = std::function<void(uint16_t voltage_mV, uint8_t cellCount)>;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize the battery monitor
     *
     * Configures the ADC pin, sets the divider multiplier, selects the chemistry
     * profile, and performs an initial voltage reading + cell count detection.
     *
     * @param adcPin              GPIO pin connected to divider output (must be ADC-capable)
     * @param dividerMultiplier   Voltage divider ratio: (R_top + R_bottom) / R_bottom
     *                            Example: 50kΩ/10kΩ → 6.0
     * @param chemistry           Battery chemistry (default: LIPO)
     */
    void begin(uint8_t adcPin, float dividerMultiplier,
               BatteryChemistry chemistry = BatteryChemistry::LIPO);

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Manually set the cell count (overrides auto-detection)
     *
     * Use this when the battery may be deeply discharged and auto-detection
     * would be unreliable, or when the pack configuration is known.
     *
     * @param cells  Number of cells in series (1–6)
     */
    void setCellCount(uint8_t cells);

    /**
     * @brief Set the ADC read interval
     * @param interval_ms  Milliseconds between ADC reads (default: 500)
     */
    void setReadInterval_ms(uint16_t interval_ms);

    /**
     * @brief Override the low voltage warning threshold (per cell)
     * @param perCell_mV  Low threshold in millivolts per cell
     */
    void setLowThreshold_mV(uint16_t perCell_mV);

    /**
     * @brief Override the critical voltage cutoff threshold (per cell)
     * @param perCell_mV  Critical threshold in millivolts per cell
     */
    void setCriticalThreshold_mV(uint16_t perCell_mV);

    // ========================================================================
    // Callbacks
    // ========================================================================

    /**
     * @brief Register callback for low voltage warning
     *
     * Fires once when per-cell voltage drops below the low threshold.
     * Re-arms when voltage recovers above threshold + hysteresis (50mV/cell).
     */
    void onLowVoltage(AlertCallback cb);

    /**
     * @brief Register callback for critical voltage alert
     *
     * Fires once when per-cell voltage drops below the critical threshold.
     * Re-arms when voltage recovers above threshold + hysteresis (50mV/cell).
     */
    void onCriticalVoltage(AlertCallback cb);

    // ========================================================================
    // Update (call in loop)
    // ========================================================================

    /**
     * @brief Read ADC and check voltage thresholds
     *
     * Call this every loop() iteration. Internally rate-limited by readInterval_ms.
     */
    void update();

    // ========================================================================
    // Measurements
    // ========================================================================

    /**
     * @brief Get total pack voltage in millivolts
     */
    uint16_t voltage_mV() const { return _voltage_mV; }

    /**
     * @brief Get average per-cell voltage in millivolts
     *
     * Returns 0 if cell count is not yet detected.
     */
    uint16_t cellVoltage_mV() const;

    /**
     * @brief Get detected (or manually set) cell count
     * @return Cell count (1–6), or 0 if not yet detected
     */
    uint8_t cellCount() const { return _cellCount; }

    /**
     * @brief Get the active battery chemistry
     */
    BatteryChemistry chemistry() const { return _chemistry; }

    /**
     * @brief Get the active voltage profile
     */
    const BatteryProfile& profile() const { return _profile; }

    // ========================================================================
    // Voltage Status
    // ========================================================================

    /**
     * @brief Check if voltage is below the low warning threshold
     */
    bool isLow() const;

    /**
     * @brief Check if voltage is below the critical cutoff threshold
     */
    bool isCritical() const;

    /**
     * @brief Estimated state-of-charge percentage (0–100%)
     *
     * Linear interpolation between critical (0%) and full charge (100%).
     * Note: Real discharge curves are non-linear; this is a rough estimate
     * suitable for warning displays, not a precision fuel gauge.
     */
    uint8_t percentage() const;

private:
    // ADC reading and cell detection
    uint16_t readAdc_mV();
    uint8_t  detectCellCount(uint16_t voltage_mV);

    // Effective thresholds (custom overrides or profile defaults)
    uint16_t effectiveLow_mV() const;
    uint16_t effectiveCritical_mV() const;

    // ---- Configuration ----
    uint8_t  _adcPin = 0;
    float    _dividerMultiplier = 1.0f;
    BatteryChemistry _chemistry = BatteryChemistry::LIPO;
    BatteryProfile   _profile = BatteryProfiles::LIPO;

    // ---- State ----
    uint16_t _voltage_mV = 0;           // Total pack voltage (mV)
    uint8_t  _cellCount = 0;            // Detected/configured cell count
    bool     _cellCountLocked = false;   // True after detection or manual set

    // ---- Timing ----
    uint16_t _readInterval_ms = 500;
    uint32_t _lastRead_ms = 0;

    // ---- Custom thresholds (per cell, 0 = use profile default) ----
    uint16_t _customLow_mV = 0;
    uint16_t _customCritical_mV = 0;

    // ---- Callbacks ----
    AlertCallback _onLow = nullptr;
    AlertCallback _onCritical = nullptr;
    bool _lowTriggered = false;
    bool _criticalTriggered = false;
};

#endif // BATTERY_MONITOR_H
