/*
 * gun_unit.h — single gun state machine for the GunFX effect.
 *
 *   Per-gun, Phase 2 of GunFX rollout (instructions/22): drives one
 *   muzzle-flash LED + optional recoil servo + optional smoke heater +
 *   optional smoke fan + optional yaw/pitch turret pair, gated by an
 *   optional fire trigger + optional rate-of-fire selector channel.
 *
 *   The unit holds a `GunSpec` (from gunfx_config.h) and reads it
 *   through the entire tick — no copies. State that varies per tick
 *   (firing flag, smoke armed, ROF item, axis positions, manual
 *   override) lives on the unit; the spec is config only.
 *
 *   All wire activity flows through topology callbacks installed at
 *   `configure()` time — keeps the state machine transport-agnostic.
 *
 *   Each shot does, atomically (one topology batch):
 *     - LED_QUEUE_LOAD + LED_START on the muzzle-flash port
 *     - SERVO_RECOIL on each enabled turret axis (a random ±offset added
 *       to the servo output for `recoilHoldMs`, de-jerked by the role)
 *     - One fan pulse if `smoke.fanMode == FN_PUFF_PER_FIRE`
 *     - ShotEvent callback (audio + wire async)
 *
 *   Yaw + pitch run as one-line passthroughs: read input channel → convert
 *   the RC pulse to a normalised fraction → `SERVO_SET_POS_NORM` to the
 *   axis's servo port, where the role maps it onto the servo's calibrated
 *   [min,max] (full input throw → full calibrated throw).  Motion shaping
 *   (clamp / speed / accel / jerk) lives on the `ServoActuatorRole` attached
 *   to the port — set once in `/hubfx.yaml`'s `ports[]` block, never
 *   integrated here (Rule 42 — actuator mechanism on the role layer).
 *
 *   Manual override: when `_manual.active`, RC channel reads are
 *   ignored and per-axis / per-subsystem targets come from `_manual.*`
 *   instead. Auto-released after `kManualTimeoutMs` of no
 *   GUN_MANUAL_SET so a Studio crash never strands the gun in puppet
 *   mode.
 */

#ifndef HUBFX_GUN_UNIT_H
#define HUBFX_GUN_UNIT_H

#include <cstdint>

#include "../effect_id.h"
#include "gunfx_config.h"
#include "gunfx_protocol.h"

namespace hubfx::effects::gunfx {

/// Manual override state — the most recent operator-driven target for
/// each subsystem.  Only fields whose `*Valid` flag is true override
/// the corresponding RC read; the others fall through to RC.
struct ManualOverride {
    bool     active            = false;   ///< true when in manual mode
    uint32_t lastUpdateMs      = 0;       ///< for auto-release timeout

    bool     yawValid          = false;
    uint16_t yawUs             = 1500;
    bool     pitchValid        = false;
    uint16_t pitchUs           = 1500;
    bool     rofIndexValid     = false;
    uint8_t  rofIndex          = 0xFF;
    bool     fireHoldValid     = false;
    bool     fireHold          = false;
    bool     smokeArmValid     = false;
    bool     smokeArm          = false;
};

/// Auto-release timeout: if no GUN_MANUAL_SET arrives within this
/// window, the unit returns to RC. Picked to be longer than Studio's
/// natural retry interval but short enough that a crash drops the
/// puppet promptly (5 s).
inline constexpr uint32_t kManualTimeoutMs = 5000;

class GunUnit {
public:
    using SendRoleCmdFn = bool (*)(void* ctx, const PortRef& addr,
                                   uint8_t innerType,
                                   const uint8_t* payload, size_t len);
    using BatchFn       = void (*)(void* ctx);
    /// Per-shot visual broadcast — `GUN_SHOT_EVENT` to Studio + diag.
    /// Called for EVERY shot (one-shot fire, auto-fire tick, sustained
    /// start) so the verbose-status mirror sees individual shot
    /// pulses even when the audio sample is looping.
    using ShotEventFn   = void (*)(void* ctx, uint8_t id);
    /// Audio-channel command — `soundPath != nullptr` starts playback
    /// (one-shot when loop=false, infinite loop when loop=true);
    /// `soundPath == nullptr` stops the channel.  Channel + output
    /// mask are picked at the gun unit (id-parity routing onto
    /// `HubFxLayout::GunA/B`).  Sustained-fire feature 2026-05-23
    /// uses this to play the shot WAV on a loop while the trigger
    /// is held — auto-fire ticks no longer re-trigger audio, so the
    /// 200 / 550 RPM cadence baked into the WAV runs cleanly.
    using AudioCmdFn    = void (*)(void* ctx,
                                   const char* soundPath,
                                   uint8_t audioChannel,
                                   uint8_t outputMask,
                                   bool loop);

    GunUnit() = default;

    void configure(const GunSpec& spec,
                   SendRoleCmdFn sendFn, void* sendCtx,
                   BatchFn beginFn, BatchFn commitFn,
                   ShotEventFn shotFn  = nullptr, void* shotCtx  = nullptr,
                   AudioCmdFn  audioFn = nullptr, void* audioCtx = nullptr) {
        _spec     = spec;
        _send     = sendFn;
        _sendCtx  = sendCtx;
        _begin    = beginFn;
        _commit   = commitFn;
        _shot     = shotFn;
        _shotCtx  = shotCtx;
        _audio    = audioFn;
        _audioCtx = audioCtx;
        _firing   = false;
        _smokeArmed = false;
        _shotsThisSession = 0;
        _activeRofIndex = pickInitialRofIndex();
        // Servo motion profile arrives via the role-attach payload from
        // /hubfx.yaml's ports[] block — the role is ready before
        // configure() runs.  The gun just pushes intent targets.
    }

    // ── Public API ────────────────────────────────────────────────────

    const GunSpec& spec() const { return _spec; }
    uint8_t  id()           const { return _spec.id; }
    bool     firing()       const { return _firing; }
    bool     smokeArmed()   const { return _smokeArmed; }
    /// True when the smoke fan is currently being driven on the wire
    /// (continuous mode = `_fanContinuous`; pulse mode = last sent
    /// pct != 0; manual burst = `_fanBurstEndMs` non-zero).  Read by
    /// the verbose-status producer for the Studio mirror.
    bool     fanRunning()   const {
        return _fanContinuous || _fanBurstEndMs != 0 || (_fanLastPct != 0 && _fanLastPct != 255);
    }
    uint8_t  rofIndex()     const { return _activeRofIndex; }
    uint16_t triggerUs()    const { return _lastTriggerUs; }
    uint16_t rofSelectorUs() const { return _lastRofSelectorUs; }
    // Phase 2.9: motion lives on the ServoActuatorRole.  GunUnit only
    // tracks the LAST COMMANDED target; the role owns the integrator
    // and the actual `current` position.  Verbose status surfaces both
    // as the same value for now — Studio reads the role's status
    // directly if it wants live current µs (deferred follow-up).
    uint16_t yawCurrentUs()   const { return _yawTargetUs; }
    uint16_t yawTargetUs()    const { return _yawTargetUs; }
    uint16_t pitchCurrentUs() const { return _pitchTargetUs; }
    uint16_t pitchTargetUs()  const { return _pitchTargetUs; }
    const ManualOverride& manual() const { return _manual; }
    uint32_t shotsThisSession() const { return _shotsThisSession; }

    /// Fire ONE shot.  `rofOverride == 0xFF` (the default) means "use
    /// the gun's currently-armed ROF" — same behaviour as the legacy
    /// single-arg form.  Any value `< numRofItems` forces THIS shot to
    /// pull its sound + rpm from the named item; out-of-range values
    /// fall back to the armed index (the caller / wire handler is
    /// responsible for NACK'ing bad indices before reaching here).
    void fireOnce(uint8_t rofOverride = 0xFF);
    /// Start auto-fire.  `rpmOverride == 0` falls back to the armed
    /// (or forced) ROF's rpm; `rofOverride == 0xFF` keeps the
    /// selector-channel arming, any other valid index forces it for
    /// the duration (cleared on `stopFiring`).  This lets the GUI
    /// "Auto-Fire" button pick an ROF from a dropdown so the test
    /// always has a concrete program even when the selector stick
    /// is between bands.
    void startFiring(uint16_t rpmOverride, uint8_t rofOverride = 0xFF);
    void stopFiring();
    void armSmoke(bool armed);

    /// Tick — drives auto-fire scheduling, recoil-return timing, fan
    /// pulses, heater bang-bang, yaw/pitch motion profile, and the
    /// manual-override auto-release timeout.
    void update(uint32_t nowMs, uint32_t dtMs);

    /// Forwarded by the service when the trigger channel crosses its
    /// configured threshold (TriggerInput::Boolean dispatch).
    void onTriggerBoolean(bool held);
    /// Forwarded for the trigger's raw µs — kept for the verbose
    /// status mirror and the future trigger-bar UI.
    void onTriggerRawUs(uint16_t pulseUs, bool valid);
    /// Forwarded for the ROF selector channel (Raw µs).
    void onRofSelectorUs(uint16_t pulseUs, bool valid);
    /// Forwarded for yaw / pitch axis input channels (Raw µs).
    void onYawInputUs(uint16_t pulseUs, bool valid);
    void onPitchInputUs(uint16_t pulseUs, bool valid);
    /// Retained only for the verbose-status wire byte [25] (Rule 11 — the
    /// byte stays in the frame).  The separate activation gate was removed
    /// 2026-05-29; `_heaterActive` is now permanently true (the gun_smoke
    /// channel arms the heater directly via armSmoke()).
    bool heaterActive() const { return _heaterActive; }

    /// True when the heater is currently being driven on the wire.
    /// Composes (a) smoke armed, (b) Rule 43 activation gate high,
    /// (c) for HM_CYCLE: currently in the ON-phase.  The verbose
    /// status broadcast reads this for `heaterDutyPct` so the Studio
    /// mirror reflects cycle phase + activation gate state, not just
    /// the smoke-armed flag.
    bool heaterDriving() const;

    // ── Manual override ──────────────────────────────────────────────

    /// Apply a manual-set packet's fields (selected by `flags`) and
    /// (re-)enter manual mode.  Auto-release timer resets on every call.
    void applyManualSet(uint8_t flags,
                        uint16_t yawUs, uint16_t pitchUs,
                        uint8_t rofIndex, bool fireHold,
                        bool smokeArm, bool smokeFanBurst,
                        uint32_t nowMs);
    /// Exit manual mode immediately; RC takes over on the next tick.
    void releaseManual();

private:
    void doShot();
    void commandFlash();
    /// Start (or, on a mid-burst ROF change, SWITCH) the sustained-fire looping
    /// WAV for the currently-armed ROF on this gun's audio channel.  The shot
    /// sound is a per-RPM baked loop, so the sample must follow the ROF — not
    /// just the LED cadence.  Called from startFiring() and onRofSelectorUs().
    void startShotLoop();
    /// Fire a role-level recoil impulse on each enabled turret axis: a random
    /// ±offset added to the servo OUTPUT for `recoilHoldMs`, then de-jerked by
    /// the role.  Rides on top of the aim (no RC suppression, no snap-back).
    void commandRecoilJerk();
    /// Send SERVO_RECOIL to one axis' servo role (random offset + hold window).
    void commandServoRecoil(const PortRef& port, int16_t offsetUs, uint16_t durationMs);
    void commandHeater(bool on);
    void commandFanPct(uint8_t pct);
    void commandServoTargetUs(const PortRef& port, uint16_t us);
    /// Send SERVO_SET_POS_NORM — convert an RC pulse to a normalised fraction
    /// and let the axis servo's role map it onto its LIVE calibrated [min,max]
    /// (Rule 42).  Used for the yaw/pitch passthrough so a full-throw input
    /// drives the full calibrated throw instead of saturating at the limits.
    void commandServoPosNorm(const PortRef& port, uint16_t rcUs);

    /// Find the RofItem whose band contains `pulseUs`. Returns 0xFF
    /// when no band matches (out-of-band = no item armed).
    uint8_t findRofIndex(uint16_t pulseUs) const;
    /// Fallback for first-tick state when no ROF channel is bound — picks
    /// the first declared item, or 0xFF if there are none.
    uint8_t pickInitialRofIndex() const;
    /// Returns the RofItem to use when firing — armed item, or a
    /// synthetic default when no ROF is configured.
    const RofItem* activeRof() const;

    void scheduleFanPuff(uint32_t nowMs);
    void tickFan(uint32_t nowMs);
    /// FN_PULSE rate-limited fan PCT update.  Sends MOTOR_SET_PCT only
    /// when (a) the integer % changed since the last send, AND (b) at
    /// least `kFanUpdatePeriodMs` (≈20 ms) has elapsed.  Caps the
    /// sinusoid's wire chatter at ~50 Hz regardless of main-loop rate.
    void sendFanPctIfChanged(uint8_t pct, uint32_t nowMs);
    // Phase 2.9: ServoActuatorRole owns the motion profile; the gun
    // just pushes a target each tick.  `lastCommandedRef` carries the
    // last value we wrote so we suppress redundant SERVO_SET_TARGET
    // packets while the input is stable.
    void tickAxis(const GunAxis& axis,
                  uint16_t lastRcUs, bool manualValid, uint16_t manualUs,
                  uint16_t& lastCommandedRef);
    void tickManualTimeout(uint32_t nowMs);

    GunSpec      _spec{};
    bool         _firing           = false;
    bool         _smokeArmed       = false;
    uint32_t     _shotIntervalMs   = 0;     ///< 0 when not auto-firing
    uint32_t     _nextShotMs       = 0;
    // Recoil is a role-level impulse: on each shot we send SERVO_RECOIL with a
    // random ±offset to each enabled turret axis.  The ServoActuatorRole adds
    // the offset on top of the aim for recoilHoldMs then removes it — the gun
    // keeps no per-shot recoil state (no snap-back, no RC suppression).

    // ROF arbitration state.
    uint8_t      _activeRofIndex   = 0xFF;
    uint16_t     _lastRofSelectorUs = 0;
    bool         _haveRofSelector  = false;

    // Trigger state (Boolean edge handled by TriggerInput in the service).
    bool         _triggerHeld      = false;
    uint16_t     _lastTriggerUs    = 0;

    // Smoke fan state.
    //   _fanContinuous     = true while FN_CONTINUOUS is actively driving
    //                        the fan (smoke armed + firing).  Edge-toggles
    //                        commandFanPct() so the wire stays quiet on
    //                        stable input.
    //   _fanPulseStartMs   = nowMs at which the current sinusoidal
    //                        envelope started (FN_PULSE only).  Each
    //                        shot resets this to nowMs so the fan
    //                        ramps fresh on every trigger event.
    //                        0 = no envelope in flight (idle at 50% base).
    //   _fanLastPct        = last commandFanPct() value sent (0..100),
    //                        or 255 if unset.  Used to suppress
    //                        duplicate wire packets when the sinusoid
    //                        rounds to the same integer two ticks in
    //                        a row.
    //   _fanLastUpdateMs   = nowMs of the last MOTOR_SET_PCT send.
    //                        Used to rate-limit sinusoidal updates to
    //                        ~50 Hz (kFanUpdatePeriodMs below).
    //   _pendingFanBurst   = manual one-shot burst flag (operator-driven
    //                        via GUN_MANUAL_SET FAN_BURST).
    //   _fanBurstEndMs     = end-time of the operator's manual burst
    //                        (0 if no burst in flight).
    bool         _fanContinuous    = false;
    uint32_t     _fanPulseStartMs  = 0;
    uint8_t      _fanLastPct       = 255;
    uint32_t     _fanLastUpdateMs  = 0;
    uint32_t     _fanBurstEndMs    = 0;     ///< 0 = no burst pending, else "fan off at this time"
    bool         _pendingFanBurst  = false; ///< one-shot manual fan burst request

    // Heater activation gate (Rule 43 named-channel, Phase 4 polish
    // 2026-05-26). Defaults to true so guns without a heaterActivationInput
    // configured drive the heater whenever it is `_smokeArmed`. The
    // service installs a callback during subscribePerGunInputs when an
    // activation channel IS bound; the dispatcher then toggles this
    // flag and we re-issue HEATER_SET_TARGET on each transition.
    bool         _heaterActive     = true;

    // Heater cycle scheduling (HM_CYCLE mode, Phase 4 polish 2026-05-26).
    // `_heaterCyclePhase` = true → currently in the ON-phase (heater
    // commanded on); false → OFF-phase (heater commanded off).
    // `_heaterCycleNextMs` = nowMs threshold at which the phase flips.
    // Only consulted when `_smokeArmed && spec.smoke.heaterMode == HM_CYCLE`;
    // cleared on armSmoke(false).  The cycle clock marches independently
    // of the Rule 43 activation gate — if activation is low during the
    // ON-phase, commandHeater() still gates the wire-out to OFF via
    // `effective = on && _heaterActive`, but the phase timer keeps
    // ticking so the next ON-phase resumes cleanly when activation
    // returns high.
    uint32_t     _heaterCycleNextMs = 0;
    bool         _heaterCyclePhase  = false;

    // Axis state.  Phase 2.9: motion lives on the ServoActuatorRole;
    // we only track input + last commanded target here.
    uint16_t     _lastYawInputUs   = 1500;
    uint16_t     _lastPitchInputUs = 1500;
    bool         _haveYawInput     = false;
    bool         _havePitchInput   = false;
    uint16_t     _yawTargetUs      = 1500;   ///< last value sent to yaw servo
    uint16_t     _pitchTargetUs    = 1500;   ///< last value sent to pitch servo

    // Manual override.
    ManualOverride _manual{};

    // Stats.
    uint32_t     _shotsThisSession = 0;

    // Transport hooks.
    SendRoleCmdFn _send     = nullptr;
    void*         _sendCtx  = nullptr;
    BatchFn       _begin    = nullptr;
    BatchFn       _commit   = nullptr;
    ShotEventFn   _shot     = nullptr;
    void*         _shotCtx  = nullptr;
    AudioCmdFn    _audio    = nullptr;
    void*         _audioCtx = nullptr;
};

}  // namespace hubfx::effects::gunfx

#include "gun_unit.ipp"

#endif  // HUBFX_GUN_UNIT_H
