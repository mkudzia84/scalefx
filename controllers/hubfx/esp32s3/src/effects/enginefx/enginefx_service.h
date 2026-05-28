/*
 * enginefx_service.h — `EngineFxServicePolicyT<TMixer, TTopology>`.
 *
 *   Master-side aircraft-engine state machine.  4 states: Stopped /
 *   Starting / Running / Stopping.  Drives the local audio mixer
 *   (TMixer = `AudioMixer<TI2S, TCodec>`) via its async API.  Listens
 *   for an optional RC PWM toggle through topology role events
 *   (RCIN_VALUE_BROADCAST) — when enabled, RC channel crossings drive
 *   start / stop alongside the explicit ENGINE_START / ENGINE_STOP
 *   protocol commands.
 *
 *   v1 ships a sequential play model (no startup-to-running cross-fade —
 *   that's a follow-up).  Configuration is install-time via `configure()`;
 *   YAML loading comes later.
 */

#ifndef HUBFX_ENGINEFX_SERVICE_H
#define HUBFX_ENGINEFX_SERVICE_H

#include <cstdint>
#include <cstring>

#include <serial/core/core.h>
#include <serial/diag_log.h>
#include <serial/roles.h>
#include <server/board_server.h>

#include "../effect_id.h"
#include "../../topology/topology_service.h"   // TopologyService concept
#include "../input/trigger_input.h"
#include "../input/input_dispatcher.h"         // InputDispatcher concept
#include "enginefx_protocol.h"
#include <audio/audio_mixer.h>                  // AudioMixer concept

namespace hubfx::effects::enginefx {

/// Static configuration installed on `configure()`.
struct EngineFxConfig {
    bool     enabled         = false;

    /// Optional RC throttle toggle — leave `rcInput.portKind == 0` to
    /// disable the auto-start path.  Must have an `RcPwmInput`,
    /// `SbusInput`, or `JetiExInput` role.  `inputChannel` is the
    /// 0-based channel id within the source protocol (PWM = 0;
    /// SBUS 0..17; Jeti 0..23).
    PortRef  rcInput;
    uint8_t  inputChannel    = 0;
    uint16_t thresholdUs     = 1500;
    uint16_t hysteresisUs    = 100;
    uint8_t  failsafe        = 1;     ///< FailsafeBehaviour::ForceLow

    /// Audio mixer channel allocation.  Two slots cycle so the
    /// "shutdown" sound can start before the "running" loop fully
    /// fades out.
    uint8_t  channelA        = 0;
    uint8_t  channelB        = 1;
    uint8_t  outputMask      = 0x03;          ///< AudioChannel::ALL

    char     startingPath[64] = {};
    char     runningPath [64] = {};
    char     stoppingPath[64] = {};
    uint32_t startingOffsetMs = 0;
    uint32_t stoppingOffsetMs = 0;

    /// Optional fade-in for the START sound (0 = play at full volume from
    /// the first sample).  Ramps the starting track 0 → full over this
    /// many ms — a soft spool-up.  Only the start sound fades in; the
    /// running loop and shutdown sound play at full volume.
    uint16_t startFadeInMs    = 0;

    /// Optional fade-out for the STOP sound (0 = play to the last sample).
    /// Ramps the shutdown track full → 0 over its final this-many ms — a
    /// soft wind-down tail.  Symmetric with `startFadeInMs`.
    uint16_t stopFadeOutMs    = 0;

    /// Cross-fade window between successive engine tracks (start→loop,
    /// loop→loop wrap, loop→stop).  The "next" track launches on the
    /// OTHER mixer slot (channelA ⇋ channelB) this many ms BEFORE the
    /// current track's natural end, with fadeIn = crossfadeMs ramping
    /// in from 0.  Simultaneously the current channel fades out over
    /// the same window via stopAsyncWithFadeMs.  Masks the source-
    /// teardown + drain pre-fill latency that causes the audible gap
    /// on every transition.  0 = sequential play (legacy behaviour).
    uint16_t crossfadeMs      = 200;
};

template <MixerLike                                   TMixer,
          hubfx::topology::TopologyService            TTopology,
          hubfx::effects::input::InputDispatcher      TInputDispatcher>
class EngineFxServicePolicyT {
public:
    static constexpr uint32_t kCapabilityBits = CoreCapability::ENGINE;

    EngineFxServicePolicyT() = default;

    void configure(const EngineFxConfig& cfg) {
        _cfg = cfg;
        // begin() runs once at boot with the struct-default _cfg
        // (rcInput unset).  YAML config arrives later via this call;
        // re-bind the throttle TriggerInput so the new rcInput +
        // channel + threshold take effect.  No-op when begin() hasn't
        // run yet (`_dispatcher == nullptr`).
        rebindThrottle();
    }

    /// Runtime kill-switch — `/hubfx.yaml`'s `enginefx.enabled:` flag
    /// flips this independently of the full `configure()` call so the
    /// master enable matrix lives in one file (Rule 26 — central enable
    /// surface).  Symmetric with LandingLight / Gear / LightFx / GunFx.
    void setEnabled(bool v) { _cfg.enabled = v; }

    /// Runtime-enable accessor — picked up by
    /// `BoardServer::computeEnabledCapabilities()`.  Driven by
    /// `_cfg.enabled`.
    bool enabled() const { return _cfg.enabled; }

    // ── SystemServicePolicy surface ──────────────────────────────────

    bool begin(sfx_core::BoardServerBase* ctx);

    bool ownsType(uint8_t type) const {
        return type == EnginePacket::ENGINE_START
            || type == EnginePacket::ENGINE_STOP
            || type == EnginePacket::ENGINE_STATUS_REQ;
    }

    CommandHandleResult handle(uint8_t type,
                               const uint8_t* payload, size_t len);

    /// Per-loop tick — advance state machine, watch audio for the
    /// transition trigger (start sound finished → run loop; stop
    /// sound finished → stopped).
    void update();

    const char* getErrorMessage(uint8_t code) const {
        return EngineError::getMessage(code);
    }

    // ── Direct API ──────────────────────────────────────────────────

    bool forceStart();
    bool forceStop();
    uint8_t state() const { return _state; }
    bool    toggleEngaged() const { return _toggleEngaged; }
    bool    active() const { return _state != EngineState::Stopped; }

private:
    void enterState(uint8_t newState);
    void emitStateEvent(uint8_t newState);

    /// Wire / rewire the throttle TriggerInput against the current
    /// `_cfg` + `_dispatcher`.  Called from `begin()` (initial bind)
    /// and from `configure()` (when YAML lands after boot).  Safe to
    /// call before `begin()` — early-outs on `_dispatcher == nullptr`.
    void rebindThrottle();

    /// Play `path` on `channel`.  `loop == true` repeats forever (running
    /// loop); `false` plays once (start / stop one-shots).  `fadeInMs > 0`
    /// ramps the track 0 → full over that many ms (start spool-up);
    /// `fadeOutMs > 0` ramps full → 0 over its final ms (stop wind-down,
    /// one-shots only).
    bool startAudio(const char* path, uint8_t channel, uint32_t offsetMs,
                    bool loop, uint16_t fadeInMs = 0, uint16_t fadeOutMs = 0);
    void stopAudio (uint8_t channel);

    /// Cross-fade transition: launch `nextPath` on the OTHER channel
    /// (relative to `_activeChannel`) with fadeIn=crossfadeMs, then
    /// fade out `_activeChannel` over the same window, swap
    /// `_activeChannel`, and enter `nextState`.  Used at the end of
    /// every engine track (start → running, running → running [loop
    /// wrap], running → stopping).  When `_cfg.crossfadeMs == 0`,
    /// falls back to the legacy sequential-play path (hard cut).
    bool transitionTo(const char* nextPath, bool loop, uint8_t nextState);

    /// `_activeChannel` swap helper.
    uint8_t otherChannel() const {
        return (_activeChannel == _cfg.channelA) ? _cfg.channelB : _cfg.channelA;
    }

    /// Callback fired by the TriggerInput on edge changes.  Drives
    /// forceStart / forceStop based on the boolean value.
    static void onTriggerChange(void* ctx,
                                const input::TriggerValue& v);

    sfx_core::BoardServerBase* _ctx        = nullptr;
    TTopology*                 _topo       = nullptr;
    TInputDispatcher*          _dispatcher = nullptr;

    EngineFxConfig _cfg{};
    uint8_t        _state          = EngineState::Stopped;
    uint8_t        _activeChannel  = 0;          ///< current primary mixer slot (channelA or channelB)
    uint32_t       _stateEnteredMs = 0;
    uint32_t       _lastSoundCheckMs = 0;
    /// Set true by forceStop() while Starting/Running.  The cross-
    /// fade scheduler swaps to the stoppingPath instead of the next
    /// loop iteration on its next firing.
    bool           _pendingStop    = false;
    /// Guards re-entry of the cross-fade scheduler during the
    /// crossfade window (we don't want to fire two `transitionTo`s
    /// for the same approaching end).
    bool           _transitionScheduled = false;

    // Throttle-toggle input plumbing.  When the user configured an
    // `rcInput` PortRef, the TriggerInput is registered with the
    // InputDispatcher at begin() time; otherwise it stays idle.
    input::TriggerInput _throttle{};
    bool                _toggleEngaged = false;
};

}  // namespace hubfx::effects::enginefx

#include "enginefx_service.ipp"

#endif  // HUBFX_ENGINEFX_SERVICE_H
