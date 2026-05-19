/*
 * enginefx_service.ipp — engine state-machine template-method bodies.
 */

#ifndef HUBFX_ENGINEFX_SERVICE_IPP
#define HUBFX_ENGINEFX_SERVICE_IPP

#include <Arduino.h>       // millis()
#include <serial/wire.h>

#if defined(SFX_HAS_AUDIO)
#include <audio/audio_mixer.h>
#endif

namespace hubfx::effects::enginefx {

// ─── Lifecycle ──────────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
bool EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::begin(
        sfx_core::BoardServerBase* ctx) {
    _ctx = ctx;
    if (!_ctx) return false;
    _topo       = ctx->template findPolicy<TTopology>();
    _dispatcher = ctx->template findPolicy<TInputDispatcher>();

    const bool haveRc = _cfg.enabled
                     && _cfg.rcInput.portKind != 0
                     && _dispatcher != nullptr;
    if (haveRc) {
        // Configure the TriggerInput as a Boolean with the legacy
        // 100 µs hysteresis band around `thresholdUs`.  On change,
        // `onTriggerChange` flips engine state.
        input::TriggerMapping m;
        m.kind         = input::TriggerKind::Boolean;
        m.thresholdUs  = _cfg.thresholdUs;
        m.hysteresisUs = 100;
        m.failsafe     = input::FailsafeBehaviour::ForceLow;
        _throttle.configure(m, &EngineFxServicePolicyT::onTriggerChange, this);
        _dispatcher->subscribe(&_throttle, _cfg.rcInput, /*channel=*/0);
        SFX_LOG_INFO("[engine] RC toggle bound: %s:%u thresh=%u",
                     _cfg.rcInput.guid[0] ? _cfg.rcInput.guid : "hub",
                     (unsigned)_cfg.rcInput.portIdx,
                     (unsigned)_cfg.thresholdUs);
    }
    SFX_LOG_INFO("[engine] effect %s (channels A=%u B=%u)",
                 _cfg.enabled ? "ENABLED" : "disabled",
                 (unsigned)_cfg.channelA, (unsigned)_cfg.channelB);
    return true;
}

// ─── Tick ───────────────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::update() {
    if (!_cfg.enabled) return;
    const uint32_t now = millis();
    if (now - _lastSoundCheckMs < 50) return;     // 20 Hz tick is plenty
    _lastSoundCheckMs = now;

#if defined(SFX_HAS_AUDIO)
    auto& mixer = TMixer::instance();
    const bool playing = mixer.isPlaying(_activeChannel);
#else
    const bool playing = false;
#endif

    switch (_state) {
        case EngineState::Starting:
            // Wait at least 200 ms to give playAsync time to actually
            // start, then advance once the start sound has finished.
            if (now - _stateEnteredMs >= 200 && !playing) {
                _activeChannel = _cfg.channelB;
                if (_cfg.runningPath[0] &&
                    startAudio(_cfg.runningPath, _cfg.channelB, 0)) {
                    enterState(EngineState::Running);
                } else {
                    SFX_LOG_WARN("[engine] no running path — going Stopped");
                    enterState(EngineState::Stopped);
                }
            }
            break;
        case EngineState::Stopping:
            if (now - _stateEnteredMs >= 200 && !playing) {
                enterState(EngineState::Stopped);
            }
            break;
        default:
            break;
    }
}

// ─── Wire dispatch ──────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
CommandHandleResult EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handle(
        uint8_t type, const uint8_t* /*payload*/, size_t /*len*/) {
    switch (type) {
        case EnginePacket::ENGINE_START:
            if (!_cfg.enabled) { _ctx->sendNack(EngineError::ENGINE_NOT_AVAILABLE); return CommandHandleResult::Handled; }
            forceStart();
            _ctx->sendAck();
            return CommandHandleResult::Handled;
        case EnginePacket::ENGINE_STOP:
            if (!_cfg.enabled) { _ctx->sendNack(EngineError::ENGINE_NOT_AVAILABLE); return CommandHandleResult::Handled; }
            forceStop();
            _ctx->sendAck();
            return CommandHandleResult::Handled;
        case EnginePacket::ENGINE_STATUS_REQ: {
            uint8_t buf[3] = {
                _state,
                static_cast<uint8_t>(_toggleEngaged ? 1 : 0),
                static_cast<uint8_t>(active()       ? 1 : 0),
            };
            _ctx->sendRawPacket(EnginePacket::ENGINE_STATUS_RESP,
                                _ctx->currentTag(), buf, sizeof(buf));
            return CommandHandleResult::Handled;
        }
        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ─── Direct API ─────────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
bool EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::forceStart() {
    if (!_cfg.enabled) return false;
    if (_state == EngineState::Starting || _state == EngineState::Running) return true;
    if (_state == EngineState::Stopping) stopAudio(_activeChannel);
    if (!_cfg.startingPath[0]) {
        SFX_LOG_WARN("[engine] start: no starting path configured");
        return false;
    }
    _activeChannel = _cfg.channelA;
    if (!startAudio(_cfg.startingPath, _cfg.channelA, _cfg.startingOffsetMs)) {
        return false;
    }
    enterState(EngineState::Starting);
    return true;
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
bool EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::forceStop() {
    if (!_cfg.enabled) return false;
    if (_state == EngineState::Stopping || _state == EngineState::Stopped) return true;
    stopAudio(_activeChannel);
    if (_cfg.stoppingPath[0]) {
        _activeChannel = _cfg.channelA;
        if (startAudio(_cfg.stoppingPath, _cfg.channelA, _cfg.stoppingOffsetMs)) {
            enterState(EngineState::Stopping);
            return true;
        }
    }
    enterState(EngineState::Stopped);
    return true;
}

// ─── Internal helpers ───────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::enterState(uint8_t newState) {
    if (newState == _state) return;
    _state = newState;
    _stateEnteredMs = millis();
    SFX_LOG_INFO("[engine] → %s", EngineState::getName(newState));
    emitStateEvent(newState);
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::emitStateEvent(uint8_t newState) {
    if (!_ctx) return;
    const uint8_t payload[1] = { newState };
    _ctx->sendRawPacket(EnginePacket::ENGINE_STATE_EVENT,
                        SfxWire::TAG_ASYNC, payload, sizeof(payload));
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
bool EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::startAudio(
        const char* path, uint8_t channel, uint32_t offsetMs) {
#if defined(SFX_HAS_AUDIO)
    AudioPlaybackOptions opts;
    opts.volume         = 1.0f;
    opts.outputChannels = _cfg.outputMask;
    opts.startOffsetMs  = static_cast<int>(offsetMs);
    return TMixer::instance().playAsync(channel, path, opts);
#else
    (void)path; (void)channel; (void)offsetMs;
    return false;
#endif
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::stopAudio(uint8_t channel) {
#if defined(SFX_HAS_AUDIO)
    TMixer::instance().stopAsync(channel, AudioStopMode::Immediate);
#else
    (void)channel;
#endif
}

// ─── Trigger-input callback ────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::onTriggerChange(
        void* ctx, const input::TriggerValue& v) {
    auto* self = static_cast<EngineFxServicePolicyT*>(ctx);
    if (!self) return;
    if (v.kind != input::TriggerKind::Boolean) return;
    self->_toggleEngaged = v.b;
    if (v.b) self->forceStart();
    else     self->forceStop();
}

}  // namespace hubfx::effects::enginefx

#endif  // HUBFX_ENGINEFX_SERVICE_IPP
