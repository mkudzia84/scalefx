/*
 * gunfx_config.h — full GunFx effect configuration shape.
 *
 *   The intent-layer config for the GunFx effect. Per Rule 42
 *   ("Actuator mechanism on the role layer"), this file holds ONLY
 *   what GunFx itself cares about — channel bindings, ROF table,
 *   fire-sound paths, smoke modes, recoil timing — and never duplicates
 *   actuator mechanism that the role layer owns:
 *
 *     - Servo motion profile (clamp / speed / accel / jerk) lives on
 *       `ServoActuatorRole` via the role-attach config in
 *       `/hubfx.yaml`'s `ports[]` block.  GunFx only carries each
 *       axis's `neutralUs` (the position it commands when the input
 *       channel isn't bound) and the recoil's `recoilAxis` /
 *       `recoilJerkUs` / `recoilHoldMs` — the recoil shape, not the
 *       servo's intrinsic slew.
 *     - Element voltage scaling for heater / fan PWM lives on
 *       `HeaterRole` / `DcMotorRole` via the same role-attach config.
 *       `SmokeConfig` only carries the intent fields (mode, target
 *       temperature, puff duration, …).
 *
 *   Phase 2.9 of the GunFX rollout (instructions/22) distilled this
 *   structure — earlier Phase 2 drafts kept `ServoMotionProfile`
 *   copies on `GunAxis` and `GunSpec.recoilProfile`, which produced a
 *   double-integrator anti-pattern.  Those fields are gone.
 */

#ifndef HUBFX_GUNFX_CONFIG_H
#define HUBFX_GUNFX_CONFIG_H

#include <cstdint>

#include "../effect_id.h"

namespace hubfx::effects::gunfx {

/// Max length of a named-channel reference (Rule 43 — channel inputs
/// are picked from /hubfx.yaml's `inputs:` block by NAME, not by raw
/// port+channel).  Kept in lock-step with `kInputNameMax` in
/// config/hubfx_config.h; small enough to fit four per gun + cap of 4
/// guns in the kMaxGuns table without bloating the static config.
constexpr size_t kInputNameMax = 24;

/// Cap from `gunfx_service.h` (kMaxGuns = 4). Hard cap on the static
/// config table — there's exactly one GunSpec per declared gun, and
/// each gun owns a small set of ROF items.
constexpr uint8_t kMaxRofItems       = 8;

/// One rate-of-fire preset. The operator picks which item is armed by
/// driving the ROF selector channel into the item's `[bandLoUs, bandHiUs]`
/// window. Bands MUST NOT overlap (Studio surfaces this as an error in
/// Phase 4 — Rule 38).
struct RofItem {
    char     name[16]    = {};           ///< friendly label ("burst" / "rapid")
    uint16_t bandLoUs    = 0;            ///< 0 = unbounded low
    uint16_t bandHiUs    = 0;            ///< 0 = unbounded high
    uint16_t rpm         = 600;          ///< rounds per minute when armed
    char     soundPath[64] = {};         ///< per-shot sample played each round
    /// Stereo routing — bit 0 = left, bit 1 = right.  0x03 (both) is
    /// the default; per-ROF so different gun bursts can pan to one
    /// side (e.g. a wing-mounted Vulcan vs a turret).
    uint8_t  outputMask  = 0x03;
};

/// Smoke-cartridge driver: separate heater + fan, each optional.
///
/// The smoke spec carries the ELEMENT RATING (heaterElementMv,
/// fanElementMv) — the rated voltage of the resistive heating wire and
/// the rated voltage of the fan motor.  The gun service passes these
/// down to HeaterRole / DcMotorRole at configure time; the role uses
/// `scaleDuty()` (element_scaling.h) to translate "100 %" intent into
/// port-native duty against the port's rail voltage.  Default 6000 mV
/// matches the common 6 V smoke-cartridge element / 6 V fan motor.
///
/// (Rule 42 originally placed element ratings on the role layer alone.
/// Phase 4 polish 2026-05-26: the smoke element is conceptually tied
/// to the GUN that drives it, not to the physical port — moving a
/// smoke harness to a different port shouldn't lose the element spec.
/// We keep the role-layer setElement() path; the gun service is now
/// the canonical source of values.)
struct SmokeConfig {
    // ── Heater (intent-level only — duty scaling lives on the role) ──
    //
    // Two modes (Phase 4 polish 2026-05-26):
    //   HM_CONTINUOUS — drive at element-scaled duty whenever the
    //     activation gate allows.  `heaterCycleOnMs` / `heaterCycleOffMs`
    //     are ignored in this mode.
    //   HM_CYCLE      — duty-cycle the heater on/off at gun-layer
    //     intervals.  When smoke is armed AND activation is high:
    //     drive heater for `heaterCycleOnMs`, then off for
    //     `heaterCycleOffMs`, then repeat.  Use this for power
    //     conservation OR to limit element temperature on smoke
    //     cartridges that can't sustain continuous drive.  No
    //     temperature sensor needed — this is open-loop timing
    //     managed by the gun, not by the role.
    //
    //   Closed-loop bang-bang (thermostatic) used to live here but was
    //   pulled out 2026-05-26 because HubFX has no temperature sensor
    //   on the smoke heater.  If a thermistor is wired later, the
    //   HeaterRole's bang-bang code in heater_role.cpp is still
    //   intact — re-introduce a `HM_BANG_BANG` value and route the
    //   target / hyst through HEATER_SET_TARGET / HEATER_SET_ELEMENT.
    PortRef  heaterPort;                 ///< Heater role on a Pwm port (optional)
    uint16_t heaterElementMv     = 6000; ///< rated element voltage (default 6 V)

    enum HeaterMode : uint8_t {
        HM_CONTINUOUS = 0,               ///< drive at element-scaled duty whenever activated
        HM_CYCLE      = 1,               ///< duty-cycle: on for cycleOnMs, off for cycleOffMs
    };
    uint8_t  heaterMode          = HM_CONTINUOUS;
    uint16_t heaterCycleOnMs     = 5000; ///< CYCLE: on-phase duration (default 5 s)
    uint16_t heaterCycleOffMs    = 3000; ///< CYCLE: off-phase duration (default 3 s)

    // Heater activation channel (Rule 43, NEW 2026-05-26) —
    // gates the heater behind a NAMED channel from /hubfx.yaml's
    // inputs[] block.  When the activation channel reads above
    // `heaterActivationThresholdUs` (with hysteresis), the heater
    // is allowed to drive; when below, the heater is forced off
    // regardless of smoke-arm state.  Empty `heaterActivationInput`
    // means "no gating" — heater is always allowed when smoke is armed.
    char     heaterActivationInput[kInputNameMax] = {};
    PortRef  heaterActivationPort;                  ///< resolved at apply time
    uint8_t  heaterActivationChannel       = 0;     ///< resolved at apply time (0-based)
    uint16_t heaterActivationThresholdUs   = 1500;
    uint16_t heaterActivationHysteresisUs  = 25;

    // ── Fan (intent-level only — duty scaling lives on the role) ─────
    PortRef  fanPort;                    ///< DcMotor role on a Pwm port (optional)
    uint16_t fanElementMv        = 6000; ///< rated motor voltage (default 6 V)
    enum FanMode : uint8_t {
        FN_CONTINUOUS = 0,               ///< fan runs at 100 % of element-rated voltage while firing
        FN_PULSE      = 1,               ///< sinusoidal envelope per shot: 50% base → 100% peak → 50% base
    };
    /// "Fan disabled" is encoded by `fanPort.portKind == 0` (no fan
    /// port bound) — every fan command in gun_unit early-returns on
    /// that.  There's no separate `FN_OFF` enum value anymore.
    uint8_t  fanMode             = FN_PULSE;
    /// FN_PULSE envelope duration (Phase 4 polish 2026-05-26).  On
    /// each shot the fan ramps from 50% (idle base) up to 100% (peak)
    /// then back down to 50% along a sinusoidal curve:
    ///   pct(t) = 50 + 50 * sin(π * t / fanPulseDurationMs), t∈[0, dur]
    /// Default 100 ms matches the default ROF firing rate
    /// (600 RPM = 100 ms / shot) so the sinusoid envelope completes
    /// once per shot at the default cadence.  Faster ROFs collapse
    /// adjacent envelopes (each shot resets the sinusoid clock); the
    /// motor's inertia smooths the resulting waveform.  Operator
    /// overrides via /gunfx.yaml's `pulse_duration_ms`.
    uint16_t fanPulseDurationMs  = 100;
};

/// One axis (yaw or pitch) of a turret. Each axis is INDEPENDENTLY
/// optional — leave `enabled = false` to skip the axis entirely.
///
/// Motion profile lives on the `ServoActuatorRole` attached to
/// `servoPort` (Rule 42); storage is `/hubfx.yaml`'s ports[] block.
/// Rule 44 only refines the editing surface (feature panel inline,
/// not the IO tab) — the gun never carries profile fields.
struct GunAxis {
    bool     enabled       = false;
    PortRef  servoPort;                  ///< ServoActuator port (output)

    // Named-channel reference (Rule 43): the operator picks from
    // /hubfx.yaml's inputs[] block by name.  `applyGunFxConfig` resolves
    // it against the parsed HubFxConfig and populates `inputPort` +
    // `inputChannel` BEFORE the service sees the spec — the GunUnit
    // never sees the name, only the resolved port + channel.
    char     inputName[kInputNameMax] = {};
    PortRef  inputPort;                  ///< resolved at apply time (do not set in YAML)
    uint8_t  inputChannel  = 0;          ///< resolved at apply time (0-based)

    uint16_t neutralUs     = 1500;       ///< target sent when no input bound (gun-side fallback)
};

/// One gun. Up to `kMaxGuns` of these in `GunFxConfig`.
struct GunSpec {
    uint8_t   id              = 0;
    char      name[16]        = {};

    // ── Trigger (fire on/off) ─ Rule 43: named-channel reference ─────
    // `triggerInput` is the name from /hubfx.yaml's inputs[] block.
    // `triggerPort` + `triggerChannel` are resolved at apply time by
    // applyGunFxConfig — never set them by hand.
    char      triggerInput[kInputNameMax] = {};
    PortRef   triggerPort;                          ///< resolved at apply time
    uint8_t   triggerChannel       = 0;             ///< resolved at apply time
    uint16_t  triggerThresholdUs   = 1500;
    uint16_t  triggerHysteresisUs  = 25;

    // ── ROF selector + table ─ Rule 43: named-channel reference ──────
    char      rofSelectorInput[kInputNameMax] = {};
    PortRef   rofSelectorPort;                      ///< resolved at apply time
    uint8_t   rofSelectorChannel   = 0;             ///< resolved at apply time
    uint8_t   numRofItems          = 0;
    RofItem   rofItems[kMaxRofItems] = {};

    // ── Muzzle flash (LedAnimator on any LED-capable Pwm port) ────────
    PortRef   muzzleFlashPort;
    uint16_t  flashDurationMs      = 30;
    uint8_t   flashBrightness      = 100;

    // ── Recoil (TURRET BEHAVIOUR — no dedicated servo) ────────────────
    // The dedicated recoil-servo port was removed (Phase 4, 2026-05-23).
    // 2026-06: recoil is now an INSTANT, RANDOM jerk applied to BOTH
    // turret axes on each shot — each of yaw + pitch snaps (bypassing the
    // motion profile) to its commanded position ± a random offset up to
    // `recoilJerkUs`, holds `recoilHoldMs`, then snaps back.  The axis
    // picker was dropped (both axes always kick).  Aiming still slews via
    // ServoActuatorRole; only the recoil kick/return is instant.
    bool      recoilEnabled        = true;          ///< apply on each shot when a turret axis is enabled
    uint16_t  recoilJerkUs         = 200;           ///< MAX random |offset| from commanded µs on a shot
    uint16_t  recoilHoldMs         = 80;            ///< time at the kicked position before snap-back

    // ── Smoke (heater + fan) ──────────────────────────────────────────
    SmokeConfig smoke;

    // ── Yaw + pitch (each independently optional) ─────────────────────
    GunAxis   yaw;
    GunAxis   pitch;
};

/// Whole-effect config — owned by `ConfigStore<GunFxConfig>` on the hub
/// and loaded from `/gunfx.yaml`.
struct GunFxConfig {
    bool      enabled        = false;
    uint8_t   numGuns        = 0;
    /// `kMaxGuns` mirror of GunFxServicePolicy's compile-time cap. Kept
    /// in sync via a static_assert in gunfx_service.h (Phase 2).
    static constexpr uint8_t kMaxGunsCfg = 4;
    GunSpec   guns[kMaxGunsCfg] = {};
};

}  // namespace hubfx::effects::gunfx

#endif  // HUBFX_GUNFX_CONFIG_H
