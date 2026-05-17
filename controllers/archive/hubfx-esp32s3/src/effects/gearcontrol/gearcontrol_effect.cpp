/*
 * GearControlEffect — implementation.
 *
 * Wires the per-gear DoorSequencer + GearSequencer pair to the slave
 * via stub callbacks: outbound emission paths (servo command, motor
 * direction, stall observer arm) are bound to local lambdas that will
 * later be replaced with SlaveApi calls.  Until that integration
 * lands, the orchestrator is fully functional in terms of state-
 * machine logic — it simply emits no wire packets and records its
 * intent through the stub paths.
 */

#include "gearcontrol_effect.h"

#include <Arduino.h>

namespace hubfx::effects::gearcontrol {

namespace {
    constexpr uint8_t GEAR_FRONT = 0;
    constexpr uint8_t GEAR_REAR  = 1;
}

// ── begin() / applyConfig() ──────────────────────────────────────────

bool GearControlEffect::begin(SlaveApi*         slave,
                              RcInputs*         rc,
                              HubStatusBuilder* statusOut) {
    _slave  = slave;
    _rc     = rc;
    _status = statusOut;

    // Servo / PWM channel assignments per gear (compile-time wiring;
    // mirrors SlaveLayout constants).
    _gears[GEAR_FRONT].servoDoor0 = SlaveLayout::SERVO_DOOR_FL;
    _gears[GEAR_FRONT].servoDoor1 = SlaveLayout::SERVO_DOOR_FR;
    _gears[GEAR_FRONT].servoLeg   = SlaveLayout::SERVO_GEAR_LEG;
    _gears[GEAR_FRONT].pwmRetract = SlaveLayout::PWM_RETRACT;

    _gears[GEAR_REAR].servoDoor0  = SlaveLayout::SERVO_DOOR_RL;
    _gears[GEAR_REAR].servoDoor1  = SlaveLayout::SERVO_DOOR_RR;
    _gears[GEAR_REAR].servoLeg    = SlaveLayout::SERVO_GEAR_LEG;   // shared leg in current layout
    _gears[GEAR_REAR].pwmRetract  = SlaveLayout::PWM_RETRACT;      // shared motor in current layout

    for (uint8_t i = 0; i < kGearCount; i++) wireGearUnit(i);

    applyConfig(_cfg);
    return true;
}

void GearControlEffect::applyConfig(const GearControlConfig& cfg) {
    _cfg = cfg;

    DoorTargets door0Targets{ cfg.doorOpen_us, cfg.doorClosed_us };
    DoorTargets door1Targets{ cfg.doorOpen_us, cfg.doorClosed_us };

    for (auto& g : _gears) {
        g.doors.setTargets(0, door0Targets);
        g.doors.setTargets(1, door1Targets);
        g.doors.setMode(cfg.doorPreDeployMode,
                        cfg.doorPostDeployMode,
                        cfg.doorDelay_ms);
    }
}

// ── Per-gear stub wiring ─────────────────────────────────────────────

void GearControlEffect::wireGearUnit(uint8_t gearIdx) {
    if (gearIdx >= kGearCount) return;
    auto& g = _gears[gearIdx];

    // DoorSequencer outbound: route 0/1 door slot to slave servo channel.
    g.doors.begin(
        [this, gearIdx](uint8_t doorSlot, uint16_t pulse_us) {
            // STUB: route to SlaveApi::servoSet once integration lands.
            // For now, capture the target + slot so the orchestrator's
            // state machine still sees its own emission intent if it
            // needs to (no-op sink).
            (void)gearIdx; (void)doorSlot; (void)pulse_us;
        },
        DoorTargets{},
        DoorTargets{});

    // GearSequencer outbound: motor direction + stall observer wiring.
    g.cycle.begin(
        &g.doors,
        // MotorFn — drive the retract motor channel.  STUB until
        // SlaveApi PWM_SET_DUTY routing is in place.
        [this, gearIdx](int8_t direction) {
            (void)gearIdx; (void)direction;
        },
        // StallStateFn — return the latched verdict.
        [this, gearIdx]() -> StallVerdict {
            if (gearIdx >= kGearCount) return StallVerdict::RUNNING;
            const auto& obs = _stallObs[gearIdx];
            // Software wall-clock fail-safe in case the slave never
            // reports stall (e.g. during the master/slave integration
            // bring-up window when PWM_STALL events aren't observed).
            if (obs.timeout_ms != 0 &&
                (millis() - obs.armedAt_ms) > obs.timeout_ms) {
                return StallVerdict::TIMEOUT_STALL;
            }
            return obs.latched;
        },
        // ArmStallObserverFn — clear the latch on motor-op start.
        [this, gearIdx](uint32_t timeout_ms) {
            if (gearIdx >= kGearCount) return;
            _stallObs[gearIdx] = StallObserverState{
                StallVerdict::RUNNING,
                millis(),
                timeout_ms,
            };
        });

    g.cycle.onPhaseChange(
        [this, gearIdx](bool finished, bool error) {
            onPhaseChange(gearIdx, finished, error);
        });
}

// ── update() ─────────────────────────────────────────────────────────

void GearControlEffect::update() {
    for (auto& g : _gears) {
        g.doors.update();
        g.cycle.update();
    }
    releaseSyncBarriersIfReady();
    pollBatteryAndApplyPolicy();
}

// ── Cycle entry points ───────────────────────────────────────────────

bool GearControlEffect::requestDeploy() {
    if (!isInterlockSafe(GearPhase::OpeningDoors)) return false;
    startCycle(/*deploying=*/true);
    return true;
}

bool GearControlEffect::requestRetract() {
    if (!isInterlockSafe(GearPhase::RetractingGear)) return false;
    startCycle(/*deploying=*/false);
    return true;
}

bool GearControlEffect::emergencyStop() {
    for (auto& g : _gears) g.cycle.stop();
    enterFaultSafe();
    return true;
}

void GearControlEffect::startCycle(bool deploying) {
    _phase           = deploying ? GearPhase::OpeningDoors
                                 : GearPhase::RetractingGear;
    _phaseEntered_ms = millis();

    const bool sync = (_cfg.coordMode == GearCoordMode::DoorSync ||
                       _cfg.coordMode == GearCoordMode::FullSync);

    switch (_cfg.coordMode) {
        case GearCoordMode::Independent:
        case GearCoordMode::DoorSync:
        case GearCoordMode::FullSync:
            for (auto& g : _gears) {
                g.syncDoorsOpenReady = false;
                g.syncMotorDoneReady = false;
                g.cycle.start(deploying, sync, _cfg.stall);
            }
            break;
        case GearCoordMode::Sequenced:
            // Front first; rear is kicked off by onPhaseChange when
            // the front completes.
            _gears[GEAR_FRONT].syncDoorsOpenReady = false;
            _gears[GEAR_FRONT].syncMotorDoneReady = false;
            _gears[GEAR_FRONT].cycle.start(deploying, /*syncMode=*/false, _cfg.stall);
            break;
    }
}

// ── Async-event ingress ──────────────────────────────────────────────

void GearControlEffect::onServoTargetReached(uint8_t servoIdx, uint16_t /*pos_us*/) {
    for (auto& g : _gears) {
        if (servoIdx == g.servoDoor0) g.doors.notifyTargetReached(0);
        if (servoIdx == g.servoDoor1) g.doors.notifyTargetReached(1);
        // SERVO_GEAR_LEG is currently driven via the PWM motor path;
        // when a future hardware variant exposes a dedicated leg servo
        // we'd notify the per-gear cycle barrier here as well.
    }
}

void GearControlEffect::onPwmStall(uint8_t pwmIdx,
                                   uint16_t peak_mA,
                                   uint16_t /*duration_ms*/) {
    _lastCurrent_mA = (int32_t)peak_mA;
    for (uint8_t i = 0; i < kGearCount; i++) {
        if (pwmIdx == _gears[i].pwmRetract) {
            _stallObs[i].latched = StallVerdict::STALL_CONFIRMED;
        }
    }
}

// ── Sync-barrier coordinator ────────────────────────────────────────

void GearControlEffect::releaseSyncBarriersIfReady() {
    if (_cfg.coordMode != GearCoordMode::DoorSync &&
        _cfg.coordMode != GearCoordMode::FullSync) {
        return;
    }

    // DoorSync: release the doors-open barrier on every gear once both
    // gears are sitting at SYNC_DOORS_OPEN.  Then later release the
    // motor-done barrier once both gears have completed their motor
    // op (FullSync uses the same release rule for both barriers).
    bool allDoorsOpen  = true;
    bool allMotorsDone = true;
    for (auto& g : _gears) {
        if (!g.cycle.isWaitingSyncDoorsOpen()) allDoorsOpen  = false;
        if (!g.cycle.isWaitingSyncMotorDone()) allMotorsDone = false;
    }
    if (allDoorsOpen)  for (auto& g : _gears) g.cycle.advanceSyncPhase();
    if (allMotorsDone) for (auto& g : _gears) g.cycle.advanceSyncPhase();
}

// ── Per-gear phase callback ─────────────────────────────────────────

void GearControlEffect::onPhaseChange(uint8_t gearIdx,
                                     bool    finished,
                                     bool    error) {
    if (error) {
        enterFaultSafe();
        return;
    }
    if (!finished) return;

    // Sequenced mode: kick off the next gear when the previous finishes.
    if (_cfg.coordMode == GearCoordMode::Sequenced && gearIdx == GEAR_FRONT) {
        const bool deploying = _gears[GEAR_FRONT].cycle.deploying();
        _gears[GEAR_REAR].cycle.start(deploying, /*syncMode=*/false, _cfg.stall);
        return;
    }

    // All gears done?
    bool anyActive = false;
    for (auto& g : _gears) anyActive = anyActive || g.cycle.isActive();
    if (!anyActive) {
        const bool deploying = _gears[gearIdx].cycle.deploying();
        _phase           = deploying ? GearPhase::Deployed : GearPhase::Stowed;
        _phaseEntered_ms = millis();
    }
}

// ── Misc helpers ────────────────────────────────────────────────────

bool GearControlEffect::isInterlockSafe(GearPhase /*nextPhase*/) const {
    // Brake interlock + battery-cutoff guards land with the SlaveApi
    // wiring step.  For now the orchestrator allows transitions
    // unconditionally — the GearSequencer's own preemption logic
    // handles in-flight conflicts.
    return _batteryAlarm != BatteryAlarm::Cutoff;
}

void GearControlEffect::enterFaultSafe() {
    for (auto& g : _gears) g.cycle.stop();
    _phase           = GearPhase::FaultSafe;
    _phaseEntered_ms = millis();
}

void GearControlEffect::pollBatteryAndApplyPolicy() {
    // Battery readback + cutoff policy is wired to PWM_QUERY against
    // the sense-enabled retract channel during the SlaveApi
    // integration step.  Scaffolded here so the update() loop already
    // has a hook in place.
}

}  // namespace hubfx::effects::gearcontrol
