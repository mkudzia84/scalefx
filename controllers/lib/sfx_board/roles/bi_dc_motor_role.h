/*
 * BiDcMotorRole — bi-directional DC motor on an `HBridgePort` with
 * optional current-sense stall detection AND optional position tracking.
 *
 * Same stall machinery as `DcMotorRole` but writes signed duty (negative
 * = reverse) to the underlying H-bridge.  Brake and coast are exposed
 * as separate commands — masters that want unambiguous zero-state
 * behaviour pick one explicitly.
 *
 * ─── Stall guard modes ───────────────────────────────────────────────
 *
 *   Fixed       (default): trip if `|I| > threshold_mA` sustained for
 *                          window_ms.  Operator must calibrate per motor
 *                          AND re-calibrate when battery sags — see Rule
 *                          42 history.
 *
 *   LiveRatio              auto-calibrates per stroke.  Three phases:
 *                            0..inrushBlank_ms     ignore (inrush spike
 *                                                  looks like stall)
 *                            +runSample_ms         average |I| to get
 *                                                  the running baseline
 *                                                  for THIS stroke
 *                            thereafter            trip when |I| exceeds
 *                                                  baseline × ratio
 *                                                  sustained for the
 *                                                  same window_ms as Fixed
 *                          Battery-independent by construction (running
 *                          AND stall currents both scale with V_bus, the
 *                          ratio is fixed by motor + load).  No stored
 *                          calibration — every move re-measures.  Pairs
 *                          with Strategy A (persist position + probe on
 *                          first command) for restart recovery.
 *
 * ─── Position tracking (Strategy A restart) ─────────────────────────
 *
 * Optional logical endstop labels A and B.  Caller commands `moveToEnd`
 * with the target label; on stall (= endstop reached) the role records
 * `_position = target`.  Caller persists the position elsewhere (YAML in
 * /hubfx.yaml on the master) and restores it via `setPosition` on boot —
 * the role doesn't touch flash.  `Unknown` is the sentinel for "never
 * calibrated / lost track on a timeout".
 *
 * Autonomous endstop seek (`seekEndstop` / `moveToEnd`): drive a signed
 * duty until the stall guard fires (= endstop reached) or an optional
 * timeout elapses, then BRAKE locally.  The drive→sense→stop loop runs
 * entirely on the expander so the motor never hammers the endstop
 * waiting for a wire round-trip.  On finish the role fires
 * `onEndstopResult` (reached / timeout / aborted); a timeout leaves the
 * role latched in a fault state (`seekState() == SeekState::TimedOut`)
 * until `clearStall()`.  Any `setSigned` / `brake` / `coast` while
 * seeking aborts the seek.
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

    /// Stall detection strategy — see header comment.
    enum class GuardMode : uint8_t { Fixed = 0, LiveRatio = 1 };

    /// Logical endstop labels.  A and B map to operator-defined endstops
    /// (e.g. "gear up" = A, "gear down" = B); the wire format doesn't
    /// know which physical end is which.  Unknown = never homed / lost.
    enum class Position : uint8_t { Unknown = 0, A = 1, B = 2 };

    /// Fired when a seek finishes: outcome from `RolePacket::BiMotorSeekOutcome`,
    /// travel time, peak current during the confirming stall window, and
    /// the resulting Position (Reached: target end; Timeout/Aborted: prior
    /// position — caller sees Unknown if it was never set).
    using EndstopCallback = std::function<void(uint8_t outcome,
                                               uint16_t travel_ms,
                                               uint16_t peak_mA,
                                               Position position)>;

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

    /// Fixed-threshold stall guard — trip on `|I| >= threshold_mA`
    /// sustained for `window_ms`.  Threshold 0 disables stall detection.
    /// Switches the guard mode to Fixed.
    void setStallGuard(uint16_t threshold_mA, uint16_t window_ms);

    /// Live-ratio stall guard — auto-baselines running current per
    /// stroke and trips when `|I| >= baseline × (ratio_x100 / 100)`
    /// sustained for `window_ms`.  Voltage-independent.  Switches the
    /// guard mode to LiveRatio.
    ///   `ratio_x100`       e.g. 250 = 2.5× running (typical).  0 = use
    ///                      a sensible default (250).
    ///   `runSample_ms`     length of the baseline-sampling window after
    ///                      inrush blanking.  0 → default 200 ms.
    ///   `inrushBlank_ms`   length of the ignore-current startup window.
    ///                      0 → default 150 ms.
    ///   `maxTravel_ms`     absolute failsafe — seek errors out if it
    ///                      hasn't tripped by then (overrides the
    ///                      per-seek timeout argument when smaller).
    ///                      0 → use the per-seek timeout only.
    /// Also reuses the Fixed-mode `window_ms` already set via
    /// `setStallGuard` for the sustained-over-threshold filter; if the
    /// caller never called `setStallGuard`, defaults to 80 ms.
    void setStallGuardRatio(uint16_t ratio_x100,
                            uint16_t runSample_ms,
                            uint16_t inrushBlank_ms,
                            uint16_t maxTravel_ms);

    /// Override the sustained-window for LiveRatio (Fixed already takes
    /// it via setStallGuard).  0 → leave unchanged.
    void setStallWindowMs(uint16_t window_ms);

    void clearStall();

    /// Drive `signedDuty` until the stall guard fires or `timeout_ms`
    /// elapses (0 = no timeout), then brake — all locally.  Requires a
    /// current sensor + a non-disabled stall guard; otherwise the seek
    /// can never confirm an endstop and only the timeout (if any) ends it.
    /// Position not tracked — use `moveToEnd` for Strategy A.
    void seekEndstop(int16_t signedDuty, uint16_t timeout_ms);

    /// Like `seekEndstop` but records `targetEnd` as the destination —
    /// on Reached the role's `_position` becomes `targetEnd` and the
    /// callback receives that position.  Strategy A entry point.  Special
    /// case: if `signedDuty == 0`, the role silently sets `_position =
    /// targetEnd` and fires `onEndstopResult` with outcome=Reached + 0 ms
    /// travel — used to restore a known position from flash without moving.
    void moveToEnd(Position targetEnd, int16_t signedDuty, uint16_t timeout_ms);

    /// Restore the role's position state (e.g. from /hubfx.yaml on boot).
    /// Does not move the motor.  Use `moveToEnd(end, 0, 0)` from the wire
    /// for the same effect.
    void setPosition(Position p) { _position = p; }

    SeekState seekState()   const { return _seekState; }
    GuardMode guardMode()   const { return _guardMode; }
    Position  position()    const { return _position; }
    uint16_t  ratio_x100()  const { return _ratio_x100; }
    uint16_t  runSampleMs() const { return _runSample_ms; }
    uint16_t  inrushBlankMs() const { return _inrushBlank_ms; }
    uint16_t  maxTravelMs() const { return _maxTravel_ms; }
    uint16_t  thresholdMa() const { return _stallThreshold_mA; }
    uint16_t  windowMs()    const { return _stallWindow_ms; }

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

    /// Common path for "trip = stall confirmed → brake + report" with a
    /// dynamically computed threshold.  Returns true if the seek was
    /// terminated this tick.
    bool stepStallDetect(uint32_t now, uint16_t threshold_mA);

    sfx_peripherals::HBridgePort*   _port    = nullptr;
    sfx_peripherals::CurrentSensor* _iSense  = nullptr;
    sfx_peripherals::VoltageSensor* _vSense  = nullptr;

    int16_t  _commandedSigned     = 0;
    bool     _stalled             = false;

    // Guard config (Fixed + LiveRatio share window_ms for the
    // sustained-over-threshold filter).
    GuardMode _guardMode          = GuardMode::Fixed;
    uint16_t  _stallThreshold_mA  = 2000;   // Fixed
    uint16_t  _stallWindow_ms     = 250;    // Fixed AND LiveRatio
    uint16_t  _ratio_x100         = 250;    // LiveRatio (= 2.5×)
    uint16_t  _runSample_ms       = 200;    // LiveRatio
    uint16_t  _inrushBlank_ms     = 150;    // LiveRatio
    uint16_t  _maxTravel_ms       = 0;      // LiveRatio failsafe (0 = none)

    // Per-tick stall detection state (sustained window).
    uint32_t _overcurrentStartMs  = 0;
    uint16_t _peakDuringWindow_mA = 0;

    // Endstop-seek state.
    SeekState _seekState      = SeekState::Idle;
    int16_t   _seekDuty       = 0;
    uint32_t  _seekStartMs    = 0;
    uint32_t  _seekDeadlineMs = 0;        ///< 0 = no per-seek timeout
    Position  _targetEnd      = Position::Unknown;

    // LiveRatio per-stroke baseline accumulator.
    uint32_t  _runAccum_mA    = 0;
    uint32_t  _runCount       = 0;
    uint16_t  _runMean_mA     = 0;        ///< 0 = not yet computed for this stroke

    // Position state machine (Strategy A).
    Position  _position       = Position::Unknown;

    StallCallback   _onStall;
    EndstopCallback _onEndstop;
};

}  // namespace sfx_core

#endif  // SFX_BI_DC_MOTOR_ROLE_H
