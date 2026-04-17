/*
 * Ina226Battery — battery sensor backed by an INA226 channel
 *
 * Reads battery rail voltage from one channel of an INA226 power monitor.
 * Used by boards that already have INA226 instances on the I2C bus for power
 * diagnostics (HubFX has 6 of them at 0x40-0x45) — pick one as "the battery
 * rail" and pass it to this policy by reference.
 *
 * The INA226 itself stays owned by the board firmware (it is shared with
 * other diagnostic readouts). Ina226Battery does NOT call ina.update() —
 * the board's main loop is expected to keep the INA226 fresh; this policy
 * just reads the cached busVoltage_mV() and feeds the state machine.
 *
 * Usage:
 *   INA226 ina[6];                   // owned by board firmware, init in setup()
 *   Ina226Battery battery(ina[0]);   // bind to channel 0 = main battery rail
 *   battery.begin(BatteryChemistry::LIPO);
 *
 *   // In loop(): board updates INA226s as part of its diagnostic loop, then:
 *   battery.update();
 */

#ifndef INA226_BATTERY_H
#define INA226_BATTERY_H

#include "battery_state_machine.h"
#include "ina226.h"

class Ina226Battery {
public:
    using AlertCallback = BatteryStateMachine::AlertCallback;

    /**
     * @brief Bind to one INA226 channel as the battery sensor.
     *
     * @param ina  INA226 driver instance (owned by the board firmware; must
     *             outlive Ina226Battery and be kept fresh via ina.update()).
     */
    explicit Ina226Battery(INA226& ina) : _ina(&ina) {}

    /// Default-construct unbound; the board MUST call setSensor() before begin().
    /// Useful for boards where the battery rail is YAML-configurable (HubFX).
    Ina226Battery() = default;

    /// Re-bind to a different INA226 channel. Resets the state machine so the
    /// new rail's first reading drives cell-detect cleanly.
    void setSensor(INA226& ina) {
        _ina = &ina;
        _sm = BatteryStateMachine{};
        _lastRead_ms = 0;
    }

    /**
     * @brief Initialize state machine with starting chemistry.
     *
     * The INA226 itself must be initialized separately by the board's setup
     * code (Ina226Battery doesn't own its hardware).
     */
    void begin(BatteryChemistry chemistry = BatteryChemistry::LIPO);

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
    /// Sample the bound INA226's cached bus voltage and feed the state machine.
    void update();

    // ─── Queries (sensor concept — see battery_server.h) ────────────────────
    uint16_t voltage_mV()      const { return _sm.voltage_mV(); }
    uint16_t cellVoltage_mV()  const { return _sm.cellVoltage_mV(); }
    uint8_t  cellCount()       const { return _sm.cellCount(); }
    BatteryChemistry chemistry() const { return _sm.chemistry(); }
    const BatteryProfile& profile() const { return _sm.profile(); }
    bool     isLow()           const { return _sm.isLow(); }
    bool     isCritical()      const { return _sm.isCritical(); }
    uint8_t  percentage()      const { return _sm.percentage(); }

    // ─── INA226-specific ────────────────────────────────────────────────────

    /// Battery presence heuristic: true if the bound INA226 is responding *and*
    /// reads above the minimum-detect threshold. No USB-phantom check needed —
    /// INA226 measures the rail directly.
    bool isPresent() const;

    /// Access the bound INA226 (e.g., to pull current/power readings).
    INA226& ina() { return *_ina; }
    const INA226& ina() const { return *_ina; }

private:
    INA226* _ina = nullptr;
    BatteryStateMachine _sm;
    uint16_t _readInterval_ms = 500;
    uint32_t _lastRead_ms = 0;
};

#endif // INA226_BATTERY_H
