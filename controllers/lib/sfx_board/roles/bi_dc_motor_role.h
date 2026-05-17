/*
 * BiDcMotorRole — bi-directional DC motor on an `HBridgePort` with
 * optional current-sense stall detection.
 *
 * Same stall machinery as `DcMotorRole` but writes signed duty (negative
 * = reverse) to the underlying H-bridge.  Brake and coast are exposed
 * as separate commands — masters that want unambiguous zero-state
 * behaviour pick one explicitly.
 */

#ifndef SFX_BI_DC_MOTOR_ROLE_H
#define SFX_BI_DC_MOTOR_ROLE_H

#include <Arduino.h>
#include <cstdint>
#include <cstdlib>
#include <functional>

#include <ports/hbridge_port.h>
#include <ports/sensors.h>

namespace sfx_core {

class BiDcMotorRole {
public:
    using StallCallback = std::function<void(uint16_t peak_mA, uint16_t duration_ms)>;

    BiDcMotorRole() = default;
    BiDcMotorRole(sfx_peripherals::HBridgePort*   port,
                  sfx_peripherals::CurrentSensor* iSense = nullptr,
                  sfx_peripherals::VoltageSensor* vSense = nullptr)
        : _port(port), _iSense(iSense), _vSense(vSense) {}

    void bind(sfx_peripherals::HBridgePort*   port,
              sfx_peripherals::CurrentSensor* iSense = nullptr,
              sfx_peripherals::VoltageSensor* vSense = nullptr) {
        _port = port; _iSense = iSense; _vSense = vSense;
    }

    /// Set signed duty in port-native units (-port.maxDuty()..+port.maxDuty()).
    void setSigned(int16_t signedDuty);
    void brake();
    void coast();

    void setStallGuard(uint16_t threshold_mA, uint16_t window_ms);
    void clearStall();

    int16_t  signedDuty() const { return _commandedSigned; }
    bool     stalled()    const { return _stalled; }
    int16_t  voltage_mV() const { return _vSense ? _vSense->voltage_mV() : 0; }
    int16_t  current_mA() const { return _iSense ? _iSense->current_mA() : 0; }

    void onStall(StallCallback cb) { _onStall = std::move(cb); }

    /// Tick — call from `update()`.
    void tick();

private:
    sfx_peripherals::HBridgePort*   _port    = nullptr;
    sfx_peripherals::CurrentSensor* _iSense  = nullptr;
    sfx_peripherals::VoltageSensor* _vSense  = nullptr;

    int16_t  _commandedSigned     = 0;
    bool     _stalled             = false;
    uint16_t _stallThreshold_mA   = 2000;
    uint16_t _stallWindow_ms      = 250;
    uint32_t _overcurrentStartMs  = 0;
    uint16_t _peakDuringWindow_mA = 0;

    StallCallback _onStall;
};

}  // namespace sfx_core

#endif  // SFX_BI_DC_MOTOR_ROLE_H
