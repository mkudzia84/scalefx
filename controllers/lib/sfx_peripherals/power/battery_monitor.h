/*
 * Battery Monitor — ADC + resistor-divider battery voltage sensor
 *
 * A battery sensor for boards that sense voltage via an ADC pin behind a
 * resistor divider. Conforms to the duck-typed concept consumed by
 * BatteryServerT<TBattery> (see battery_server.h for the required surface).
 * Internally delegates cell-detect / hysteresis / alert logic to
 * BatteryStateMachine so behavior is identical to the INA226-based sensor.
 *
 * Hardware assumptions:
 *   - RP2040/RP2350 ADC: 12-bit, 3.3V reference
 *   - Resistive voltage divider between battery and ADC pin
 *   - Divider multiplier = (R_top + R_bottom) / R_bottom
 *     Example: 50kΩ/10kΩ divider → multiplier = 6.0
 *
 * The divider multiplier is a compile-time template parameter expressed in
 * milli-units (×1000) so we can use a non-type template parameter without
 * needing C++20 float NTTPs:
 *
 *   ÷6.0   →  AdcDividerBatteryT<6000>
 *   ÷5.1   →  AdcDividerBatteryT<5100>
 *   ÷11.0  →  AdcDividerBatteryT<11000>
 *
 * This lets the compiler inline the divider math; each board picks its own
 * specialization. Convenience aliases for common ratios are at the bottom of
 * the header.
 *
 * Usage:
 *   AdcDividerBatteryT<6000> battery;        // ÷6 divider (e.g. 50k/10k)
 *   battery.begin(29);                       // GP29, default LiPo
 *
 *   // Runtime reconfiguration (does NOT re-init the ADC):
 *   battery.setChemistry(BatteryChemistry::LI_ION);
 *   battery.setCellCount(3);                 // Force 3S
 *   battery.setCellCount(0);                 // Re-arm auto-detect
 *
 *   battery.onLowVoltage([](uint16_t v, uint8_t cells) { ... });
 *
 *   // In loop():
 *   battery.update();                        // ADC read + state machine
 *   uint16_t v = battery.voltage_mV();
 *   bool low = battery.isLow();
 */

#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <Arduino.h>
#include <functional>

#include "battery_types.h"          // BatteryChemistry / BatteryProfile / BatteryProfiles
#include "battery_state_machine.h"  // BatteryStateMachine
#include "platform/sfx_platform.h"  // SFX_VBUS_PIN

// ============================================================================
// AdcDividerBatteryT — battery sensor for ADC + resistor-divider hardware
// Templated on the divider multiplier in milli-units (×1000).
// ============================================================================

template<uint32_t MultiplierMilli>
class AdcDividerBatteryT {
public:
    using AlertCallback = BatteryStateMachine::AlertCallback;

    /// Divider multiplier (R_top + R_bottom) / R_bottom, encoded ×1000.
    static constexpr uint32_t MULTIPLIER_MILLI = MultiplierMilli;
    static constexpr float    DIVIDER_MULTIPLIER = MultiplierMilli / 1000.0f;

    AdcDividerBatteryT() = default;

    /**
     * @brief Initialize ADC pin and perform initial reading.
     *
     * @param adcPin     GPIO pin connected to divider output (ADC-capable)
     * @param chemistry  Initial chemistry profile (default: LIPO)
     */
    void begin(uint8_t adcPin, BatteryChemistry chemistry = BatteryChemistry::LIPO) {
        _adcPin = adcPin;
        _lastRead_ms = 0;

        _sm = BatteryStateMachine{};
        _sm.setChemistry(chemistry);

        pinMode(_adcPin, INPUT);

        if (SFX_VBUS_PIN != 0xFF) {
            pinMode(SFX_VBUS_PIN, INPUT);
            _usbPowered = digitalRead(SFX_VBUS_PIN) == HIGH;
        } else {
            _usbPowered = false;
        }

        _sm.feed(readAdc_mV());
    }

    // ─── Configuration ──────────────────────────────────────────────────────
    void setChemistry(BatteryChemistry chemistry)        { _sm.setChemistry(chemistry); }
    void setCellCount(uint8_t cells)                     { _sm.setCellCount(cells); }
    void setReadInterval_ms(uint16_t interval_ms)        { _readInterval_ms = interval_ms; }
    void setLowThreshold_mV(uint16_t perCell_mV)         { _sm.setLowThreshold_mV(perCell_mV); }
    void setCriticalThreshold_mV(uint16_t perCell_mV)    { _sm.setCriticalThreshold_mV(perCell_mV); }

    // ─── Alerts ─────────────────────────────────────────────────────────────
    void onLowVoltage(AlertCallback cb)      { _sm.onLowVoltage(cb); }
    void onCriticalVoltage(AlertCallback cb) { _sm.onCriticalVoltage(cb); }

    // ─── Update ─────────────────────────────────────────────────────────────
    void update() {
        uint32_t now = SFX_MILLIS();
        if (now - _lastRead_ms < _readInterval_ms) return;
        _lastRead_ms = now;

        if (SFX_VBUS_PIN != 0xFF) {
            _usbPowered = digitalRead(SFX_VBUS_PIN) == HIGH;
        }

        _sm.feed(readAdc_mV());
    }

    // ─── Queries (sensor concept — see battery_server.h) ────────────────────
    uint16_t voltage_mV()      const { return _sm.voltage_mV(); }
    uint16_t cellVoltage_mV()  const { return _sm.cellVoltage_mV(); }
    uint8_t  cellCount()       const { return _sm.cellCount(); }
    BatteryChemistry chemistry() const { return _sm.chemistry(); }
    const BatteryProfile& profile() const { return _sm.profile(); }
    bool     isLow()           const { return _sm.isLow(); }
    bool     isCritical()      const { return _sm.isCritical(); }
    uint8_t  percentage()      const { return _sm.percentage(); }

    // ─── ADC-divider-specific ───────────────────────────────────────────────

    /// True when USB VBUS is detected (Pico GP24); used by isPresent().
    bool isUsbPowered() const { return _usbPowered; }

    /**
     * @brief Heuristic battery-present check.
     *
     * Returns false when voltage is below MIN_DETECT_mV, or when USB is
     * powering the board AND cell count was not pinned manually (likely a
     * phantom reading from the Pico's internal VSYS/3 on GP29).
     */
    bool isPresent() const {
        if (_sm.voltage_mV() < BatteryStateMachineConfig::MIN_DETECT_mV) return false;
        if (_usbPowered && !_sm.isCellCountManual()) return false;
        return true;
    }

private:
    static constexpr uint16_t ADC_REF_mV     = 3300;
    static constexpr uint16_t ADC_LEVELS     = 4096;   // 12-bit ADC
    static constexpr uint8_t  ADC_OVERSAMPLE = 4;      // Samples per read

    uint16_t readAdc_mV() {
        uint32_t sum = 0;
        for (uint8_t i = 0; i < ADC_OVERSAMPLE; i++) {
            sum += analogRead(_adcPin);
        }
        uint32_t raw = sum / ADC_OVERSAMPLE;

        // scale = ADC_REF_mV * multiplier; fits uint32 for any sane multiplier.
        // Final = raw * scale / ADC_LEVELS — max ≈ 4096 * 3300 * 100 / 4096 ≈ 3.3e5 per
        // unit of multiplier, well within uint64 intermediate.
        uint64_t scale = (uint64_t)ADC_REF_mV * MULTIPLIER_MILLI;  // mV * ×1000
        return (uint16_t)((raw * scale) / ((uint64_t)ADC_LEVELS * 1000ULL));
    }

    BatteryStateMachine _sm;
    uint8_t  _adcPin = 0;
    bool     _usbPowered = false;
    uint16_t _readInterval_ms = 500;
    uint32_t _lastRead_ms = 0;
};

// ============================================================================
// Convenience aliases for common dividers
// ============================================================================

using AdcDividerBatteryX5_1 = AdcDividerBatteryT<5100>;   // 41k / 10k
using AdcDividerBatteryX6_0 = AdcDividerBatteryT<6000>;   // 50k / 10k
using AdcDividerBatteryX11  = AdcDividerBatteryT<11000>;  // 100k / 10k

#endif // BATTERY_MONITOR_H
