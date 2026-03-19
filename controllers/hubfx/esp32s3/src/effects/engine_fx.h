/*
 * EngineFX — Engine Sound Effects State Machine (Singleton)
 *
 * Manages engine startup/running/shutdown sound effects with a 4-state
 * state machine.  Monitors a toggle input (typically a PWM channel) to
 * drive automatic transitions, and exposes manual force-start / force-stop
 * for CLI / protocol control.
 *
 * State Machine:
 *   STOPPED ←→ STARTING → RUNNING → STOPPING → STOPPED
 *
 *   Stopped → Starting:  Toggle ON or forceStart().
 *                         Plays startup sound on activeChannel().
 *   Starting → Running:  Startup sound confirmed playing, then finishes.
 *                         Crossfade to running loop on crossfadeChannel()
 *                         starts CROSSFADE_SEC before startup ends.
 *                         Swap channels so running loop becomes active.
 *   Running → Stopping:  Toggle OFF or forceStop().
 *                         Fade out running loop, play shutdown sound.
 *   Stopping → Stopped:  Shutdown sound confirmed playing, then finishes.
 *
 *   Reversals: Starting↔Stopping transitions supported mid-flight
 *   (e.g., toggle OFF during startup → immediate switch to Stopping).
 *
 * Async Gap Protection:
 *   playAsync() is non-blocking — the mixer queues the command and the
 *   producer task processes it later (~100-200ms). During this window,
 *   isPlaying() returns false. "Confirmed" flags ensure the state machine
 *   waits for actual playback before checking crossfade timing or
 *   completion. A safety timeout forces advancement if the sound file
 *   is missing or fails to load.
 *
 * Audio is driven through the AudioMixer async API (thread-safe command
 * queue — protocol / state-machine on Core 0, I2S output on Core 1).
 * Two mixer channels (ENGINE_A, ENGINE_B from HubFxChannel) are used
 * for crossfading between startup and running sounds.
 *
 * Configuration comes from EngineConfig (via config.yaml / ConfigStore).
 * Call applyConfig() to update settings at runtime (e.g., on config reload).
 * Sound paths default to empty — all paths must be configured in config.yaml.
 * Missing/empty paths are handled gracefully (state transitions skip the
 * corresponding audio without error).
 *
 * Singleton: EngineFX is a board-unique resource — one engine sound
 * per hub.  Access via EngineFX::instance().
 *
 * All logging uses DiagLog via SFX_LOG_* macros.
 */

#ifndef ENGINE_FX_H
#define ENGINE_FX_H

#include <cstdint>
#include <platform/diag_log.h>
#include "../config/engine_config.h"
#include "../hubfx_audio.h"

// ============================================================================
// Constants
// ============================================================================

namespace EngineFXConst {
    constexpr float    CROSSFADE_SEC             = 0.5f;   // Pre-end crossfade (seconds)
    constexpr uint32_t HYSTERESIS_US             = 100;    // Toggle detection hysteresis
    constexpr uint32_t PROCESS_INTERVAL_MS       = 10;     // State machine poll rate
    constexpr uint32_t MIN_STATE_TIME_MS         = 100;    // Min time before completion check
    constexpr uint32_t SOUND_CONFIRM_TIMEOUT_MS  = 2000;   // Max wait for async play to start
}

// ============================================================================
// State
// ============================================================================

enum class EngineState : uint8_t {
    Stopped  = 0,
    Starting = 1,
    Running  = 2,
    Stopping = 3
};

inline const char* engineStateString(EngineState s) {
    switch (s) {
        case EngineState::Stopped:  return "STOPPED";
        case EngineState::Starting: return "STARTING";
        case EngineState::Running:  return "RUNNING";
        case EngineState::Stopping: return "STOPPING";
        default:                    return "UNKNOWN";
    }
}

// ============================================================================
// EngineFX Class
// ============================================================================

class EngineFX {
public:
    /// Singleton accessor
    static EngineFX& instance() {
        static EngineFX inst;
        return inst;
    }

    // Delete copy/move
    EngineFX(const EngineFX&) = delete;
    EngineFX& operator=(const EngineFX&) = delete;
    EngineFX(EngineFX&&) = delete;
    EngineFX& operator=(EngineFX&&) = delete;

    ~EngineFX() { end(); }

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Initialize with engine configuration.
     *
     * Copies relevant fields from EngineConfig.  Does NOT start the engine.
     * Call process() from the main loop to drive the state machine.
     *
     * @param cfg EngineConfig from ConfigStore
     * @return true if enabled and initialized
     */
    bool begin(const EngineConfig& cfg);

    /** @brief Shut down — stop all sounds, reset state. */
    void end();

    /**
     * @brief Re-apply configuration (e.g. after config reload).
     *
     * If the engine was running with the old config it will be stopped
     * first (gracefully) and the new settings applied.
     *
     * @param cfg New EngineConfig from ConfigStore
     */
    void applyConfig(const EngineConfig& cfg);

    // ========================================================================
    // Processing
    // ========================================================================

    /** @brief Tick the state machine.  Call from loop(), rate-limited internally. */
    void process();

    // ========================================================================
    // State Queries
    // ========================================================================

    EngineState state() const { return _state; }
    bool isActive()        const { return _state != EngineState::Stopped; }
    bool isEnabled()       const { return _enabled; }
    bool isInitialized()   const { return _initialized; }
    bool isToggleEngaged() const { return _toggleEngaged; }

    // ========================================================================
    // Manual Control (protocol / CLI)
    // ========================================================================

    /** @brief Force engine start (overrides toggle). */
    void forceStart();

    /** @brief Force engine stop (overrides toggle). */
    void forceStop();

    /**
     * @brief Feed a toggle input value (µs PWM pulse width).
     *
     * Used when toggle is delivered via serial command rather than a
     * physical PWM pin.  The value is compared against threshold_us
     * with hysteresis.
     */
    void setToggleValue(uint16_t value_us);

private:
    // ---- State machine helpers ----
    void enterState(EngineState newState);

    // ---- Audio helpers ----
    AudioPlaybackOptions makeOpts(bool loop, int offset_ms = 0) const;
    void playStartupSound(uint32_t offset_ms = 0);
    void playRunningSound();
    void crossfadeToRunning();
    void playShutdownSound(uint32_t offset_ms = 0);
    void stopAllSounds();

    EngineFX() = default;  // Private constructor (singleton)

    /// Get concrete mixer singleton
    Mixer& mixer() { return Mixer::instance(); }

    // ---- Channel helpers (A/B crossfade pair) ----
    int activeChannel()    const { return _useChannelB ? _channelB : _channelA; }
    int crossfadeChannel() const { return _useChannelB ? _channelA : _channelB; }
    void swapChannels() { _useChannelB = !_useChannelB; }

    // ---- Configuration (copied from EngineConfig) ----
    bool     _enabled = false;
    int      _channelA = HubFxChannel::ENGINE_A;
    int      _channelB = HubFxChannel::ENGINE_B;
    char     _startingPath[64] = {};
    char     _runningPath[64]  = {};
    char     _stoppingPath[64] = {};
    uint32_t _startingOffset_ms = 0;
    uint32_t _stoppingOffset_ms = 0;
    float    _crossfade_sec      = EngineFXConst::CROSSFADE_SEC;
    uint16_t _threshold_us      = 1500;

    // ---- Runtime state ----
    EngineState _state           = EngineState::Stopped;
    EngineState _requestedState  = EngineState::Stopped;

    bool     _toggleEngaged      = false;
    bool     _lastToggleState    = false;
    bool     _forceRunning       = false;   // Keep running via CLI/protocol

    uint16_t _toggleValue_us     = 0;       // Most recent toggle input (µs)

    uint32_t _stateStart_ms      = 0;
    uint32_t _lastProcess_ms     = 0;

    bool     _startupSoundStarted   = false;
    bool     _startupSoundConfirmed = false;   // isPlaying() returned true at least once
    bool     _runningSoundStarted   = false;
    bool     _shutdownSoundStarted  = false;
    bool     _shutdownSoundConfirmed = false;  // isPlaying() returned true at least once
    bool     _crossfadeStarted      = false;

    bool     _useChannelB = false;          // Active-channel flip-flop

    bool     _initialized = false;
};

#endif // ENGINE_FX_H
