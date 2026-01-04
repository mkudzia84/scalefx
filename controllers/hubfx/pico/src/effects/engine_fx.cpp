/**
 * EngineFX - Implementation
 * 
 * Engine sound effects with state machine and PWM toggle detection.
 * Uses AudioMixer for async audio playback on Core 1.
 */

#include "engine_fx.h"
#include "../audio/audio_mixer.h"

// ============================================================================
//  DEBUG LOGGING
// ============================================================================

#ifndef ENGINE_FX_DEBUG
#define ENGINE_FX_DEBUG 1
#endif

#if ENGINE_FX_DEBUG
#define LOG(fmt, ...) Serial.printf("[EngineFX] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG(fmt, ...)
#endif

// ============================================================================
//  INITIALIZATION
// ============================================================================

bool EngineFX::begin(const EngineFXSettings& settings, AudioMixer* mixer) {
    if (!settings.enabled) {
        LOG("Disabled in settings");
        return false;
    }
    
    if (!mixer) {
        LOG("Error: No AudioMixer provided");
        return false;
    }
    
    _settings = settings;
    _mixer = mixer;
    
    // Initialize state
    _state = EngineState::Stopped;
    _requestedState = EngineState::Stopped;
    _toggleEngaged = false;
    _lastToggleState = false;
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
    switch (_state) {
        case EngineState::Stopped:
            if (toggleOn || _requestedState == EngineState::Starting) {
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Starting);
                playStartupSound();
            }
            break;
            
        case EngineState::Starting:
            if (!toggleOn || _requestedState == EngineState::Stopping) {
                // Toggle turned off while starting
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Stopping);
                _mixer->stopAsync(_settings.channelStartup, AudioStopMode::Immediate);
                _startupSoundStarted = false;
                playShutdownSound(_settings.stoppingOffsetFromStartingMs);
            } else {
                // Check crossfade timing
                int remainingMs = _mixer->remainingMs(_settings.channelStartup);
                if (remainingMs >= 0 && remainingMs <= EngineFXConfig::CROSSFADE_MS) {
                    playRunningSound();
                }
                // Check if startup finished
                if (!_mixer->isPlaying(_settings.channelStartup)) {
                    enterState(EngineState::Running);
                    playRunningSound();
                }
            }
            break;
            
        case EngineState::Running:
            playRunningSound();
            if (!toggleOn || _requestedState == EngineState::Stopping) {
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Stopping);
                _mixer->stopAsync(_settings.channelRunning, AudioStopMode::Immediate);
                _runningSoundStarted = false;
                playShutdownSound();
            }
            break;
            
        case EngineState::Stopping:
            if (toggleOn || _requestedState == EngineState::Starting) {
                // Toggle turned back on while stopping
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Starting);
                _mixer->stopAsync(_settings.channelShutdown, AudioStopMode::Immediate);
                _shutdownSoundStarted = false;
                playStartupSound(_settings.startingOffsetFromStoppingMs);
            } else if (!_mixer->isPlaying(_settings.channelShutdown)) {
                // Shutdown finished
                enterState(EngineState::Stopped);
                stopAllSounds();
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
}

// ============================================================================
//  AUDIO HELPERS
// ============================================================================

void EngineFX::playStartupSound(int offsetMs) {
    if (_startupSoundStarted) return;
    if (!_settings.startupSound.filename) return;
    
    LOG("Playing startup: %s (offset=%d ms)", _settings.startupSound.filename, offsetMs);
    
    AudioPlaybackOptions opts;
    opts.loop = false;
    opts.volume = _settings.startupSound.volume;
    opts.output = AudioOutput::Stereo;
    opts.startOffsetMs = offsetMs;
    
    _mixer->playAsync(_settings.channelStartup, _settings.startupSound.filename, opts);
    _startupSoundStarted = true;
}

void EngineFX::playRunningSound() {
    if (_runningSoundStarted) return;
    if (!_settings.runningSound.filename) return;
    
    LOG("Playing running: %s", _settings.runningSound.filename);
    
    AudioPlaybackOptions opts;
    opts.loop = true;
    opts.volume = _settings.runningSound.volume;
    opts.output = AudioOutput::Stereo;
    opts.startOffsetMs = 0;
    
    _mixer->playAsync(_settings.channelRunning, _settings.runningSound.filename, opts);
    _runningSoundStarted = true;
}

void EngineFX::playShutdownSound(int offsetMs) {
    if (_shutdownSoundStarted) return;
    if (!_settings.shutdownSound.filename) return;
    
    LOG("Playing shutdown: %s (offset=%d ms)", _settings.shutdownSound.filename, offsetMs);
    
    AudioPlaybackOptions opts;
    opts.loop = false;
    opts.volume = _settings.shutdownSound.volume;
    opts.output = AudioOutput::Stereo;
    opts.startOffsetMs = offsetMs;
    
    _mixer->playAsync(_settings.channelShutdown, _settings.shutdownSound.filename, opts);
    _shutdownSoundStarted = true;
}

void EngineFX::stopAllSounds() {
    LOG("Stopping all sounds");
    
    if (_mixer) {
        _mixer->stopAsync(_settings.channelStartup, AudioStopMode::Immediate);
        _mixer->stopAsync(_settings.channelRunning, AudioStopMode::Immediate);
        _mixer->stopAsync(_settings.channelShutdown, AudioStopMode::Immediate);
    }
    
    _startupSoundStarted = false;
    _runningSoundStarted = false;
    _shutdownSoundStarted = false;
}

// ============================================================================
//  MANUAL CONTROL
// ============================================================================

void EngineFX::forceStart() {
    LOG("Force start requested");
    _requestedState = EngineState::Starting;
}

void EngineFX::forceStop() {
    LOG("Force stop requested");
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
