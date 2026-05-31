/*
 * BatteryStateMachine — Shared battery-monitoring state logic
 *
 * Sensor-agnostic state machine for cell-count auto-detection and
 * low/critical voltage alerting with hysteresis. Used by both battery
 * sensors (AdcDividerBatteryT, Ina226Battery) so the alert, cell-detect,
 * and chemistry-switching behavior is identical across sensors.
 *
 * The sensor class owns the *measurement* (e.g. ADC oversampling, INA226 I2C
 * read) and feeds the result to BatteryStateMachine::feed(voltage_mV). The
 * state machine handles everything downstream: cell-count locking, alert
 * trigger/re-arm hysteresis, and SOC% interpolation.
 */

#ifndef BATTERY_STATE_MACHINE_H
#define BATTERY_STATE_MACHINE_H

#include <functional>
#include "battery_types.h"  // BatteryChemistry / BatteryProfile / BatteryProfiles

namespace BatteryStateMachineConfig {
    constexpr uint8_t  MAX_CELLS              = 6;
    constexpr uint16_t MIN_DETECT_mV          = 3000;  // Below this, voltage is treated as no-pack
    constexpr uint16_t HYSTERESIS_PER_CELL_mV = 50;    // Re-arm margin above threshold
}

/**
 * @brief Sensor-agnostic battery state machine.
 *
 * Owns: chemistry, cell-count auto-detect/manual override, low/critical
 * thresholds (per-cell), alert trigger state with hysteresis, SOC% calc.
 *
 * Does NOT own measurement — the policy class polls its sensor and calls
 * feed(voltage_mV) at whatever cadence is appropriate for that hardware.
 */
class BatteryStateMachine {
public:
    using AlertCallback = std::function<void(uint16_t voltage_mV, uint8_t cellCount)>;

    BatteryStateMachine() = default;

    // ─── Configuration (mirrors BatteryMonitor's contract) ──────────────────

    void setChemistry(BatteryChemistry chemistry);
    void setCellCount(uint8_t cells);                  // 0 = re-arm auto-detect
    void setLowThreshold_mV(uint16_t perCell_mV)       { _customLow_mV = perCell_mV; }
    void setCriticalThreshold_mV(uint16_t perCell_mV)  { _customCritical_mV = perCell_mV; }

    void onLowVoltage(AlertCallback cb)       { _onLow = cb; }
    void onCriticalVoltage(AlertCallback cb)  { _onCritical = cb; }

    // ─── Sensor input ───────────────────────────────────────────────────────

    /**
     * @brief Feed a fresh voltage reading; runs cell-detect + alert logic.
     *
     * Cheap to call — internal rate-limiting belongs to the policy that
     * owns the sensor (different sensors have different costs).
     */
    void feed(uint16_t voltage_mV);

    // ─── Queries ────────────────────────────────────────────────────────────

    uint16_t voltage_mV()       const { return _voltage_mV; }
    uint16_t cellVoltage_mV()   const { return _cellCount ? _voltage_mV / _cellCount : 0; }
    uint8_t  cellCount()        const { return _cellCount; }
    BatteryChemistry chemistry() const { return _chemistry; }
    const BatteryProfile& profile() const { return _profile; }

    bool isLow()      const;
    bool isCritical() const;
    uint8_t percentage() const;

    bool isLowTriggered()      const { return _lowTriggered; }
    bool isCriticalTriggered() const { return _criticalTriggered; }
    bool isCellCountManual()   const { return _manualCellCount; }

private:
    uint16_t effectiveLow_mV()      const { return _customLow_mV      ? _customLow_mV      : _profile.low_mV;      }
    uint16_t effectiveCritical_mV() const { return _customCritical_mV ? _customCritical_mV : _profile.critical_mV; }
    uint8_t  detectCellCount(uint16_t voltage_mV) const;

    BatteryChemistry _chemistry = BatteryChemistry::LIPO;
    BatteryProfile   _profile   = BatteryProfiles::LIPO;

    uint16_t _voltage_mV       = 0;
    uint8_t  _cellCount        = 0;
    bool     _cellCountLocked  = false;
    bool     _manualCellCount  = false;

    uint16_t _customLow_mV      = 0;
    uint16_t _customCritical_mV = 0;

    AlertCallback _onLow;
    AlertCallback _onCritical;
    bool _lowTriggered      = false;
    bool _criticalTriggered = false;
};

#endif // BATTERY_STATE_MACHINE_H
