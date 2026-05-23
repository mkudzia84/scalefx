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
    if (!_port || _target_cx10 == INT16_MIN) return;

    // Drive duty = scaled from drivePct + element + port rail (Phase 2).
    // Old `setDriveDuty(raw)` API is gone — operators set drivePct (0..100)
    // at the intent level and let the role + element_scaling helper do
    // the voltage math. Unknown rail / element falls back to passthrough,
    // which the helper handles → drive = drivePct * portMaxDuty / 100.
    const uint16_t drive = scaleDuty(_drivePct, _port->maxDuty(),
                                     _portRailMv, _element);

    // Open-loop mode (no temperature sensor): just hold drive duty
    // whenever a target is set (target == INT16_MIN → already handled
    // above as off).
    if (!_tSense) {
        if (_commandedDuty != drive) {
            _commandedDuty = drive;
            _port->setDuty(drive);
        }
        return;
    }

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
