/*
 * Door Sequencer Module - Header
 *
 * Mode-aware door servo sequencing for landing gear.
 * Handles open/close sequences respecting door mode configuration:
 *   NONE       - No door servos (immediate completion)
 *   SINGLE     - One door servo (servo 0 only)
 *   DUAL_SYNC  - Two doors, activated simultaneously
 *   DUAL_DELAY - Two doors, door 1 starts after configurable delay
 *   DUAL_SEQ   - Two doors, door 1 starts after door 0 completes
 *
 * For DUAL_DELAY and DUAL_SEQ: doors open 0→1, close 1→0
 * (inner door opens first, closes last — mimics real aircraft).
 *
 * Operates on externally-owned ServoControl objects via pointers.
 * Does NOT handle servo motion profiling — the caller must call
 * ServoControl::update() separately.
 *
 * Usage:
 *   DoorSequencer seq;
 *   seq.begin(&servo0, &servo1);
 *   seq.setConfig(doorConfig);
 *   seq.setMode(DoorMode::DUAL_SEQ);
 *
 *   // Start opening:
 *   seq.startOpen();
 *   // In loop:
 *   seq.update();
 *   if (seq.isComplete()) { ... }
 */

#ifndef DOOR_SEQUENCER_H
#define DOOR_SEQUENCER_H

#include <Arduino.h>
#include <srv_control.h>
#include <serial_gearcontrol.h>

// ============================================================================
// Door Sequencer Configuration
// ============================================================================

namespace DoorSeqConfig {
    constexpr uint16_t DOOR_TRAVEL_TIME_ms  = 1500;  // Max wait for door servo travel
    constexpr uint16_t SETTLE_TIME_ms       = 200;   // Min time after servo reaches target
}

// ============================================================================
// DoorSequencer Class
// ============================================================================

/**
 * @brief Mode-aware door servo sequencer
 *
 * Manages open/close sequencing for 1-2 door servos with configurable
 * activation modes (synchronous, delayed, sequential).
 *
 * Operates on externally-owned ServoControl objects via pointers.
 * The caller is responsible for:
 *   1. Owning and initializing the ServoControl objects
 *   2. Calling ServoControl::update() for motion profiling each loop iteration
 *   3. Calling DoorSequencer::update() when a sequence is in progress
 */
class DoorSequencer {
public:
    DoorSequencer() = default;
    ~DoorSequencer() = default;

    // Non-copyable (holds pointers to external ServoControl objects)
    DoorSequencer(const DoorSequencer&) = delete;
    DoorSequencer& operator=(const DoorSequencer&) = delete;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize with pointers to door servo objects
     *
     * The ServoControl objects must outlive the DoorSequencer.
     * Call this after the ServoControl objects have been created
     * (they need not be attached to pins yet).
     *
     * @param door0 Pointer to first door ServoControl
     * @param door1 Pointer to second door ServoControl (may be nullptr for SINGLE mode)
     */
    void begin(ServoControl* door0, ServoControl* door1);

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Set door open/close positions
     */
    void setConfig(const GearControlDoorConfig& config);

    /**
     * @brief Set door activation mode
     * @param mode DoorMode value (NONE, SINGLE, DUAL_SYNC, DUAL_DELAY, DUAL_SEQ)
     * @param delay_ms Delay between doors in ms (only used for DUAL_DELAY mode)
     */
    void setMode(uint8_t mode, uint16_t delay_ms = 500);

    /** @brief Get current door mode */
    uint8_t mode() const { return _mode; }

    /** @brief Get current door delay (for DUAL_DELAY mode) */
    uint16_t delay_ms() const { return _delay_ms; }

    // ========================================================================
    // Sequenced Operations
    // ========================================================================

    /**
     * @brief Start mode-aware door opening sequence
     *
     * For NONE mode: immediately completes (no-op).
     * For SINGLE/DUAL_SYNC: commands all configured doors immediately.
     * For DUAL_DELAY/DUAL_SEQ: commands door 0 first (door 1 started by update()).
     */
    void startOpen();

    /**
     * @brief Start mode-aware door closing sequence
     *
     * Reverse order for DUAL_DELAY/DUAL_SEQ: door 1 closes first, then door 0.
     */
    void startClose();

    /**
     * @brief Update door sequencing state machine
     *
     * Must be called regularly when a sequence is in progress.
     * Only processes state when OPENING or CLOSING; no-op when IDLE/COMPLETE.
     */
    void update();

    /**
     * @brief Reset to idle without waiting for completion
     */
    void reset() { _state = State::IDLE; }

    // ========================================================================
    // State Queries
    // ========================================================================

    /** @brief Check if sequence has completed */
    bool isComplete() const { return _state == State::COMPLETE; }

    /** @brief Check if sequencer is idle (not running) */
    bool isIdle() const { return _state == State::IDLE; }

    /** @brief Check if a sequence is in progress */
    bool isBusy() const { return _state == State::OPENING || _state == State::CLOSING; }

    // ========================================================================
    // Direct Control (bypasses sequencing)
    // ========================================================================

    /**
     * @brief Open all configured doors immediately (no sequencing)
     *
     * Commands all doors based on current mode:
     *   NONE: no-op, SINGLE: door 0 only, DUAL_*: both doors.
     */
    void openImmediate();

    /**
     * @brief Close all configured doors immediately (no sequencing)
     */
    void closeImmediate();

    /**
     * @brief Set a specific door to an explicit position
     * @param doorIndex Door index (0 or 1)
     * @param pos_us Position in microseconds
     */
    void setPosition(uint8_t doorIndex, uint16_t pos_us);

    // ========================================================================
    // Door Servo Access
    // ========================================================================

    /** @brief Get current door servo position */
    uint16_t position_us(uint8_t doorIndex) const;

    /** @brief Get reference to a door ServoControl for configuration */
    ServoControl& servo(uint8_t doorIndex);

    /** @brief Get const reference to a door ServoControl */
    const ServoControl& servo(uint8_t doorIndex) const;

private:
    enum class State : uint8_t { IDLE, OPENING, CLOSING, COMPLETE };

    ServoControl* _doors[2] = { nullptr, nullptr };
    GearControlDoorConfig _config;
    uint8_t _mode = DoorMode::DUAL_SYNC;
    uint16_t _delay_ms = 500;

    State _state = State::IDLE;
    uint32_t _startTime_ms = 0;
    uint8_t _phase = 0;
    uint32_t _phaseStart_ms = 0;

    void _updateOpening();
    void _updateClosing();
};

#endif // DOOR_SEQUENCER_H
