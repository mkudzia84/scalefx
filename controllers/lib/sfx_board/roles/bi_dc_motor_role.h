/*
 * BiDcMotorRole — bi-directional DC motor on an `HBridgePort` with
 * optional current-sense stall detection.
 *
 * Same stall machinery as `DcMotorRole` but writes signed duty (negative
 * = reverse) to the underlying H-bridge.  Brake and coast are exposed
 * as separate commands — masters that want unambiguous zero-state
 * behaviour pick one explicitly.
 *
 * Autonomous endstop seek (`seekEndstop`): drive a signed duty until the
 * stall guard fires (= endstop reached) or an optional timeout elapses,
 * then BRAKE locally.  The drive→sense→stop loop runs entirely on the
 * expander so the motor never hammers the endstop waiting for a wire
 * round-trip.  On finish the role fires `onEndstopResult` (reached /
 * timeout / aborted); a timeout leaves the role latched in a fault state
 * (`seekState() == SeekState::TimedOut`) until `clearStall()`.  Any
 * `setSigned` / `brake` / `coast` while seeking aborts the seek.
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

    /// Endstop-seek lifecycle.  Idle = not seeking; Seeking = driving;
    /// Reached = endstop stall hit (then braked); TimedOut = latched
    /// fault, no stall within the timeout (then braked) until clearStall().
    enum class SeekState : uint8_t { Idle = 0, Seeking = 1, Reached = 2, TimedOut = 3 };

    /// Fired when a seek finishes: outcome from `RolePacket::BiMotorSeekOutcome`,
    /// travel time, and peak current during the confirming stall window.
    using EndstopCallback = std::function<void(uint8_t outcome,
                                               uint16_t travel_ms,
                                               uint16_t peak_mA)>;

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

    /// Drive `signedDuty` until the stall guard fires or `timeout_ms`
    /// elapses (0 = no timeout), then brake — all locally.  Requires a
    /// current sensor + non-zero stall threshold; otherwise the seek
    /// can never confirm an endstop and only the timeout (if any) ends it.
    void seekEndstop(int16_t signedDuty, uint16_t timeout_ms);

    SeekState seekState() const { return _seekState; }

    int16_t  signedDuty() const { return _commandedSigned; }
    bool     stalled()    const { return _stalled; }
    int16_t  voltage_mV() const { return _vSense ? _vSense->voltage_mV() : 0; }
    int16_t  current_mA() const { return _iSense ? _iSense->current_mA() : 0; }

    void onStall(StallCallback cb)       { _onStall = std::move(cb); }
    void onEndstopResult(EndstopCallback cb) { _onEndstop = std::move(cb); }

    /// Tick — call from `update()`.
    void tick();

private:
    /// Abort an in-progress seek (no result event) — called by the
    /// public setSigned/brake/coast so an explicit command cancels a seek.
    void abortSeek();

    sfx_peripherals::HBridgePort*   _port    = nullptr;
    sfx_peripherals::CurrentSensor* _iSense  = nullptr;
    sfx_peripherals::VoltageSensor* _vSense  = nullptr;

    int16_t  _commandedSigned     = 0;
    bool     _stalled             = false;
    uint16_t _stallThreshold_mA   = 2000;
    uint16_t _stallWindow_ms      = 250;
    uint32_t _overcurrentStartMs  = 0;
    uint16_t _peakDuringWindow_mA = 0;

    // Endstop-seek state.
    SeekState _seekState      = SeekState::Idle;
    int16_t   _seekDuty       = 0;
    uint32_t  _seekStartMs    = 0;
    uint32_t  _seekDeadlineMs = 0;        ///< 0 = no timeout

    StallCallback   _onStall;
    EndstopCallback _onEndstop;
};

}  // namespace sfx_core

#endif  // SFX_BI_DC_MOTOR_ROLE_H
