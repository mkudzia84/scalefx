/*
 * GunFxEffect — master-side orchestrator for GunFX-class slaves.
 *
 * Status: scaffolding only — no implementation.  Captures the API
 * surface so the master-side effect graph can compile against a
 * concrete type before the body lands during the GunFX migration
 * (see instructions/15-GENERIC-SLAVE-REFACTOR.md § "Step 1 — GunFX").
 *
 * In the post-pivot architecture the slave exposes:
 *   - 1 servo            (recoil)
 *   - 3 PWM channels     (trigger motor, smoke heater, smoke fan)
 *                        with current-sense on heater
 * All firing semantics — rate-of-fire scheduling, burst counters,
 * smoke warm-up timing, recoil synchronisation — live HERE.
 */

#ifndef HUBFX_GUNFX_EFFECT_H
#define HUBFX_GUNFX_EFFECT_H

#include <cstdint>

namespace hubfx::effects::gunfx {

/// Channel allocation on the slave side.  Compile-time constants —
/// match against `COMPONENT_LIST_RESP` at attach time and refuse to
/// engage if the slave's fingerprint disagrees.
namespace SlaveLayout {
    constexpr uint8_t SERVO_RECOIL    = 0;
    constexpr uint8_t PWM_TRIGGER     = 0;   ///< PwmMotor / PwmGeneric
    constexpr uint8_t PWM_SMOKE_HEAT  = 1;   ///< PwmGeneric (current-sensed via INA226)
    constexpr uint8_t PWM_SMOKE_FAN   = 2;   ///< PwmGeneric
}

enum class TriggerMode : uint8_t {
    Disarmed     = 0,
    SingleShot   = 1,
    Burst        = 2,   ///< fires `_burstRounds` per pull
    FullAuto     = 3,
};

enum class SmokeState : uint8_t {
    Off       = 0,
    WarmingUp = 1,
    Armed     = 2,
    Firing    = 3,
    CoolDown  = 4,
};

/// Configuration loaded from `/gunfx.yaml` on the hub's flash.
struct GunFxConfig {
    TriggerMode triggerMode      = TriggerMode::SingleShot;
    uint16_t    rateOfFire_rpm   = 600;     ///< rounds-per-minute (full-auto / burst)
    uint8_t     burstRounds      = 3;
    uint16_t    triggerOnTime_ms = 50;      ///< trigger motor pulse width
    uint16_t    triggerOffTime_ms = 50;     ///< gap between trigger pulses

    uint16_t    recoilFire_us    = 2000;    ///< servo target on fire
    uint16_t    recoilRest_us    = 1500;    ///< servo target between rounds

    uint16_t    smokeHeatDuty    = 800;     ///< 0..1000
    uint16_t    smokeFanDuty     = 1000;
    uint32_t    smokeWarmup_ms   = 4000;
    uint32_t    smokeCooldown_ms = 2000;
    uint16_t    smokeMaxCurrent_mA = 1500;  ///< heater cutoff threshold

    uint8_t     rcTriggerChannel = 1;       ///< 1-based RC input alias
    uint8_t     rcSmokeChannel   = 2;
};

/// Forward declarations for collaborators (defined elsewhere in hubfx).
class SlaveApi;
class RcInputs;
class HubStatusBuilder;

class GunFxEffect {
public:
    /// Bind the orchestrator to a concrete generic slave + RC input
    /// frontend + status builder.  Validates the slave's component
    /// fingerprint against `SlaveLayout` and returns false if the
    /// slave doesn't carry the expected components.
    bool begin(SlaveApi*         slave,
               RcInputs*         rc,
               HubStatusBuilder* statusOut);

    /// Apply config (re)load — invoked from the master's
    /// `gunFxConfig.onLoaded()` callback.
    void applyConfig(const GunFxConfig& cfg);

    /// Drive the state machine — call from main loop().
    void update();

    /// Slave-async hooks — invoked from the SlaveApi observer chain
    /// when the slave emits SERVO_TARGET_REACHED on the recoil channel.
    void onServoTargetReached(uint8_t idx, uint16_t pos_us);

    /// Manual triggers (CLI / Studio) — bypass RC input.
    void manualFireOnce();
    void manualFireStart();
    void manualFireStop();
    void manualSmokeArm(bool on);

    // ── Telemetry ────────────────────────────────────────────────────
    TriggerMode currentTriggerMode() const;
    SmokeState  currentSmokeState()  const;
    uint16_t    lastHeaterCurrent_mA() const;
    uint16_t    lastRecoilPos_us()    const;

private:
    SlaveApi*         _slave    = nullptr;
    RcInputs*         _rc       = nullptr;
    HubStatusBuilder* _status   = nullptr;
    GunFxConfig       _cfg{};

    // ── Trigger state ────────────────────────────────────────────────
    bool      _triggerHeld          = false;   ///< RC / manual current state
    uint8_t   _roundsRemaining      = 0;       ///< for Burst mode
    uint32_t  _nextTriggerEdge_ms   = 0;       ///< RoF scheduler
    bool      _triggerMotorOn       = false;
    bool      _recoilArmedReturn    = false;   ///< awaiting SERVO_TARGET_REACHED

    // ── Smoke state ──────────────────────────────────────────────────
    SmokeState _smoke               = SmokeState::Off;
    uint32_t   _smokeStateEntered_ms = 0;
    uint16_t   _lastHeaterCurrent_mA = 0;

    // ── Internal helpers (private impl in .cpp) ──────────────────────
    void   pulseTrigger();
    void   transitionSmoke(SmokeState next);
    bool   matchesFingerprint() const;
};

}  // namespace hubfx::effects::gunfx

#endif  // HUBFX_GUNFX_EFFECT_H
