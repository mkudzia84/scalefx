/*
 * BatteryStateMachine — Implementation
 *
 * See battery_state_machine.h for design notes.
 */

#include "battery_state_machine.h"

using namespace BatteryStateMachineConfig;

void BatteryStateMachine::setChemistry(BatteryChemistry chemistry) {
    _chemistry = chemistry;
    _profile = BatteryProfiles::forChemistry(chemistry);
    if (!_manualCellCount) {
        // Nominal voltage just changed — previous detection may be wrong.
        _cellCount = 0;
        _cellCountLocked = false;
    }
    _lowTriggered = false;
    _criticalTriggered = false;
}

void BatteryStateMachine::setCellCount(uint8_t cells) {
    if (cells == 0) {
        _manualCellCount = false;
        _cellCount = 0;
        _cellCountLocked = false;
        return;
    }
    if (cells <= MAX_CELLS) {
        _cellCount = cells;
        _cellCountLocked = true;
        _manualCellCount = true;
    }
}

void BatteryStateMachine::feed(uint16_t voltage_mV) {
    _voltage_mV = voltage_mV;

    if (!_cellCountLocked && _voltage_mV >= MIN_DETECT_mV) {
        _cellCount = detectCellCount(_voltage_mV);
        if (_cellCount > 0) _cellCountLocked = true;
    }

    if (_cellCount == 0) return;

    uint16_t cellV  = _voltage_mV / _cellCount;
    uint16_t lowTh  = effectiveLow_mV();
    uint16_t critTh = effectiveCritical_mV();

    if (!_lowTriggered && cellV <= lowTh) {
        _lowTriggered = true;
        if (_onLow) _onLow(_voltage_mV, _cellCount);
    } else if (_lowTriggered && cellV > lowTh + HYSTERESIS_PER_CELL_mV) {
        _lowTriggered = false;
    }

    if (!_criticalTriggered && cellV <= critTh) {
        _criticalTriggered = true;
        if (_onCritical) _onCritical(_voltage_mV, _cellCount);
    } else if (_criticalTriggered && cellV > critTh + HYSTERESIS_PER_CELL_mV) {
        _criticalTriggered = false;
    }
}

bool BatteryStateMachine::isLow() const {
    if (_cellCount == 0) return false;
    return (_voltage_mV / _cellCount) <= effectiveLow_mV();
}

bool BatteryStateMachine::isCritical() const {
    if (_cellCount == 0) return false;
    return (_voltage_mV / _cellCount) <= effectiveCritical_mV();
}

uint8_t BatteryStateMachine::percentage() const {
    if (_cellCount == 0) return 0;
    uint16_t cellV = _voltage_mV / _cellCount;
    uint16_t full  = _profile.fullCharge_mV;
    uint16_t crit  = effectiveCritical_mV();
    if (cellV >= full) return 100;
    if (cellV <= crit) return 0;
    return (uint8_t)((uint32_t)(cellV - crit) * 100 / (full - crit));
}

uint8_t BatteryStateMachine::detectCellCount(uint16_t voltage_mV) const {
    if (voltage_mV < MIN_DETECT_mV) return 0;
    uint16_t nominal = _profile.nominal_mV;
    uint8_t cells = (voltage_mV + nominal / 2) / nominal;
    if (cells < 1) cells = 1;
    if (cells > MAX_CELLS) cells = MAX_CELLS;
    return cells;
}
