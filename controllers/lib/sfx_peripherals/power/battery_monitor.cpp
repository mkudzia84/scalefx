/*
 * Battery Monitor Library - Implementation
 *
 * ADC-based battery voltage monitor for LiPo and Li-Ion packs.
 * See battery_monitor.h for API documentation.
 */

#include "battery_monitor.h"
#include "platform/sfx_platform.h"

// ============================================================================
// ADC Constants
// ============================================================================

static constexpr uint16_t ADC_REF_mV       = 3300;   // 3.3V reference
static constexpr uint16_t ADC_LEVELS       = 4096;   // 12-bit ADC (2^12)
static constexpr uint8_t  ADC_OVERSAMPLE    = 4;      // Samples per read (noise reduction)

// ============================================================================
// Cell Detection Constants
// ============================================================================

static constexpr uint8_t  MAX_CELLS         = 6;      // Maximum supported series cells
static constexpr uint16_t MIN_DETECT_mV     = 3000;   // Minimum voltage for auto-detect (mV)

// ============================================================================
// Alert Hysteresis
// ============================================================================

static constexpr uint16_t HYSTERESIS_PER_CELL_mV = 50;  // Re-arm margin above threshold

// ============================================================================
// USB Power Detection
// ============================================================================

// ============================================================================
// Initialization
// ============================================================================

void BatteryMonitor::begin(uint8_t adcPin, float dividerMultiplier,
                           BatteryChemistry chemistry) {
    _adcPin = adcPin;
    _dividerMultiplier = dividerMultiplier;
    _chemistry = chemistry;
    _profile = BatteryProfiles::forChemistry(chemistry);

    // Reset state
    _voltage_mV = 0;
    _cellCount = 0;
    _cellCountLocked = false;
    _lowTriggered = false;
    _criticalTriggered = false;
    _lastRead_ms = 0;

    // Configure ADC pin
    pinMode(_adcPin, INPUT);

    // Configure VBUS detect pin (platform-specific)
    if (SFX_VBUS_PIN != 0xFF) {
        pinMode(SFX_VBUS_PIN, INPUT);
        _usbPowered = digitalRead(SFX_VBUS_PIN) == HIGH;
    } else {
        _usbPowered = false;  // No VBUS detect on this platform
    }

    // Perform initial reading and cell detection
    _voltage_mV = readAdc_mV();
    if (_voltage_mV >= MIN_DETECT_mV) {
        _cellCount = detectCellCount(_voltage_mV);
        if (_cellCount > 0) {
            _cellCountLocked = true;
        }
    }
}

// ============================================================================
// Configuration
// ============================================================================

void BatteryMonitor::setCellCount(uint8_t cells) {
    if (cells >= 1 && cells <= MAX_CELLS) {
        _cellCount = cells;
        _cellCountLocked = true;
        _manualCellCount = true;
    }
}

void BatteryMonitor::setReadInterval_ms(uint16_t interval_ms) {
    _readInterval_ms = interval_ms;
}

void BatteryMonitor::setLowThreshold_mV(uint16_t perCell_mV) {
    _customLow_mV = perCell_mV;
}

void BatteryMonitor::setCriticalThreshold_mV(uint16_t perCell_mV) {
    _customCritical_mV = perCell_mV;
}

// ============================================================================
// Callbacks
// ============================================================================

void BatteryMonitor::onLowVoltage(AlertCallback cb) {
    _onLow = cb;
}

void BatteryMonitor::onCriticalVoltage(AlertCallback cb) {
    _onCritical = cb;
}

// ============================================================================
// Update
// ============================================================================

void BatteryMonitor::update() {
    uint32_t now = millis();
    if (now - _lastRead_ms < _readInterval_ms) return;
    _lastRead_ms = now;

    _voltage_mV = readAdc_mV();
    if (SFX_VBUS_PIN != 0xFF) {
        _usbPowered = digitalRead(SFX_VBUS_PIN) == HIGH;
    }

    // Auto-detect cell count on first valid reading (if not manually set)
    if (!_cellCountLocked && _voltage_mV >= MIN_DETECT_mV) {
        _cellCount = detectCellCount(_voltage_mV);
        if (_cellCount > 0) {
            _cellCountLocked = true;
        }
    }

    // Skip threshold checks until cell count is known
    if (_cellCount == 0) return;

    uint16_t cellV = _voltage_mV / _cellCount;
    uint16_t lowTh = effectiveLow_mV();
    uint16_t critTh = effectiveCritical_mV();

    // --- Low voltage check with hysteresis ---
    if (!_lowTriggered && cellV <= lowTh) {
        _lowTriggered = true;
        if (_onLow) _onLow(_voltage_mV, _cellCount);
    } else if (_lowTriggered && cellV > lowTh + HYSTERESIS_PER_CELL_mV) {
        _lowTriggered = false;
    }

    // --- Critical voltage check with hysteresis ---
    if (!_criticalTriggered && cellV <= critTh) {
        _criticalTriggered = true;
        if (_onCritical) _onCritical(_voltage_mV, _cellCount);
    } else if (_criticalTriggered && cellV > critTh + HYSTERESIS_PER_CELL_mV) {
        _criticalTriggered = false;
    }
}

// ============================================================================
// Measurements
// ============================================================================

uint16_t BatteryMonitor::cellVoltage_mV() const {
    if (_cellCount == 0) return 0;
    return _voltage_mV / _cellCount;
}

bool BatteryMonitor::isLow() const {
    if (_cellCount == 0) return false;
    return (_voltage_mV / _cellCount) <= effectiveLow_mV();
}

bool BatteryMonitor::isCritical() const {
    if (_cellCount == 0) return false;
    return (_voltage_mV / _cellCount) <= effectiveCritical_mV();
}

uint8_t BatteryMonitor::percentage() const {
    if (_cellCount == 0) return 0;

    uint16_t cellV = _voltage_mV / _cellCount;
    uint16_t full = _profile.fullCharge_mV;
    uint16_t crit = effectiveCritical_mV();

    if (cellV >= full) return 100;
    if (cellV <= crit) return 0;

    // Linear interpolation: 0% at critical, 100% at full charge
    return (uint8_t)((uint32_t)(cellV - crit) * 100 / (full - crit));
}

bool BatteryMonitor::isPresent() const {
    // No voltage at all → no battery
    if (_voltage_mV < MIN_DETECT_mV) return false;
    // USB powered with no manual cell count → likely phantom reading from VSYS/3
    if (_usbPowered && !_manualCellCount) return false;
    return true;
}

// ============================================================================
// Private: ADC Reading
// ============================================================================

/**
 * @brief Read ADC with oversampling and apply divider conversion
 *
 * Takes ADC_OVERSAMPLE readings and averages to reduce noise.
 * Conversion: mV = (raw × ADC_REF_mV × dividerMultiplier) / ADC_LEVELS
 *
 * @return Total battery voltage in millivolts
 */
uint16_t BatteryMonitor::readAdc_mV() {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < ADC_OVERSAMPLE; i++) {
        sum += analogRead(_adcPin);
    }
    uint16_t raw = sum / ADC_OVERSAMPLE;

    // mV = raw × (3300 × divider) / 4096
    uint32_t scale = (uint32_t)(ADC_REF_mV * _dividerMultiplier);
    return (uint16_t)((uint32_t)raw * scale / ADC_LEVELS);
}

// ============================================================================
// Private: Cell Count Detection
// ============================================================================

/**
 * @brief Estimate series cell count from pack voltage
 *
 * Uses the nominal voltage for the configured chemistry to determine how many
 * cells are in series: cells = round(voltage_mV / nominal_mV).
 *
 * Reliable when battery is above ~3.3V/cell. For deeply discharged packs
 * (<3.0V/cell), detection may be off by ±1 cell — use setCellCount() instead.
 *
 * @param voltage_mV  Measured total pack voltage in millivolts
 * @return Detected cell count (1–6), or 0 if voltage is too low
 */
uint8_t BatteryMonitor::detectCellCount(uint16_t voltage_mV) {
    if (voltage_mV < MIN_DETECT_mV) return 0;

    uint16_t nominal = _profile.nominal_mV;

    // Round to nearest cell count: (voltage + nominal/2) / nominal
    uint8_t cells = (voltage_mV + nominal / 2) / nominal;

    // Clamp to valid range
    if (cells < 1) cells = 1;
    if (cells > MAX_CELLS) cells = MAX_CELLS;

    return cells;
}

// ============================================================================
// Private: Effective Thresholds
// ============================================================================

uint16_t BatteryMonitor::effectiveLow_mV() const {
    return (_customLow_mV > 0) ? _customLow_mV : _profile.low_mV;
}

uint16_t BatteryMonitor::effectiveCritical_mV() const {
    return (_customCritical_mV > 0) ? _customCritical_mV : _profile.critical_mV;
}
