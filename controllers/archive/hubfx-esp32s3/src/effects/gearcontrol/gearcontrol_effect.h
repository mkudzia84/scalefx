/*
 * GearControlEffect — master-side orchestrator for GearControl-class slaves.
 *
 * Status: scaffolding for the GearControl migration step.  The
 * sequencer state machines (DoorSequencer + GearSequencer) are fully
 * implemented; outbound emission to SlaveApi is left as stub
 * callbacks that the integration step will wire up.
 *
 * See instructions/15-GENERIC-SLAVE-REFACTOR.md § "Step 2 — GearControl".
 *
 * Slave fingerprint:
 *   6 servos = 4 bay doors + main gear leg + steering yaw
 *   2 PWM   = retract motor (current-sensed) + brake
 *   0 LEDs
 *
 * Master responsibilities:
 *   - per-gear cycle state machine driven by SERVO_TARGET_REACHED + PWM_STALL
 *   - multi-gear coordination (DoorSync / Sequenced / Independent)
 *   - steering yaw direct binding to RC input
 *   - brake interlock policy
 *   - battery voltage monitor + cutoff policy
 *
 * The orchestrator owns:
 *   - one DoorSequencer per gear (currently 2: front + rear) with stub
 *     ServoCommandFn callbacks routing to SlaveApi::servoSet
 *   - one GearSequencer per gear with stub MotorFn / StallStateFn
 *     callbacks routing to SlaveApi PWM commands + the PWM_STALL
 *     observer chain
 *   - a top-level GearOrchestrator multiplexer that selects sync vs
 *     independent operation across the two gears
 */

#ifndef HUBFX_GEARCONTROL_EFFECT_H
#define HUBFX_GEARCONTROL_EFFECT_H

#include <cstdint>
#include <array>

#include "door_sequencer.h"
#include "gear_sequencer.h"

namespace hubfx::effects::gearcontrol {

// ── Slave port layout (compile-time wiring) ──────────────────────────

namespace SlaveLayout {
    // Servos
    constexpr uint8_t SERVO_DOOR_FL  = 0;   ///< front-left bay door
    constexpr uint8_t SERVO_DOOR_FR  = 1;   ///< front-right bay door
    constexpr uint8_t SERVO_DOOR_RL  = 2;   ///< rear-left bay door
    constexpr uint8_t SERVO_DOOR_RR  = 3;   ///< rear-right bay door
    constexpr uint8_t SERVO_GEAR_LEG = 4;   ///< main gear extend/retract (servo-driven)
    constexpr uint8_t SERVO_YAW      = 5;   ///< steering yaw

    // PWM
    constexpr uint8_t PWM_RETRACT    = 0;   ///< gear-leg retract motor (current-sensed)
    constexpr uint8_t PWM_BRAKE      = 1;
}

// ── Coordination policy across the two gears (front / rear) ──────────
//
// The slave has 4 doors split 2+2 across "front" and "rear" bays.
// Each bay has its own DoorSequencer instance.  The high-level
// orchestrator picks how the two gears advance through the cycle:
//
enum class GearCoordMode : uint8_t {
    Independent  = 0,   ///< each gear deploys/retracts on its own
    DoorSync     = 1,   ///< doors open together, then each gear runs
                        ///<   its motor independently, then doors close
                        ///<   together — uses the GearSequencer sync
                        ///<   barrier on door-open + close
    FullSync     = 2,   ///< doors open together, motors run together,
                        ///<   doors close together (3-way barrier)
    Sequenced    = 3,   ///< front gear cycles fully, then rear gear
};

// ── Top-level cycle phase (broadcast in HubFX STATUS payload) ────────

enum class GearPhase : uint8_t {
    Stowed                  = 0,
    OpeningDoors            = 1,
    DoorsOpen               = 2,
    ExtendingGear           = 3,
    GearExtended            = 4,
    ClosingDoors            = 5,
    Deployed                = 6,
    RetractingGear          = 7,
    GearStowed_AwaitingDoors = 8,
    FaultSafe               = 0xFF,
};

enum class BatteryAlarm : uint8_t {
    Ok      = 0,
    Warning = 1,
    Cutoff  = 2,
};

// ── Configuration ────────────────────────────────────────────────────

struct GearControlConfig {
    // Door + gear-leg target microseconds
    uint16_t doorClosed_us   = 1000;
    uint16_t doorOpen_us     = 2000;
    uint16_t gearStowed_us   = 1000;
    uint16_t gearExtended_us = 2000;

    // Door sequencing modes (mirrors slave-side preDeploy/postDeploy)
    uint8_t  doorPreDeployMode  = ::DoorMode::DUAL_SYNC;
    uint8_t  doorPostDeployMode = ::DoorMode::NONE;
    uint16_t doorDelay_ms       = 500;

    // Multi-gear coordination
    GearCoordMode coordMode = GearCoordMode::DoorSync;

    // Stall / motor policy forwarded to GearSequencer on each motor op
    GearSequencer::StallConfig stall{
        /*startupIgnore_ms*/        300,
        /*motorDetectThreshold_mA*/ 80,
        /*stallThreshold_mA*/       1500,
        /*stallConfirm_ms*/         150,
        /*timeout_ms*/              5000,
    };

    // Cycle-level fail-safe ceiling — primary driver remains the
    // SERVO_TARGET_REACHED + PWM_STALL event chain.
    uint32_t phaseTimeout_ms = 8000;

    // RC bindings (1-based aliases; 0 = unbound)
    uint8_t rcGearChannel  = 0;
    uint8_t rcBrakeChannel = 0;
    uint8_t rcYawChannel   = 0;

    // Battery policy
    uint8_t  battery_chemistry             = 0;     ///< 0=LiPo, 1=LiFe, 2=NiMH
    uint8_t  battery_cellCount             = 3;
    uint16_t battery_warning_mV_per_cell   = 3500;
    uint16_t battery_cutoff_mV_per_cell    = 3300;
};

// Forward declarations (master-side helpers wired by the integration step)
class SlaveApi;
class RcInputs;
class HubStatusBuilder;

// ── Per-gear bundle ─────────────────────────────────────────────────
//
// Two gears (front, rear) — each with its own door sequencer + gear
// sequencer.  Bundle keeps them grouped so the orchestrator can iterate.

struct GearUnit {
    DoorSequencer  doors;
    GearSequencer  cycle;
    uint8_t        servoDoor0 = 0;   ///< slave servo channel for door 0
    uint8_t        servoDoor1 = 0;   ///< slave servo channel for door 1
    uint8_t        servoLeg   = 0;   ///< slave servo channel for the gear-leg servo (if any)
    uint8_t        pwmRetract = 0;   ///< slave PWM channel for the retract motor
    bool           syncDoorsOpenReady = false;   ///< latched at the doors-open barrier
    bool           syncMotorDoneReady = false;   ///< latched at the motor-done barrier
};

// ── GearControlEffect ────────────────────────────────────────────────

class GearControlEffect {
public:
    bool begin(SlaveApi*         slave,
               RcInputs*         rc,
               HubStatusBuilder* statusOut);

    void applyConfig(const GearControlConfig& cfg);

    /// Drive both gears + sync coordinator + battery monitor + RC dispatch.
    void update();

    // ── Slave-async hooks (wired by the host) ────────────────────────

    /// Forward SERVO_TARGET_REACHED for any of the gear / door servos.
    /// Internally routes to the correct GearUnit's DoorSequencer or the
    /// gear-leg sequencer barrier as appropriate.
    void onServoTargetReached(uint8_t servoIdx, uint16_t pos_us);

    /// Forward PWM_STALL for the retract motor channels.  Latches the
    /// verdict for the corresponding GearSequencer's StallStateFn.
    void onPwmStall(uint8_t pwmIdx, uint16_t peak_mA, uint16_t duration_ms);

    // ── Public commands (CLI / Studio / RC) ──────────────────────────

    bool requestDeploy();      ///< start a deploy on every configured gear
    bool requestRetract();     ///< start a retract on every configured gear
    bool emergencyStop();      ///< park all servos, drop PWMs to 0

    // ── Telemetry ────────────────────────────────────────────────────

    GearPhase    currentPhase()        const { return _phase; }
    BatteryAlarm currentBatteryAlarm() const { return _batteryAlarm; }
    uint16_t     lastBatteryVoltage_mV() const { return _lastVoltage_mV; }
    int32_t      lastRetractCurrent_mA() const { return _lastCurrent_mA; }

private:
    SlaveApi*         _slave  = nullptr;
    RcInputs*         _rc     = nullptr;
    HubStatusBuilder* _status = nullptr;
    GearControlConfig _cfg{};

    // Two gears: front and rear.  Index 0 = front, 1 = rear.
    static constexpr uint8_t kGearCount = 2;
    std::array<GearUnit, kGearCount> _gears{};

    GearPhase    _phase           = GearPhase::Stowed;
    uint32_t     _phaseEntered_ms = 0;
    BatteryAlarm _batteryAlarm    = BatteryAlarm::Ok;
    uint16_t     _lastVoltage_mV  = 0;
    int32_t      _lastCurrent_mA  = 0;

    // Stall observer state — latched per gear, consulted by the
    // GearSequencer's StallStateFn callback.
    struct StallObserverState {
        StallVerdict latched          = StallVerdict::RUNNING;
        uint32_t     armedAt_ms       = 0;
        uint32_t     timeout_ms       = 0;
    };
    std::array<StallObserverState, kGearCount> _stallObs{};

    // Wiring helpers (called once from begin()).  Each binds the
    // sequencer stubs to the orchestrator's emission paths — until the
    // SlaveApi-routing integration step lands these stay as in-class
    // closures that no-op the actual wire emission and just trace.
    void wireGearUnit(uint8_t gearIdx);

    // Coordination helpers
    void startCycle(bool deploying);
    void onPhaseChange(uint8_t gearIdx, bool finished, bool error);
    void releaseSyncBarriersIfReady();

    // Misc
    bool isInterlockSafe(GearPhase nextPhase) const;
    void enterFaultSafe();
    void pollBatteryAndApplyPolicy();
};

}  // namespace hubfx::effects::gearcontrol

#endif  // HUBFX_GEARCONTROL_EFFECT_H
