/*
 * EngineFX — Implementation
 *
 * Engine sound effects with state machine and dual-channel crossfade.
 * Uses AudioMixer async API for thread-safe audio control (Core 0 → Core 1).
 *
 * State Machine:
 *   STOPPED ←→ STARTING → RUNNING → STOPPING → STOPPED
 *
 * Dual-Channel Crossfade (A/B):
 *   Two mixer channels (ENGINE_A, ENGINE_B) form a flip-flop pair.
 *   activeChannel() plays the current sound; crossfadeChannel() is the standby.
 *   swapChannels() toggles which is active after a crossfade completes.
 *
 * Async Gap Protection:
 *   playAsync() enqueues commands to the mixer's command queue, but the WAV
 *   file doesn't start playing until the producer task dequeues + pre-fills
 *   the buffer (~100-200ms later). During this gap, isPlaying() returns false
 *   and remainingSec() returns -1. Without protection, the state machine would
 *   misinterpret "not yet started" as "finished playing" and skip states.
 *
 *   Solution: "confirmed" flags track when isPlaying() first returns true.
 *   Crossfade timing and completion checks are gated on confirmation.
 *   A safety timeout (SOUND_CONFIRM_TIMEOUT_MS) forces advancement if the
 *   sound fails to start (e.g., missing file).
 *
 * All logging goes through DiagLog (SFX_LOG_*).
 */

#include "engine_fx.h"
#include <audio/audio_log.h>
#include <platform/sfx_platform.h>

#define ENGINE_LOG(fmt, ...) SFX_LOG_DEBUG("[EngineFX] " fmt, ##__VA_ARGS__)

// ============================================================================
// Lifecycle
// ============================================================================

bool EngineFX::begin(const EngineConfig& cfg) {
    _enabled = cfg.enabled;
    if (!_enabled) {
        SFX_LOG_INFO("[EngineFX] Disabled in config");
        _initialized = false;
        return false;
    }

    // Copy config fields to internal state
    strncpy(_startingPath, cfg.sounds.starting, sizeof(_startingPath) - 1);
    strncpy(_runningPath,  cfg.sounds.running,  sizeof(_runningPath) - 1);
    strncpy(_stoppingPath, cfg.sounds.stopping, sizeof(_stoppingPath) - 1);
    _startingOffset_ms = cfg.sounds.transitions.startingOffset_ms;
    _stoppingOffset_ms = cfg.sounds.transitions.stoppingOffset_ms;
    _threshold_us      = cfg.toggle.threshold_us;
    _outputChannels    = cfg.outputChannels;
    _crossfade_sec      = EngineFXConst::CROSSFADE_SEC;

    // Reset all runtime state
    _state           = EngineState::Stopped;
    _requestedState  = EngineState::Stopped;
    _toggleEngaged   = false;
    _lastToggleState = false;
    _forceRunning    = false;
    _toggleValue_us  = 0;

    _stateStart_ms   = millis();
    _lastProcess_ms  = millis();

    _startupSoundStarted  = false;
    _startupSoundConfirmed = false;
    _runningSoundStarted  = false;
    _shutdownSoundStarted = false;
    _shutdownSoundConfirmed = false;
    _crossfadeStarted     = false;
    _useChannelB          = false;

    _initialized = true;

    SFX_LOG_INFO("[EngineFX] Initialized \u2014 threshold=%u us, ch=%d/%d, output=%s",
                 _threshold_us, _channelA, _channelB,
                 outputChannelsString(_outputChannels));
    SFX_LOG_INFO("[EngineFX] Sounds: start=%s run=%s stop=%s",
                 _startingPath, _runningPath, _stoppingPath);

    return true;
}

void EngineFX::end() {
    if (!_initialized) return;

    stopAllSounds();
    _initialized = false;
    _state = EngineState::Stopped;
    SFX_LOG_INFO("[EngineFX] Ended");
}

void EngineFX::applyConfig(const EngineConfig& cfg) {
    if (_initialized && isActive()) {
        SFX_LOG_INFO("[EngineFX] Config reload — stopping engine for re-init");
        forceStop();
        // Let process() drain once to begin the Stopping transition
    }

    end();
    begin(cfg);
}

// ============================================================================
// Processing — State Machine
// ============================================================================

void EngineFX::process() {
    if (!_initialized) return;

    // Rate-limit to process interval
    uint32_t now = millis();
    if (now - _lastProcess_ms < EngineFXConst::PROCESS_INTERVAL_MS) return;
    _lastProcess_ms = now;

    // ---- Toggle detection with hysteresis ----
    bool toggleOn;
    if (_toggleValue_us > 0) {
        uint16_t hi = _threshold_us + (uint16_t)EngineFXConst::HYSTERESIS_US;
        uint16_t lo = _threshold_us - (uint16_t)EngineFXConst::HYSTERESIS_US;
        toggleOn = _toggleEngaged
            ? (_toggleValue_us >= lo)
            : (_toggleValue_us >= hi);
    } else {
        toggleOn = false;
    }
    _toggleEngaged = toggleOn;

    if (toggleOn != _lastToggleState) {
        SFX_LOG_DEBUG("[EngineFX] Toggle %s (value=%u us)",
                      toggleOn ? "ON" : "OFF", _toggleValue_us);
        _lastToggleState = toggleOn;
    }

    // Engine should run if toggle is ON or force-running is set
    bool shouldRun = toggleOn || _forceRunning;

    auto& mx = mixer();

    switch (_state) {
        // ----------------------------------------------------------------
        case EngineState::Stopped:
            if (shouldRun || _requestedState == EngineState::Starting) {
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Starting);
                playStartupSound();
            }
            break;

        // ----------------------------------------------------------------
        case EngineState::Starting:
            if ((!shouldRun && _requestedState != EngineState::Starting) ||
                _requestedState == EngineState::Stopping) {
                // Toggle turned off while starting → reverse to Stopping
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Stopping);
                mx.stopAsync(_channelA, AudioStopMode::Immediate);
                mx.stopAsync(_channelB, AudioStopMode::Immediate);
                _startupSoundStarted = false;
                _runningSoundStarted = false;
                _crossfadeStarted    = false;
                playShutdownSound(_stoppingOffset_ms);
            } else {
                // Async gap protection: confirm startup sound has begun playing
                if (_startupSoundStarted && !_startupSoundConfirmed) {
                    if (mx.isPlaying(activeChannel())) {
                        _startupSoundConfirmed = true;
                        ENGINE_LOG("Startup sound confirmed playing on ch%d", activeChannel());
                    } else if (now - _stateStart_ms >= EngineFXConst::SOUND_CONFIRM_TIMEOUT_MS) {
                        SFX_LOG_WARN("[EngineFX] Startup sound not playing after %lu ms — forcing advance",
                                     (unsigned long)EngineFXConst::SOUND_CONFIRM_TIMEOUT_MS);
                        _startupSoundConfirmed = true;
                    }
                }

                // Check crossfade timing (only after confirmed playing)
                if (_startupSoundConfirmed) {
                    float remaining = mx.remainingSec(activeChannel());
                    if (!_crossfadeStarted && remaining >= 0.0f &&
                        remaining <= _crossfade_sec) {
                        crossfadeToRunning();
                    }
                }

                // Check if startup finished
                uint32_t inState = now - _stateStart_ms;
                bool canComplete = _startupSoundConfirmed || !_startupSoundStarted;
                if (inState >= EngineFXConst::MIN_STATE_TIME_MS &&
                    canComplete &&
                    !mx.isPlaying(activeChannel())) {
                    // Preserve crossfade state across transition
                    bool runningSoundActive = _runningSoundStarted;
                    swapChannels();   // Running (crossfade) channel → active
                    enterState(EngineState::Running);
                    _runningSoundStarted = runningSoundActive;
                    if (!_runningSoundStarted) {
                        playRunningSound();
                    }
                }
            }
            break;

        // ----------------------------------------------------------------
        case EngineState::Running:
            // Keep running sound looping
            if (!mx.isPlaying(activeChannel())) {
                playRunningSound();
            }

            if (!shouldRun || _requestedState == EngineState::Stopping) {
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Stopping);
                mx.stopAsync(activeChannel(), AudioStopMode::Fade);
                _runningSoundStarted = false;
                playShutdownSound();
            }
            break;

        // ----------------------------------------------------------------
        case EngineState::Stopping:
            if (shouldRun || _requestedState == EngineState::Starting) {
                // Toggle turned back on → reverse to Starting
                _requestedState = EngineState::Stopped;
                enterState(EngineState::Starting);
                mx.stopAsync(_channelA, AudioStopMode::Immediate);
                mx.stopAsync(_channelB, AudioStopMode::Immediate);
                _shutdownSoundStarted = false;
                playStartupSound(_startingOffset_ms);
            } else {
                // Async gap protection: confirm shutdown sound has begun playing
                if (_shutdownSoundStarted && !_shutdownSoundConfirmed) {
                    if (mx.isPlaying(activeChannel())) {
                        _shutdownSoundConfirmed = true;
                        ENGINE_LOG("Shutdown sound confirmed playing on ch%d", activeChannel());
                    } else if (now - _stateStart_ms >= EngineFXConst::SOUND_CONFIRM_TIMEOUT_MS) {
                        SFX_LOG_WARN("[EngineFX] Shutdown sound not playing after %lu ms — forcing advance",
                                     (unsigned long)EngineFXConst::SOUND_CONFIRM_TIMEOUT_MS);
                        _shutdownSoundConfirmed = true;
                    }
                }

                uint32_t inState = now - _stateStart_ms;
                bool canComplete = _shutdownSoundConfirmed || !_shutdownSoundStarted;
                if (inState >= EngineFXConst::MIN_STATE_TIME_MS &&
                    canComplete &&
                    !mx.isPlaying(_channelA) &&
                    !mx.isPlaying(_channelB)) {
                    enterState(EngineState::Stopped);
                    stopAllSounds();
                }
            }
            break;
    }
}

// ============================================================================
// State Machine Helpers
// ============================================================================

void EngineFX::enterState(EngineState newState) {
    if (_state == newState) return;

    SFX_LOG_INFO("[EngineFX] %s → %s",
                 engineStateString(_state), engineStateString(newState));

    _state         = newState;
    _stateStart_ms = millis();

    _startupSoundStarted   = false;
    _startupSoundConfirmed = false;
    _runningSoundStarted   = false;
    _shutdownSoundStarted  = false;
    _shutdownSoundConfirmed = false;
    _crossfadeStarted      = false;
}

// ============================================================================
// Audio Helpers
// ============================================================================

AudioPlaybackOptions EngineFX::makeOpts(bool loop, int offset_ms) const {
    AudioPlaybackOptions opts;
    opts.loop          = loop;
    opts.loopCount     = loop ? LOOP_INFINITE : 0;
    opts.volume        = 1.0f;
    opts.outputChannels = _outputChannels;
    opts.startOffsetMs = offset_ms;
    return opts;
}

void EngineFX::playStartupSound(uint32_t offset_ms) {
    if (_startupSoundStarted) return;
    if (_startingPath[0] == '\0') return;

    ENGINE_LOG("Play startup ch%d: %s (offset=%lu ms)",
               activeChannel(), _startingPath, (unsigned long)offset_ms);
    mixer().playAsync(activeChannel(), _startingPath, makeOpts(false, (int)offset_ms));
    _startupSoundStarted = true;
}

void EngineFX::playRunningSound() {
    if (_runningSoundStarted) return;
    if (_runningPath[0] == '\0') return;

    ENGINE_LOG("Play running ch%d: %s (looped)", activeChannel(), _runningPath);
    mixer().playAsync(activeChannel(), _runningPath, makeOpts(true));
    _runningSoundStarted = true;
}

void EngineFX::crossfadeToRunning() {
    if (_crossfadeStarted) return;
    if (_runningPath[0] == '\0') return;

    ENGINE_LOG("Crossfade to running ch%d: %s", crossfadeChannel(), _runningPath);
    mixer().playAsync(crossfadeChannel(), _runningPath, makeOpts(true));
    _crossfadeStarted    = true;
    _runningSoundStarted = true;
}

void EngineFX::playShutdownSound(uint32_t offset_ms) {
    if (_shutdownSoundStarted) return;
    if (_stoppingPath[0] == '\0') return;

    int shutdownCh = crossfadeChannel();
    ENGINE_LOG("Play shutdown ch%d: %s (offset=%lu ms)",
               shutdownCh, _stoppingPath, (unsigned long)offset_ms);
    mixer().playAsync(shutdownCh, _stoppingPath, makeOpts(false, (int)offset_ms));
    _shutdownSoundStarted = true;
    swapChannels();   // Shutdown is now the active channel
}

void EngineFX::stopAllSounds() {
    ENGINE_LOG("Stopping all engine sounds");

    auto& m = mixer();
    m.stopAsync(_channelA, AudioStopMode::Immediate);
    m.stopAsync(_channelB, AudioStopMode::Immediate);

    _startupSoundStarted   = false;
    _startupSoundConfirmed = false;
    _runningSoundStarted   = false;
    _shutdownSoundStarted  = false;
    _shutdownSoundConfirmed = false;
    _crossfadeStarted      = false;
}

// ============================================================================
// Manual Control
// ============================================================================

void EngineFX::forceStart() {
    SFX_LOG_INFO("[EngineFX] Force start requested");
    _forceRunning   = true;
    _requestedState = EngineState::Starting;
}

void EngineFX::forceStop() {
    SFX_LOG_INFO("[EngineFX] Force stop requested");
    _forceRunning   = false;
    _requestedState = EngineState::Stopping;
}

void EngineFX::setToggleValue(uint16_t value_us) {
    _toggleValue_us = value_us;
}
