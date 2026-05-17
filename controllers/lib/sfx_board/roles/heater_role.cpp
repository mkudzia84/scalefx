/*
 * HeaterRole implementation.
 */

#include "heater_role.h"

namespace sfx_core {

void HeaterRole::setTarget(int16_t target_cx10) {
    _target_cx10 = target_cx10;
    if (target_cx10 == INT16_MIN) {
        // Off — shut down output immediately.
        _commandedDuty = 0;
        if (_port) _port->setDuty(0);
    }
}

void HeaterRole::tick() {
    if (!_port || !_tSense || _target_cx10 == INT16_MIN) return;

    // Default drive duty = port's full scale if user didn't set one.
    const uint16_t drive = _driveDutyExplicit ? _driveDuty : _port->maxDuty();

    const int16_t actual = _tSense->temperature_cx10();
    if (!_tSense->isAvailable()) {
        // Lost sense — fail safe.
        _commandedDuty = 0;
        _port->setDuty(0);
        return;
    }

    const int16_t lower = _target_cx10 - _hysteresis_cx10;
    const int16_t upper = _target_cx10 + _hysteresis_cx10;

    if (actual <= lower && _commandedDuty == 0) {
        _commandedDuty = drive;
        _port->setDuty(drive);
    } else if (actual >= upper && _commandedDuty != 0) {
        _commandedDuty = 0;
        _port->setDuty(0);
    }
    // Inside the band — hold current state.
}

}  // namespace sfx_core
