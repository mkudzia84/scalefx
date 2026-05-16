/*
 * ServoCollection<N> — N hobby servos behind one uniform protocol.
 *
 * Wraps the existing ServoControl driver (lib/sfx_peripherals/servo/srv_control.h)
 * with:
 *   - compile-time channel count N
 *   - per-channel pin descriptor (native GPIO or expander pin)
 *   - per-channel calibration loaded from the board's YAML config
 *   - generic protocol API: setPosition / setRange / setMotion / query
 *
 * Position model (wire format):
 *   Public API uses microsecond pulse widths (500..2500 µs typical) to
 *   keep the protocol unambiguous across boards.  A normalised signed
 *   helper (-1000..+1000 around the calibrated centre) is built on top
 *   for convenience.
 *
 * Lifecycle:
 *   - boot:           every channel created in IDLE — no PWM output yet
 *   - INIT(SLAVE):    PWM peripheral attached, channel parked at centre
 *   - INIT(DIRECT):   same as SLAVE but no master-side keepalive enforced
 *   - SHUTDOWN:       channel detached from PWM (output pin floats)
 *
 * Templatising on N keeps the storage flat (no heap), and the
 * ComponentInfo array reported in COMPONENT_LIST_RESP is built from
 * these compile-time constants without runtime allocation.
 */

#ifndef SFX_SERVO_COLLECTION_H
#define SFX_SERVO_COLLECTION_H

#include <array>
#include <cstdint>

#include <sfx_peripherals/servo/srv_control.h>

#include "component_event.h"
#include <serial/slave/component_kind.h>

namespace sfx_peripherals {

/// Per-channel hardware descriptor (compile-time, supplied by the board
/// firmware in a std::array<ChannelSpec, N>).
struct ServoSpec {
    uint8_t  pin;             ///< GPIO pin index (native) or expander pin
    bool     useExpander;     ///< false = native GPIO, true = expander pin (board-known instance)
    uint16_t defaultMin_us;   ///< calibration default; YAML can override
    uint16_t defaultMax_us;
    uint16_t defaultCenter_us;
    uint16_t defaultMaxSpeed; ///< us/sec — trapezoidal profile max speed
    uint16_t defaultAccel;    ///< us/sec²
    uint16_t defaultDecel;    ///< us/sec²
    uint16_t safeStatePos_us; ///< position commanded on safe-state entry.
                              ///< Defaults to defaultCenter_us; overridable
                              ///< for one-way mechanisms (trigger pulleys
                              ///< etc. where "neutral" isn't centre)
};

/// `ServoCollection<N, TServoCtrl>` — N-channel servo array.
///
/// Templated on the underlying single-channel servo class so the
/// collection itself stays a thin facade — all motion-profile logic
/// (trapezoidal acceleration, jerk decay, position clamping) lives
/// in the wrapped `TServoCtrl` from `sfx_peripherals/servo/`.  The
/// default `ServoControl` is the existing production driver; a
/// substitute (mock for tests, alternate algorithm for benchmarking)
/// can be supplied without forking this header.
///
/// Required `TServoCtrl` interface — must support these methods, all
/// of which `ServoControl` already provides:
///   - `bool begin(int pin, int minUs, int maxUs, int initialUs);`
///   - `void end();`
///   - `void setLimits(int minUs, int maxUs);`
///   - `void setMotionProfile(int spd, int acc, int dec);`
///   - `void setTarget(int us);`
///   - `void applyJerk(int16_t offset_us, uint16_t duration_ms);`
///   - `bool isAtTarget() const;`
///   - `int  currentPositionUs() const;`
///   - `int  targetPositionUs() const;`
///   - `int  velocityUsPerSec() const;`
///   - `void update();`
template <size_t N, typename TServoCtrl = ServoControl>
class ServoCollection {
public:
    static constexpr size_t COUNT = N;
    using ServoCtrl = TServoCtrl;

    /// Provide compile-time hardware descriptors at boot.  Stores the
    /// specs but does NOT yet attach the PWM peripheral — that happens
    /// in attach() once the board transitions out of IDLE.
    void configure(const std::array<ServoSpec, N>& specs) { _specs = specs; }

    /// Attach the PWM peripheral and begin driving outputs.  Called from
    /// the slave server when INIT lands.
    bool attach();

    /// Detach the PWM peripheral; outputs go high-Z.  Called on SHUTDOWN
    /// or before the chip enters deep-sleep.  Idempotent.
    void detach();

    /// Park every servo at its calibrated centre and let the profile
    /// run there.  Invoked from `SlaveServer::enterSafeState()` on
    /// keepalive timeout / SHUTDOWN.  Safe to call from any state.
    void parkAtNeutral();

    /// Update the motion profile — call frequently from loop().
    void update();

    // ── Per-channel API (1-based to match wire convention) ─────────────

    bool setPosition_us (uint8_t idx, uint16_t pulse_us);
    bool setRange       (uint8_t idx, uint16_t min_us, uint16_t max_us, uint16_t center_us);
    bool setMotion      (uint8_t idx, uint16_t maxSpeed, uint16_t accel, uint16_t decel);

    /// Normalised signed position around the calibrated centre.
    /// Convenience wrapper over setPosition_us; -1000 = min, 0 = center, 1000 = max.
    bool setPositionNorm(uint8_t idx, int16_t normSigned);

    /// Apply a jerk — transient signed offset added to the running
    /// target for `duration_ms`.  Use for recoil kicks, idle wobble,
    /// brief mechanical reactions.  The slave's profile carries the
    /// position there and unwinds it back, all subject to the same
    /// max-speed / accel / decel constraints.  Multiple in-flight
    /// jerks compose additively — fire one for "trigger pull recoil"
    /// while another is decaying for "continued vibration" and the
    /// servo sums them.  Negative offsets jerk the other way.
    bool applyJerk(uint8_t idx, int16_t offset_us, uint16_t duration_ms);

    /// Soft-disable PWM output.  When held=true the slave keeps the
    /// internal target / motion-profile state but stops emitting the
    /// PWM signal — pin idles, servo position holds via spring/
    /// gravity (or whatever's mechanical).  held=false re-attaches
    /// PWM and the servo snaps back to the held target through the
    /// motion profile.  Useful for low-power hold (e.g. gear locked
    /// in deployed position) and for hand-positioning during
    /// bench-test.
    bool setHold(uint8_t idx, bool held);

    bool query          (uint8_t idx,
                         uint16_t& out_pulse_us,
                         uint16_t& out_target_us,
                         int16_t&  out_velocity) const;

    // ── Target-reached callback ──────────────────────────────────────
    //
    // SlaveServer registers a single callback here at attach() time.
    // Each call to setPosition_us() / setPositionNorm() arms a fresh
    // motion; when the underlying ServoControl converges on the target
    // (velocity ≈ 0 within POSITION_TOLERANCE), the collection invokes
    // this callback exactly once with the final post-clamp position.
    //
    // The SlaveServer turns that callback into a SERVO_TARGET_REACHED
    // async packet to the master.
    using TargetReachedCb = std::function<void(uint8_t idx, uint16_t pos_us)>;
    void setTargetReachedCallback(TargetReachedCb cb) { _onTargetReached = std::move(cb); }

    /// Periodic mid-motion progress emitter.  Fires for each servo
    /// that's currently profiling (target armed, not yet reached) at
    /// the configured rate.  SlaveServer turns these into
    /// SERVO_MOTION_UPDATE async packets.  Disabled by default; the
    /// master enables before sequences that benefit from progress
    /// tracking (gear cycle, recoil) and disables after.
    using MotionUpdateCb = std::function<void(uint8_t idx, uint16_t pos, uint16_t target, int16_t vel)>;
    void setMotionUpdateCallback(MotionUpdateCb cb) { _onMotionUpdate = std::move(cb); }

    /// Toggle motion-update emission.  enable=false halts callbacks
    /// regardless of profile state.  rate_hz=0 → keep current rate
    /// (default 10 Hz on first enable).
    void setMotionUpdatesEnabled(bool enable, uint8_t rate_hz = 0) {
        _motionUpdatesEnabled = enable;
        if (rate_hz > 0) _motionUpdate_period_ms = (uint16_t)(1000u / rate_hz);
    }

    /// Board-local event callback (indicator LEDs / buzzers / logs).
    /// Fires on Activated / Deactivated / MotionStarted / MotionEnded /
    /// SafeStateEntered.  See `component_event.h`.
    void setEventCallback(ComponentEventCb cb) { _onEvent = std::move(cb); }

    // ── Component enumeration (for COMPONENT_LIST_RESP) ────────────────

    static constexpr ComponentInfo describe(size_t i) {
        return ComponentInfo{
            .index = (uint8_t)i,
            .kind  = ComponentKind::Servo,
            .flags = ServoFlags::HAS_MOTION_PROFILE | ServoFlags::HAS_PERSISTED_CFG,
            .reserved = 0,
        };
    }

private:
    std::array<ServoSpec, N>    _specs{};
    std::array<TServoCtrl, N>   _servos{};
    /// Per-channel "armed for target-reached event" — set on each
    /// setPosition_us() call, cleared after the callback fires so we
    /// emit exactly one event per command.
    std::array<bool, N>         _armed{};
    TargetReachedCb             _onTargetReached;
    MotionUpdateCb              _onMotionUpdate;
    ComponentEventCb            _onEvent;
    bool                        _attached            = false;
    bool                        _motionUpdatesEnabled = false;
    uint16_t                    _motionUpdate_period_ms = 100;   ///< 10 Hz default
    uint32_t                    _lastMotionUpdate_ms  = 0;
};

}  // namespace sfx_peripherals

#endif  // SFX_SERVO_COLLECTION_H
