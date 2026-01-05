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
#include "../audio/audio_channels.h"

// Forward declaration
class AudioMixer;

// ============================================================================
//  CONSTANTS
// ============================================================================

namespace EngineFXConfig {
    constexpr int CROSSFADE_MS       = 500;   // Crossfade start before sound ends
    constexpr int HYSTERESIS_US      = 100;   // Toggle detection hysteresis
    constexpr int PROCESS_INTERVAL_MS = 10;   // State machine update interval
    constexpr int MIN_STATE_TIME_MS  = 100;   // Minimum time in state before checking completion
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
    
    // Audio channels (two channels for crossfading)
    int channelA = AudioChannels::ENGINE_A;  // Primary channel
    int channelB = AudioChannels::ENGINE_B;  // Secondary for crossfade
    
    // Crossfade duration (ms before sound ends to start next)
    int crossfadeMs = 500;
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
    void crossfadeToRunning();  // Start running on channel B while startup fades
    void playShutdownSound(int offsetMs = 0);
    void stopAllSounds();
    
    // ---- Channel Helpers ----
    int activeChannel() const { return _useChannelB ? _settings.channelB : _settings.channelA; }
    int crossfadeChannel() const { return _useChannelB ? _settings.channelA : _settings.channelB; }
    void swapChannels() { _useChannelB = !_useChannelB; }

    // ---- State ----
    EngineFXSettings _settings;
    AudioMixer* _mixer           = nullptr;
    PwmInput _toggleInput;
    
    EngineState _state           = EngineState::Stopped;
    EngineState _requestedState  = EngineState::Stopped;
    
    bool _toggleEngaged          = false;
    bool _lastToggleState        = false;
    bool _forceRunning           = false;  // Keep running via CLI command
    
    uint32_t _stateStartMs       = 0;
    uint32_t _lastProcessMs      = 0;
    
    bool _startupSoundStarted    = false;
    bool _runningSoundStarted    = false;
    bool _shutdownSoundStarted   = false;
    bool _crossfadeStarted       = false;
    
    bool _useChannelB            = false;  // Track which channel is active
    
    bool _initialized            = false;
};

#endif // ENGINE_FX_H
