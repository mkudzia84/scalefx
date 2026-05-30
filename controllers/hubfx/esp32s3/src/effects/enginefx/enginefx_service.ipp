/*
 * enginefx_service.ipp — engine state-machine template-method bodies.
 */

#ifndef HUBFX_ENGINEFX_SERVICE_IPP
#define HUBFX_ENGINEFX_SERVICE_IPP

#include <Arduino.h>
#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include <serial/wire.h>
#include <server/effect_clock.h>   // Rule 40 — effects use EffectClock, not raw SFX_MILLIS()

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
    const uint32_t now = sfx_core::EffectClock::instance().nowMs();
    if (now - _lastSoundCheckMs < 50) return;     // 20 Hz tick is plenty
    _lastSoundCheckMs = now;

#if defined(SFX_HAS_AUDIO)
    auto& mixer = TMixer::instance();
    const bool   playing       = mixer.isPlaying(_activeChannel);
    const float  remainingSec  = mixer.remainingSec(_activeChannel);
    const float  crossfadeSec  = _cfg.crossfadeMs / 1000.0f;
#else
    const bool   playing       = false;
    const float  remainingSec  = 0.0f;
    const float  crossfadeSec  = 0.0f;
#endif

    switch (_state) {
        case EngineState::Starting:
        case EngineState::Running: {
            // ── Cross-fade scheduler ────────────────────────────────
            // When the current track's natural-end is within the
            // crossfade window AND we haven't already scheduled the
            // swap, launch the next track on the OTHER mixer slot with
            // a matching fade-in and fade out the current slot.  The
            // exception is _cfg.crossfadeMs == 0 (legacy sequential
            // play): wait for the track to actually finish, then start
            // the next one with a hard cut.
            const bool inCrossfadeWindow =
                _cfg.crossfadeMs > 0
                && playing
                && remainingSec > 0.0f
                && remainingSec <= crossfadeSec;

            const bool fellOffEnd = !playing && (now - _stateEnteredMs >= 200);

            if (inCrossfadeWindow && !_transitionScheduled) {
                // Pick the next track per the state machine.  Pending
                // stop wins over loop wrap so a forceStop during
                // Starting/Running cleanly hands off to the stop sound.
                const char* nextPath = nullptr;
                uint8_t     nextState = _state;
                if (_state == EngineState::Starting) {
                    if (_pendingStop) {
                        nextPath  = _cfg.stoppingPath;
                        nextState = EngineState::Stopping;
                        _pendingStop = false;
                    } else if (_cfg.runningPath[0]) {
                        nextPath  = _cfg.runningPath;
                        nextState = EngineState::Running;
                    }
                } else /* Running */ {
                    if (_pendingStop) {
                        nextPath  = _cfg.stoppingPath;
                        nextState = EngineState::Stopping;
                        _pendingStop = false;
                    } else if (_cfg.runningPath[0]) {
                        // Loop wrap — re-launch the same path on the
                        // other channel so the wrap glitch is masked.
                        nextPath  = _cfg.runningPath;
                        nextState = EngineState::Running;
                    }
                }
                if (nextPath && nextPath[0]) {
                    _transitionScheduled = true;
                    transitionTo(nextPath, /*loop=*/false, nextState);
                }
            } else if (fellOffEnd) {
                // Sequential-play path OR the cross-fade window
                // somehow missed (very short track + slow tick).
                // Fall through to the legacy "play next now" hard cut.
                if (_state == EngineState::Starting) {
                    if (_pendingStop) {
                        _pendingStop = false;
                        if (_cfg.stoppingPath[0] &&
                            startAudio(_cfg.stoppingPath, _activeChannel,
                                       _cfg.stoppingOffsetMs, /*loop=*/false,
                                       /*fadeInMs=*/0,
                                       /*fadeOutMs=*/_cfg.stopFadeOutMs)) {
                            enterState(EngineState::Stopping);
                        } else {
                            enterState(EngineState::Stopped);
                        }
                    } else {
                        _activeChannel = _cfg.channelB;
                        if (_cfg.runningPath[0] &&
                            startAudio(_cfg.runningPath, _activeChannel,
                                       0, /*loop=*/false)) {
                            enterState(EngineState::Running);
                        } else {
                            SFX_LOG_WARN("[engine] no running path — going Stopped");
                            enterState(EngineState::Stopped);
                        }
                    }
                } else /* Running */ {
                    if (_pendingStop) {
                        _pendingStop = false;
                        if (_cfg.stoppingPath[0] &&
                            startAudio(_cfg.stoppingPath, _activeChannel,
                                       _cfg.stoppingOffsetMs, /*loop=*/false,
                                       /*fadeInMs=*/0,
                                       /*fadeOutMs=*/_cfg.stopFadeOutMs)) {
                            enterState(EngineState::Stopping);
                        } else {
                            enterState(EngineState::Stopped);
                        }
                    } else if (_cfg.runningPath[0]) {
                        // Loop wrap (sequential mode): just re-launch
                        // on the same channel.
                        startAudio(_cfg.runningPath, _activeChannel,
                                   0, /*loop=*/false);
                    } else {
                        enterState(EngineState::Stopped);
                    }
                }
            }
            break;
        }

        case EngineState::Stopping: {
            // Stop sound is a one-shot; once it finishes (or the
            // fade-out completes) we're done.  No cross-fade out of
            // Stopping — that would just be silence overlapping
            // silence.
            const bool fellOffEnd = !playing && (now - _stateEnteredMs >= 200);
            if (fellOffEnd) {
                enterState(EngineState::Stopped);
            }
            break;
        }

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

    // The starting offset is a WARM-start skip: re-engaging while the engine
    // is still spooling down (Stopping) jumps INTO the starting sound by
    // startingOffsetMs so we don't replay the full ignition.  A COLD start
    // from a fully Stopped engine plays the starting sound from 0 (the whole
    // ignition / spool-up).  Capture the prior state BEFORE we mutate it.
    const bool warmStart = (_state == EngineState::Stopping);
    if (_state == EngineState::Stopping) stopAudio(_activeChannel);

    _pendingStop         = false;
    _transitionScheduled = false;

    // Starting sound is OPTIONAL: when present we play it on channel A
    // as a ONE-SHOT, then the cross-fade scheduler in update() swaps
    // to the running loop on channel B near the end.  Each subsequent
    // loop iteration also alternates channels via cross-fade so the
    // loop wrap glitch is masked.
    if (_cfg.startingPath[0]) {
        _activeChannel = _cfg.channelA;
        const uint32_t startOffsetMs = warmStart ? _cfg.startingOffsetMs : 0;
        // Fade-in is for the COLD start only (silence → ignition).  A WARM
        // re-engage seeks INTO an already-spun-up engine, so it must come in at
        // full level — a fade there sounds like the engine fading up from
        // nothing.  No offset (cold) → fade; offset (warm) → hard in.
        const uint16_t fadeInMs = warmStart ? 0 : _cfg.startFadeInMs;
        SFX_LOG_INFO("[engine] forceStart %s — '%s' offset=%lums fadeIn=%ums (cfg startingOffset=%lums)",
                     warmStart ? "WARM (from stopping)" : "COLD (from stopped)",
                     _cfg.startingPath, (unsigned long)startOffsetMs,
                     (unsigned)fadeInMs, (unsigned long)_cfg.startingOffsetMs);
        if (!startAudio(_cfg.startingPath, _cfg.channelA, startOffsetMs,
                        /*loop=*/false, /*fadeInMs=*/fadeInMs)) {
            return false;
        }
        enterState(EngineState::Starting);
        return true;
    }

    // No starting sound → go to Running directly.  The first loop
    // iteration plays as a one-shot on channel A; the cross-fade
    // scheduler alternates from there.
    if (!_cfg.runningPath[0]) {
        SFX_LOG_WARN("[engine] start: no starting OR running path configured");
        return false;
    }
    _activeChannel = _cfg.channelA;
    if (!startAudio(_cfg.runningPath, _activeChannel, 0,
                    /*loop=*/false, /*fadeInMs=*/_cfg.startFadeInMs)) {
        return false;
    }
    enterState(EngineState::Running);
    return true;
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
bool EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::forceStop() {
    if (!_cfg.enabled) return false;
    if (_state == EngineState::Stopping || _state == EngineState::Stopped) return true;

    // Cross-fade path: mark _pendingStop and let the scheduler in
    // update() swap to the stop sound at the next crossfade window.
    // For the operator's-finger UX (stop right now), still trigger an
    // immediate transition if cross-fade is enabled — otherwise we'd
    // wait up to one full loop iteration before hearing anything.
    _pendingStop = true;
    if (_cfg.crossfadeMs > 0 && _cfg.stoppingPath[0]) {
        if (transitionTo(_cfg.stoppingPath, /*loop=*/false, EngineState::Stopping)) {
            _pendingStop = false;
            return true;
        }
    }

    // Legacy path (crossfadeMs == 0, or transitionTo failed): hard cut.
    stopAudio(_activeChannel);
    if (_cfg.stoppingPath[0]) {
        _activeChannel = _cfg.channelA;
        if (startAudio(_cfg.stoppingPath, _activeChannel, _cfg.stoppingOffsetMs,
                       /*loop=*/false, /*fadeInMs=*/0, /*fadeOutMs=*/_cfg.stopFadeOutMs)) {
            _pendingStop = false;
            enterState(EngineState::Stopping);
            return true;
        }
    }
    _pendingStop = false;
    enterState(EngineState::Stopped);
    return true;
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
bool EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::transitionTo(
        const char* nextPath, bool loop, uint8_t nextState) {
    if (!nextPath || !nextPath[0]) return false;
    const uint8_t next = otherChannel();
    const uint16_t cf  = _cfg.crossfadeMs;

    // Launch the next track on the other channel with a matching
    // fade-in.  If this is the stop sound, also apply the legacy
    // stopFadeOutMs as a tail fade.
    const bool isStop = (nextState == EngineState::Stopping);
    if (!startAudio(nextPath, next,
                    isStop ? _cfg.stoppingOffsetMs : 0u,
                    /*loop=*/loop,
                    /*fadeInMs=*/cf,
                    /*fadeOutMs=*/isStop ? _cfg.stopFadeOutMs : 0u)) {
        SFX_LOG_WARN("[engine] crossfade: failed to start %s on ch%u",
                     nextPath, (unsigned)next);
        return false;
    }

#if defined(SFX_HAS_AUDIO)
    // Fade out the previously-active channel over the same window.
    // Mixer destroys the source automatically when the fade completes.
    TMixer::instance().stopAsyncWithFadeMs(_activeChannel, cf);
#endif

    SFX_LOG_INFO("[engine] crossfade %ums: ch%u (current) ⇋ ch%u (%s)",
                 (unsigned)cf, (unsigned)_activeChannel,
                 (unsigned)next, nextPath);

    _activeChannel       = next;
    _transitionScheduled = false;        // reset for the next cycle
    enterState(nextState);
    return true;
}

// ─── Internal helpers ───────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void EngineFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::enterState(uint8_t newState) {
    if (newState == _state) {
        // Even when the state label doesn't change (Running → Running
        // on a loop wrap), the next cross-fade cycle is fresh — clear
        // the in-flight guard so the scheduler fires again when the
        // new track approaches its end.
        _transitionScheduled = false;
        _stateEnteredMs      = sfx_core::EffectClock::instance().nowMs();
        return;
    }
    _state               = newState;
    _stateEnteredMs      = sfx_core::EffectClock::instance().nowMs();
    _transitionScheduled = false;
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
