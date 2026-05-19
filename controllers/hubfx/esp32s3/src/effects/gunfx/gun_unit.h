/*
 * gun_unit.h — single gun state machine for the GunFX effect.
 *
 *   Per-unit: drives one muzzle-flash LED (LedAnimator role), an
 *   optional recoil servo (ServoActuator), and an optional smoke
 *   heater (Heater role).  Single-shot and auto-fire modes are both
 *   supported.  All wire activity flows through topology callbacks
 *   that the owning service installs at `configure()` time — keeping
 *   the state machine independent of transport templates.
 *
 *   Each shot does, atomically (one topology batch):
 *     - LED_QUEUE_LOAD with a single short flash event + LED_START
 *     - SERVO_SET_TARGET to the jerk position (if recoil servo present)
 *     - Caller schedules an audio playAsync separately (the unit is
 *       transport-agnostic about audio).
 *
 *   Recoil "return to center" happens on the next `update()` tick
 *   after `recoilHoldMs` has elapsed since the shot — simple
 *   time-driven, no async callback required.
 */

#ifndef HUBFX_GUN_UNIT_H
#define HUBFX_GUN_UNIT_H

#include <cstdint>

#include "../effect_id.h"
#include "gunfx_protocol.h"

namespace hubfx::effects::gunfx {

struct GunDef {
    uint8_t  id            = 0;
    char     name[16]      = {};

    PortRef  muzzleFlash;                          ///< LedAnimator port (required)
    PortRef  recoilServo;                          ///< ServoActuator port (optional; portKind==0 = none)
    PortRef  smokeHeater;                          ///< Heater port (optional)
    PortRef  trigger;                              ///< RcPwmInput port (optional auto-fire)

    uint16_t flashDurationMs   = 30;
    uint8_t  flashBrightness   = 100;

    uint16_t recoilCenterUs    = 1500;
    uint16_t recoilJerkUs      = 200;
    uint16_t recoilHoldMs      = 80;

    int16_t  smokeTargetCx10   = 1500;             ///< 150.0 °C, only used while armed
    uint16_t triggerThresholdUs = 1500;
    uint16_t defaultIntervalMs = 100;              ///< auto-fire fallback (600 RPM)

    char     fireSoundPath[64] = {};               ///< optional, played via service callback
    uint8_t  audioChannel      = 0;                ///< set from `HubFxLayout::Gun0` etc.
    uint8_t  outputMask        = 0x03;             ///< AudioWire::OUTPUT_ALL by default
};

class GunUnit {
public:
    using SendRoleCmdFn = bool (*)(void* ctx, const PortRef& addr,
                                   uint8_t innerType,
                                   const uint8_t* payload, size_t len);
    using BatchFn       = void (*)(void* ctx);
    using ShotEventFn   = void (*)(void* ctx, uint8_t id,
                                   const char* soundPath,
                                   uint8_t audioChannel,
                                   uint8_t outputMask);

    GunUnit() = default;

    void configure(const GunDef& def,
                   SendRoleCmdFn sendFn, void* sendCtx,
                   BatchFn beginFn, BatchFn commitFn,
                   ShotEventFn shotFn = nullptr, void* shotCtx = nullptr) {
        _def      = def;
        _send     = sendFn;
        _sendCtx  = sendCtx;
        _begin    = beginFn;
        _commit   = commitFn;
        _shot     = shotFn;
        _shotCtx  = shotCtx;
        _firing   = false;
        _smokeArmed = false;
    }

    const GunDef& def() const { return _def; }
    uint8_t id() const        { return _def.id; }
    bool firing() const       { return _firing; }
    bool smokeArmed() const   { return _smokeArmed; }

    void fireOnce();
    void startFiring(uint16_t rpm);
    void stopFiring();
    void armSmoke(bool armed);

    /// Tick — drives auto-fire scheduling and recoil-return timing.
    void update(uint32_t nowMs);

    /// Forwarded by the service on RCIN_VALUE_BROADCAST for our
    /// configured trigger port.
    void onTriggerInput(uint16_t pulseUs, bool valid);

private:
    void doShot();
    void returnRecoil();
    void commandFlash();
    void commandRecoilJerk();
    void commandHeater(bool on);

    GunDef        _def{};
    bool          _firing               = false;
    bool          _smokeArmed           = false;
    uint32_t      _shotIntervalMs       = 0;
    uint32_t      _nextShotMs           = 0;
    uint32_t      _recoilReturnAtMs     = 0;
    bool          _triggerHeld          = false;

    SendRoleCmdFn _send     = nullptr;
    void*         _sendCtx  = nullptr;
    BatchFn       _begin    = nullptr;
    BatchFn       _commit   = nullptr;
    ShotEventFn   _shot     = nullptr;
    void*         _shotCtx  = nullptr;
};

}  // namespace hubfx::effects::gunfx

#include "gun_unit.ipp"

#endif  // HUBFX_GUN_UNIT_H
