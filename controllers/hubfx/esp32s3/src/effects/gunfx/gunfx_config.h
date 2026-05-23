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
 *       channel isn't bound) and the recoil's `recoilCenterUs` /
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
};

/// Smoke-cartridge driver: separate heater + fan, each optional.
///
/// Element voltage + scaling for the heater + fan elements live ON
/// THE ROLE LAYER (HeaterRole / DcMotorRole, configured via
/// /hubfx.yaml's `ports[]` role-attach config) — NOT here.  The gun
/// only sees the intent layer ("heat to T °C", "puff for X ms").
struct SmokeConfig {
    // ── Heater (intent-level only — duty scaling lives on the role) ──
    PortRef  heaterPort;                 ///< Heater role on a Pwm port (optional)

    enum HeaterMode : uint8_t {
        HM_ALWAYS_ON   = 0,              ///< role drives at its configured drivePct while armed
        HM_BANG_BANG   = 1,              ///< toggle on temp; falls back to ALWAYS_ON without sensor
        HM_CLOSED_LOOP = 2,              ///< delegate to HeaterRole's bang-bang PID
    };
    uint8_t  heaterMode          = HM_ALWAYS_ON;
    int16_t  heaterTargetCx10    = 1500; ///< 150.0 °C, BANG_BANG / CLOSED_LOOP target
    int16_t  heaterHystCx10      = 50;   ///< BANG_BANG ±5.0 °C deadband

    // ── Fan (intent-level only — duty scaling lives on the role) ─────
    PortRef  fanPort;                    ///< DcMotor role on a Pwm port (optional)
    enum FanMode : uint8_t {
        FN_OFF                 = 0,
        FN_CONTINUOUS          = 1,      ///< fan runs at role's drivePct while firing
        FN_PUFF_PER_SHOT       = 2,      ///< one `fanPuffMs` pulse per fired shot
        FN_PUFF_ON_FIRE_ACTIVE = 3,      ///< one pulse on the rising edge of fire flag
    };
    uint8_t  fanMode             = FN_OFF;
    uint16_t fanPuffMs           = 200;
};

/// One axis (yaw or pitch) of a turret. Each axis is INDEPENDENTLY
/// optional — leave `enabled = false` to skip the axis entirely.
///
/// Motion profile (clamp / speed / accel / jerk) for the servo lives
/// on `ServoActuatorRole`, configured at port-attach time via
/// `/hubfx.yaml`'s `ports[]` block (Rule 42).  GunAxis carries only
/// the intent — which named channel drives the axis (Rule 43) and the
/// fallback µs it commands when the channel isn't bound (`neutralUs`).
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

    // ── Recoil servo ──────────────────────────────────────────────────
    // The servo's motion shape (slew limits) lives on ServoActuatorRole.
    // The fields here describe the RECOIL PULSE shape — what GunFx does
    // with the servo: kick it forward by `recoilJerkUs` from
    // `recoilCenterUs`, hold for `recoilHoldMs`, then return to center.
    PortRef   recoilServoPort;
    uint16_t  recoilCenterUs       = 1500;          ///< servo "at rest" position
    uint16_t  recoilJerkUs         = 200;           ///< delta from centerUs on a shot
    uint16_t  recoilHoldMs         = 80;            ///< time at jerk position before return

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
