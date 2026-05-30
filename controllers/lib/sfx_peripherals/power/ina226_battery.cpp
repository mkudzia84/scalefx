/*
 * Ina226Battery — Implementation
 *
 * Thin wrapper over the shared BatteryStateMachine. The owning board keeps
 * the INA226 fresh as part of its diagnostic loop; we only sample the cached
 * busVoltage_mV() register here.
 */

#include "ina226_battery.h"
#include "platform/sfx_platform.h"

void Ina226Battery::begin(BatteryChemistry chemistry) {
    _lastRead_ms = 0;
    _sm = BatteryStateMachine{};
    _sm.setChemistry(chemistry);

    if (_ina && _ina->isAvailable()) {
        _sm.feed((uint16_t)_ina->busVoltage_mV());
    }
}

void Ina226Battery::update() {
    if (!_ina) return;

    uint32_t now = SFX_MILLIS();
    if (now - _lastRead_ms < _readInterval_ms) return;
    _lastRead_ms = now;

    if (!_ina->isAvailable()) return;

    _sm.feed((uint16_t)_ina->busVoltage_mV());
}

bool Ina226Battery::isPresent() const {
    if (!_ina || !_ina->isAvailable()) return false;
    return _sm.voltage_mV() >= BatteryStateMachineConfig::MIN_DETECT_mV;
}
