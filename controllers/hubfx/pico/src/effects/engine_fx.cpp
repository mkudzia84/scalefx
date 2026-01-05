/**
 * EngineFX - Implementation
 * 
 * Engine sound effects with state machine and PWM toggle detection.
 * Uses AudioMixer for async audio playback on Core 1.
 */

#include "engine_fx.h"
#include "effects_config.h"
#include "../audio/audio_mixer.h"

// Debug logging using centralized config
#define LOG(fmt, ...) EFFECTS_LOG("EngineFX", fmt, ##__VA_ARGS__)

// ============================================================================
//  INITIALIZATION
// ============================================================================

bool EngineFX::begin(const EngineFXSettings& settings) {
    if (!settings.enabled) {
        LOG("Disabled in settings");
        return false;
    }
    
    _settings = settings;
    
    // Initialize state
    _state = EngineState::Stopped;
    _requestedState = EngineState::Stopped;
    _toggleEngaged = false;
    _lastToggleState = false;
    _forceRunning = false;
    _stateStartMs = millis();
    _lastProcessMs = millis();
    
    // Initialize toggle input
    _toggleInput.begin(settings.toggleInputType, settings.togglePin);
    
    // For serial mode, start with toggle off
    if (settings.toggleInputType == PwmInputType::Serial) {
        _toggleInput.setValue(settings.toggleThresholdUs - 200);
    }
    
    _initialized = true;
    
    LOG("Initialized - pin=%d, threshold=%d us", 
        settings.togglePin, settings.toggleThresholdUs);
    
    return true;
}

void EngineFX::end() {
    if (!_initialized) return;
    
    stopAllSounds();
    _toggleInput.end();
    _initialized = false;
    
    LOG("Ended");
}

// ============================================================================
//  PROCESSING
// ============================================================================

void EngineFX::process() {
    if (!_initialized) return;
    
    // Rate limit to process interval
    uint32_t now = millis();
    if (now - _lastProcessMs < EngineFXConfig::PROCESS_INTERVAL_MS) return;
    _lastProcessMs = now;
    
    // Update toggle input
    _toggleInput.update();
    
    // Check toggle state with hysteresis
    bool toggleOn = _toggleInput.aboveThreshold(
        _settings.toggleThresholdUs, 
        EngineFXConfig::HYSTERESIS_US
    );
    _toggleEngaged = toggleOn;
    
    // Log toggle state changes
    if (toggleOn != _lastToggleState) {
        LOG("Toggle %s (PWM=%d us)", toggleOn ? "ON" : "OFF", _toggleInput.average());
        _lastToggleState = toggleOn;
    }
    
    // Process state machine
    // Engine should run if toggle is ON or force running is set
    bool shouldRun = toggleOn || _forceRunning;
    
    switch (_state) {
        case EngineState::Stopped:
            if (shouldRun || _requestedState == EngineState::Starting) {
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Starting);
                playStartupSound();
            }
            break;
            
        case EngineState::Starting:
            if ((!shouldRun && _requestedState != EngineState::Starting) || 
                _requestedState == EngineState::Stopping) {
                // Toggle turned off while starting
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Stopping);
                // Stop both channels
                mixer().stopAsync(_settings.channelA, AudioStopMode::Immediate);
                mixer().stopAsync(_settings.channelB, AudioStopMode::Immediate);
                _startupSoundStarted = false;
                _runningSoundStarted = false;
                _crossfadeStarted = false;
                playShutdownSound(_settings.stoppingOffsetFromStartingMs);
            } else {
                // Check crossfade timing - start running on other channel near end of startup
                int remainingMs = mixer().remainingMs(activeChannel());
                if (!_crossfadeStarted && remainingMs >= 0 && remainingMs <= _settings.crossfadeMs) {
                    crossfadeToRunning();
                }
                // Check if startup finished (only check after min time in state)
                uint32_t timeInState = millis() - _stateStartMs;
                if (timeInState >= EngineFXConfig::MIN_STATE_TIME_MS &&
                    _startupSoundStarted && !mixer().isPlaying(activeChannel())) {
                    // Startup channel finished, running is on crossfade channel
                    swapChannels();  // Running is now the active channel
                    enterState(EngineState::Running);
                    if (!_runningSoundStarted) {
                        playRunningSound();
                    }
                }
            }
            break;
            
        case EngineState::Running:
            // Ensure running sound keeps playing (looped)
            if (!mixer().isPlaying(activeChannel())) {
                playRunningSound();
            }
            if (!shouldRun || _requestedState == EngineState::Stopping) {
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Stopping);
                // Fade out running and play shutdown on other channel
                mixer().stopAsync(activeChannel(), AudioStopMode::Fade);
                _runningSoundStarted = false;
                playShutdownSound();
            }
            break;
            
        case EngineState::Stopping:
            if (shouldRun || _requestedState == EngineState::Starting) {
                // Toggle turned back on while stopping
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Starting);
                mixer().stopAsync(_settings.channelA, AudioStopMode::Immediate);
                mixer().stopAsync(_settings.channelB, AudioStopMode::Immediate);
                _shutdownSoundStarted = false;
                playStartupSound(_settings.startingOffsetFromStoppingMs);
            } else {
                // Allow time for async sound to start before checking completion
                uint32_t timeInState = millis() - _stateStartMs;
                if (timeInState >= EngineFXConfig::MIN_STATE_TIME_MS &&
                    _shutdownSoundStarted && 
                    !mixer().isPlaying(_settings.channelA) && 
                    !mixer().isPlaying(_settings.channelB)) {
                    // Shutdown finished
                    enterState(EngineState::Stopped);
                    stopAllSounds();
                }
            }
            break;
    }
}

// ============================================================================
//  STATE MACHINE
// ============================================================================

void EngineFX::enterState(EngineState newState) {
    if (_state == newState) return;
    
    LOG("State: %s -> %s", stateString(_state), stateString(newState));
    
    _state = newState;
    _stateStartMs = millis();
    
    // Reset sound tracking
    _startupSoundStarted = false;
    _runningSoundStarted = false;
    _shutdownSoundStarted = false;
    _crossfadeStarted = false;
}

// ============================================================================
//  AUDIO HELPERS
// ============================================================================

void EngineFX::playStartupSound(int offsetMs) {
    if (_startupSoundStarted) return;
    if (!_settings.startupSound.filename) return;
    
    LOG("Playing startup on ch%d: %s (offset=%d ms)", activeChannel(), _settings.startupSound.filename, offsetMs);
    
    AudioPlaybackOptions opts;
    opts.loop = false;
    opts.loopCount = 0;
    opts.volume = _settings.startupSound.volume;
    opts.output = AudioOutput::Stereo;
    opts.startOffsetMs = offsetMs;
    
    mixer().playAsync(activeChannel(), _settings.startupSound.filename, opts);
    _startupSoundStarted = true;
}

void EngineFX::playRunningSound() {
    if (_runningSoundStarted) return;
    if (!_settings.runningSound.filename) return;
    
    LOG("Playing running on ch%d: %s (looped)", activeChannel(), _settings.runningSound.filename);
    
    AudioPlaybackOptions opts;
    opts.loop = true;
    opts.loopCount = LOOP_INFINITE;
    opts.volume = _settings.runningSound.volume;
    opts.output = AudioOutput::Stereo;
    opts.startOffsetMs = 0;
    
    mixer().playAsync(activeChannel(), _settings.runningSound.filename, opts);
    _runningSoundStarted = true;
}

void EngineFX::crossfadeToRunning() {
    if (_crossfadeStarted) return;
    if (!_settings.runningSound.filename) return;
    
    LOG("Crossfading to running on ch%d: %s", crossfadeChannel(), _settings.runningSound.filename);
    
    AudioPlaybackOptions opts;
    opts.loop = true;
    opts.loopCount = LOOP_INFINITE;
    opts.volume = _settings.runningSound.volume;
    opts.output = AudioOutput::Stereo;
    opts.startOffsetMs = 0;
    
    // Start running sound on the other channel (crossfade channel)
    mixer().playAsync(crossfadeChannel(), _settings.runningSound.filename, opts);
    _crossfadeStarted = true;
    _runningSoundStarted = true;  // Mark as started since it's playing on crossfade channel
}

void EngineFX::playShutdownSound(int offsetMs) {
    if (_shutdownSoundStarted) return;
    if (!_settings.shutdownSound.filename) return;
    
    // Play shutdown on the crossfade channel (other than current)
    int shutdownCh = crossfadeChannel();
    LOG("Playing shutdown on ch%d: %s (offset=%d ms)", shutdownCh, _settings.shutdownSound.filename, offsetMs);
    
    AudioPlaybackOptions opts;
    opts.loop = false;
    opts.loopCount = 0;
    opts.volume = _settings.shutdownSound.volume;
    opts.output = AudioOutput::Stereo;
    opts.startOffsetMs = offsetMs;
    
    mixer().playAsync(shutdownCh, _settings.shutdownSound.filename, opts);
    _shutdownSoundStarted = true;
    swapChannels();  // Shutdown is now the active channel
}

void EngineFX::stopAllSounds() {
    LOG("Stopping all sounds");
    
    mixer().stopAsync(_settings.channelA, AudioStopMode::Immediate);
    mixer().stopAsync(_settings.channelB, AudioStopMode::Immediate);
    
    _startupSoundStarted = false;
    _runningSoundStarted = false;
    _shutdownSoundStarted = false;
    _crossfadeStarted = false;
}

// ============================================================================
//  MANUAL CONTROL
// ============================================================================

void EngineFX::forceStart() {
    LOG("Force start requested");
    _forceRunning = true;
    _requestedState = EngineState::Starting;
}

void EngineFX::forceStop() {
    LOG("Force stop requested");
    _forceRunning = false;
    _requestedState = EngineState::Stopping;
}

void EngineFX::setToggleValue(int valueUs) {
    _toggleInput.setValue(valueUs);
}

// ============================================================================
//  UTILITIES
// ============================================================================

const char* EngineFX::stateString(EngineState state) {
    switch (state) {
        case EngineState::Stopped:  return "STOPPED";
        case EngineState::Starting: return "STARTING";
        case EngineState::Running:  return "RUNNING";
        case EngineState::Stopping: return "STOPPING";
        default:                    return "UNKNOWN";
    }
}
