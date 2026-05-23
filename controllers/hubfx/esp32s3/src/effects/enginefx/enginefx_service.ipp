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

    // Initial bind against `_cfg` — which is struct-default at boot
    // unless the user called configure() before begin().  The boot
    // config chain in the sketch calls configure() AFTER board.begin()
    // so the throttle re-binds via `rebindThrottle()` when /enginefx.yaml
    // lands.
    rebindThrottle();
    SFX_LOG_INFO("[engine] effect %s (channels A=%u B=%u)",
                 _cfg.enabled ? "ENABLED" : "disabled",
                 (unsigned)_cfg.channelA, (unsigned)_cfg.channelB);
    return true;
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::rebindThrottle() {
    if (!_dispatcher) return;
    _dispatcher->unsubscribe(&_throttle);

    const bool haveRc = _cfg.enabled && _cfg.rcInput.portKind != 0;
    if (!haveRc) return;

    // Boolean trigger on the resolved input channel.  Threshold +
    // hysteresis + failsafe come from `/enginefx.yaml`'s `toggle:` block.
    input::TriggerMapping m;
    m.kind         = input::TriggerKind::Boolean;
    m.thresholdUs  = _cfg.thresholdUs;
    m.hysteresisUs = _cfg.hysteresisUs;
    m.failsafe     = (input::FailsafeBehaviour)_cfg.failsafe;
    _throttle.configure(m, &EngineFxServicePolicyT::onTriggerChange, this);
    _dispatcher->subscribe(&_throttle, _cfg.rcInput, _cfg.inputChannel);
    SFX_LOG_INFO("[engine] RC toggle bound: %s:%u ch=%u thresh=%uus hyst=%uus",
                 _cfg.rcInput.guid[0] ? _cfg.rcInput.guid : "hub",
                 (unsigned)_cfg.rcInput.portIdx,
                 (unsigned)_cfg.inputChannel,
                 (unsigned)_cfg.thresholdUs,
                 (unsigned)_cfg.hysteresisUs);
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
                    startAudio(_cfg.runningPath, _cfg.channelB, 0, /*loop=*/true)) {
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

    // Starting sound is OPTIONAL: when present we play it on channel A,
    // then the tick() handler swaps to the running loop on channel B once
    // it ends.  When absent we go straight to the running loop.
    if (_cfg.startingPath[0]) {
        _activeChannel = _cfg.channelA;
        if (!startAudio(_cfg.startingPath, _cfg.channelA, _cfg.startingOffsetMs,
                        /*loop=*/false, /*fadeInMs=*/_cfg.startFadeInMs)) {
            return false;
        }
        enterState(EngineState::Starting);
        return true;
    }

    // No starting sound → go to Running directly (skip the Starting phase).
    if (!_cfg.runningPath[0]) {
        SFX_LOG_WARN("[engine] start: no starting OR running path configured");
        return false;
    }
    _activeChannel = _cfg.channelB;
    if (!startAudio(_cfg.runningPath, _cfg.channelB, 0,
                    /*loop=*/true, /*fadeInMs=*/_cfg.startFadeInMs)) {
        return false;
    }
    enterState(EngineState::Running);
    return true;
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
bool EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::forceStop() {
    if (!_cfg.enabled) return false;
    if (_state == EngineState::Stopping || _state == EngineState::Stopped) return true;
    stopAudio(_activeChannel);
    if (_cfg.stoppingPath[0]) {
        _activeChannel = _cfg.channelA;
        if (startAudio(_cfg.stoppingPath, _cfg.channelA, _cfg.stoppingOffsetMs,
                       /*loop=*/false, /*fadeInMs=*/0, /*fadeOutMs=*/_cfg.stopFadeOutMs)) {
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
        const char* path, uint8_t channel, uint32_t offsetMs, bool loop,
        uint16_t fadeInMs, uint16_t fadeOutMs) {
#if defined(SFX_HAS_AUDIO)
    AudioPlaybackOptions opts;
    opts.volume         = 1.0f;
    opts.outputChannels = _cfg.outputMask;
    opts.startOffsetMs  = static_cast<int>(offsetMs);
    opts.fadeInMs       = fadeInMs;
    opts.fadeOutMs      = fadeOutMs;
    // The running loop must repeat forever; the start/stop one-shots must
    // play exactly once.  The mixer disables looping when `loop == false`
    // OR `loopCount == 0`, so set both fields explicitly per call (the
    // struct defaults — loop=false, loopCount=INFINITE — resolve to "play
    // once", which silently broke the running loop).
    opts.loop      = loop;
    opts.loopCount = loop ? LOOP_INFINITE : 0;
    return TMixer::instance().playAsync(channel, path, opts);
#else
    (void)path; (void)channel; (void)offsetMs; (void)loop; (void)fadeInMs; (void)fadeOutMs;
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
    // Edge-only: the toggle starts/stops the engine on a deliberate flip,
    // never on the boot/reload baseline.  `v.initial` is the first event
    // after (re)subscribe — adopt the level for status, but don't act, so
    // a reflash can't auto-start the engine or play the shutdown sound on
    // a settling-RC transient.
    if (v.initial) return;
    if (v.b) self->forceStart();
    else     self->forceStop();
}

}  // namespace hubfx::effects::enginefx

#endif  // HUBFX_ENGINEFX_SERVICE_IPP
