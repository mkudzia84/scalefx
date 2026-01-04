/**
 * EngineFX - Engine Sound Effects Module
 * 
 * Provides engine startup/running/shutdown sound effects with state machine.
 * Monitors toggle input (PWM/analog/serial) to trigger state transitions.
 * 
 * State Machine:
 *   STOPPED <--> STARTING --> RUNNING --> STOPPING --> STOPPED
 * 
 * Features:
 * - PWM, analog, or serial input monitoring with hysteresis
 * - Crossfade between startup and running sounds
 * - Configurable transition offsets for seamless audio
 * - Async audio playback via AudioMixer (runs on Core 1)
 */

#ifndef ENGINE_FX_H
#define ENGINE_FX_H

#include <Arduino.h>
#include <pwm_control.h>

// Forward declaration
class AudioMixer;

// ============================================================================
//  CONSTANTS
// ============================================================================

namespace EngineFXConfig {
    constexpr int CROSSFADE_MS       = 500;   // Crossfade start before sound ends
    constexpr int HYSTERESIS_US      = 100;   // Toggle detection hysteresis
    constexpr int PROCESS_INTERVAL_MS = 10;   // State machine update interval
}

// ============================================================================
//  TYPES
// ============================================================================

enum class EngineState : uint8_t {
    Stopped = 0,
    Starting,
    Running,
    Stopping
};

struct EngineSoundConfig {
    const char* filename = nullptr;
    float volume         = 1.0f;
};

struct EngineFXSettings {
    bool enabled = false;
    
    // Toggle input configuration
    int togglePin                    = -1;
    PwmInputType toggleInputType     = PwmInputType::None;
    int toggleThresholdUs            = 1500;
    
    // Sound files
    EngineSoundConfig startupSound;
    EngineSoundConfig runningSound;
    EngineSoundConfig shutdownSound;
    
    // Transition timing (ms offset when reversing direction)
    int startingOffsetFromStoppingMs = 0;
    int stoppingOffsetFromStartingMs = 0;
    
    // Audio channels
    int channelStartup  = 0;
    int channelRunning  = 1;
    int channelShutdown = 2;
};

// ============================================================================
//  ENGINE FX CLASS
// ============================================================================

class EngineFX {
public:
    EngineFX() = default;
    ~EngineFX() = default;
    
    // Non-copyable
    EngineFX(const EngineFX&) = delete;
    EngineFX& operator=(const EngineFX&) = delete;

    // ---- Initialization ----
    bool begin(const EngineFXSettings& settings, AudioMixer* mixer);
    void end();

    // ---- Processing ----
    void process();

    // ---- State ----
    EngineState state() const { return _state; }
    bool isActive() const { return _state != EngineState::Stopped; }
    bool isToggleEngaged() const { return _toggleEngaged; }
    
    // ---- Manual Control ----
    void forceStart();
    void forceStop();
    
    // ---- Serial Input (for PwmInputType::Serial) ----
    void setToggleValue(int valueUs);

    // ---- Utilities ----
    static const char* stateString(EngineState state);

private:
    // ---- State Machine ----
    void enterState(EngineState newState);

    // ---- Audio Helpers ----
    void playStartupSound(int offsetMs = 0);
    void playRunningSound();
    void playShutdownSound(int offsetMs = 0);
    void stopAllSounds();

    // ---- State ----
    EngineFXSettings _settings;
    AudioMixer* _mixer           = nullptr;
    PwmInput _toggleInput;
    
    EngineState _state           = EngineState::Stopped;
    EngineState _requestedState  = EngineState::Stopped;
    
    bool _toggleEngaged          = false;
    bool _lastToggleState        = false;
    
    uint32_t _stateStartMs       = 0;
    uint32_t _lastProcessMs      = 0;
    
    bool _startupSoundStarted    = false;
    bool _runningSoundStarted    = false;
    bool _shutdownSoundStarted   = false;
    
    bool _initialized            = false;
};

#endif // ENGINE_FX_H
