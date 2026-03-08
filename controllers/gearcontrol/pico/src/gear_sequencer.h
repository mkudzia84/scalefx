/*
 * Gear Sequencer Module - Header
 *
 * Op-queue based deploy/retract sequencing with preemption support.
 *
 * Manages the lifecycle: open doors → run motor → close doors.
 * Each step is represented as a GearOp queued in order. The sequencer
 * executes ops sequentially, polling the relevant subsystem (DoorSequencer
 * for door ops, StallDetector for motor ops) until completion.
 *
 * Preemption:
 *   When a conflicting command arrives (deploy during retract or vice versa),
 *   the queue is rebuilt for the new direction. The current operation is
 *   handled safely: motor is stopped, doors are reversed if mid-close,
 *   and already-opening doors are reused without restarting.
 *
 * Sync mode:
 *   In coordinated (GEAR_ALL) operations, SYNC_BARRIER ops are inserted
 *   between phases. The sequencer pauses at each barrier until an external
 *   coordinator calls advanceSyncPhase().
 *
 * Dependencies (injected via begin()):
 *   - DoorSequencer: door open/close state machine (pointer)
 *   - StallDetector: motor stall/timeout detection (pointer)
 *   - MotorFn: motor direction control callback
 *   - CurrentFn: motor current reading callback
 *
 * Notifications:
 *   A phase callback fires on each op transition and on sequence
 *   completion (success or error). The owner (LandingGear) uses this
 *   to update its own state and emit wire-format progress packets.
 */

#ifndef GEAR_SEQUENCER_H
#define GEAR_SEQUENCER_H

#include <Arduino.h>
#include <functional>
#include "door_sequencer.h"
#include "stall_detector.h"

// ============================================================================
// Gear Sequencing Step (derived from queue position)
// ============================================================================

enum class GearSeqStep : uint8_t {
    IDLE = 0,
    OPENING_DOORS,
    SYNC_DOORS_OPEN,   // Doors finished, waiting for other gears before motor
    RUNNING_MOTOR,
    SYNC_MOTOR_DONE,   // Motor finished, waiting for other gears before closing
    CLOSING_DOORS,
    ERROR
};

// ============================================================================
// Gear Operation Queue Entry
// ============================================================================

/**
 * @brief Single operation in a gear deploy/retract sequence
 *
 * Operations are queued in order and executed sequentially.
 * When a conflicting command arrives (deploy during retract or vice versa),
 * the queue is preempted: the current operation is handled safely and a
 * new queue is built for the reversed direction.
 */
struct GearOp {
    enum Type : uint8_t {
        OPEN_DOORS,     // Open doors via DoorSequencer
        RUN_MOTOR,      // Run motor until stall/timeout
        CLOSE_DOORS,    // Close doors via DoorSequencer
        SYNC_BARRIER    // Wait for coordinator (sync mode only)
    };
    Type type;
    bool deploying;     // Direction context: affects door mode and motor direction
};

// ============================================================================
// GearSequencer Class
// ============================================================================

class GearSequencer {
public:
    static constexpr uint8_t MAX_OPS = 5;  // OPEN + SYNC + MOTOR + SYNC + CLOSE

    // Callbacks for hardware interaction (injected by LandingGear)
    using MotorFn    = std::function<void(int8_t direction)>;   // 1=CW, -1=CCW, 0=stop
    using CurrentFn  = std::function<uint16_t()>;               // Read motor current (mA)

    /**
     * @brief Phase change callback
     * @param finished true if the sequence has ended (success or error)
     * @param error    true if the sequence ended due to an error (TIMEOUT_ERROR)
     *
     * When finished==false: a new op has started (phase transition).
     * When finished==true && error==false: queue exhausted, gear reached terminal state.
     * When finished==true && error==true: timeout/error, motor stopped, queue cleared.
     */
    using PhaseCallback = std::function<void(bool finished, bool error)>;

    /**
     * @brief Stall detector configuration for a sequence
     *
     * Provided by LandingGear at sequence start because the effective
     * stall threshold may change based on calibration and drag headroom.
     */
    struct StallConfig {
        uint16_t startupIgnore_ms;
        uint16_t motorDetectThreshold_mA;
        uint16_t stallThreshold_mA;
        uint16_t stallConfirm_ms;
        uint32_t timeout_ms;
    };

    GearSequencer() = default;
    ~GearSequencer() = default;

    // Non-copyable (contains std::function members)
    GearSequencer(const GearSequencer&) = delete;
    GearSequencer& operator=(const GearSequencer&) = delete;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize the sequencer with its dependencies
     * @param doorSeq      Pointer to the DoorSequencer (owned by LandingGear)
     * @param stallDetector Pointer to the StallDetector (owned by LandingGear)
     * @param motorFn      Callback to set motor direction (1=CW, -1=CCW, 0=stop)
     * @param currentFn    Callback to read motor current in mA
     */
    void begin(DoorSequencer* doorSeq, StallDetector* stallDetector,
               MotorFn motorFn, CurrentFn currentFn);

    // ========================================================================
    // Sequence Control
    // ========================================================================

    /**
     * @brief Build and start a deploy/retract op queue, preempting if necessary
     *
     * If a sequence is already active in the opposite direction, the current
     * operation is handled safely (motor stopped, doors reversed if needed)
     * and a new queue is built for the target direction.
     *
     * @param deploying  true for deploy, false for retract
     * @param syncMode   true for coordinated operation (inserts SYNC_BARRIERs)
     * @param cfg        Stall detector configuration for the motor phase
     */
    void start(bool deploying, bool syncMode, const StallConfig& cfg);

    /**
     * @brief Poll the current op for completion
     *
     * Must be called regularly in the main loop. Each op type has
     * specific completion logic (door complete, stall detected, etc.).
     */
    void update();

    /**
     * @brief Advance past a sync barrier (called by coordinator)
     *
     * The current queue op must be SYNC_BARRIER. Advances to the next op
     * and begins it (motor start or door close).
     */
    void advanceSyncPhase();

    /**
     * @brief Emergency stop — stops motor and clears the queue
     *
     * Does NOT fire the phase callback (the caller handles state transitions).
     * Use for stop(), shutdown(), and reset() in LandingGear.
     */
    void stop();

    // ========================================================================
    // Callbacks
    // ========================================================================

    /** @brief Register callback for phase transitions and completion */
    void onPhaseChange(PhaseCallback cb) { _phaseCb = cb; }

    // ========================================================================
    // State Queries
    // ========================================================================

    /** @brief Check if a sequence is currently active */
    bool isActive() const { return _current < _count; }

    /** @brief Get the current step (derived from queue position) */
    GearSeqStep step() const;

    /** @brief Get the target direction of the current/last sequence */
    bool deploying() const { return _deploying; }

    /** @brief Check if the sequence is in sync mode */
    bool syncMode() const { return _syncMode; }

    /** @brief Check if the motor is currently running */
    bool motorRunning() const { return _motorRunning; }

    /** @brief Get elapsed time since sequence started */
    uint32_t elapsed_ms() const { return millis() - _sequenceStartTime_ms; }

    // Sync mode queries
    bool isWaitingSyncDoorsOpen() const { return step() == GearSeqStep::SYNC_DOORS_OPEN; }
    bool isWaitingSyncMotorDone() const { return step() == GearSeqStep::SYNC_MOTOR_DONE; }
    bool isSyncOpeningDoors() const { return _syncMode && step() == GearSeqStep::OPENING_DOORS; }
    bool isSyncRunningMotor() const { return _syncMode && step() == GearSeqStep::RUNNING_MOTOR; }

private:
    // Op queue
    GearOp _ops[MAX_OPS];
    uint8_t _count = 0;
    uint8_t _current = 0;

    // Sequence metadata
    bool _deploying = false;
    bool _syncMode = false;
    bool _motorRunning = false;
    uint32_t _stepStartTime_ms = 0;
    uint32_t _sequenceStartTime_ms = 0;

    // Stall config for the current sequence
    StallConfig _stallConfig;

    // Dependencies (not owned)
    DoorSequencer* _doorSeq = nullptr;
    StallDetector* _stallDetector = nullptr;

    // Callbacks
    MotorFn _motorFn;
    CurrentFn _currentFn;
    PhaseCallback _phaseCb;

    // Queue management
    void _enqueue(GearOp::Type type, bool dep);
    void _clear();

    // Op execution
    void _beginCurrentOp();
    void _advanceToNextOp();
    void _finish();
    void _error();
    void _emitPhase(bool finished = false, bool error = false);
};

#endif // GEAR_SEQUENCER_H
