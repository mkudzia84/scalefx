/*
 * gunfx_service.h — `GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>`.
 *
 *   Master-side gun effect (Phase 2 of GunFX rollout, instructions/22).
 *   Holds up to `kMaxGuns` `GunUnit`s, each consuming a `GunSpec`
 *   (gunfx_config.h) with muzzle-flash LED + optional recoil servo +
 *   optional smoke heater + fan + optional yaw/pitch turret + optional
 *   trigger / ROF-selector / per-axis RC inputs.
 *
 *   Wire surface:
 *     0xCC..0xD2  FIRE_ONCE / START / STOP / SMOKE_ARM / STATUS / SHOT
 *     0xE2..0xE5  MANUAL_SET / MANUAL_RELEASE / VERBOSE_STATUS_REQ /
 *                 VERBOSE_STATUS (~10 Hz broadcasts when subscribed).
 *
 *   Listens to topology role events for the per-gun RC channels and
 *   forwards them to the matching unit's input callbacks.  Optional
 *   audio plays through the local mixer (TMixer) on every shot.
 */

#ifndef HUBFX_GUNFX_SERVICE_H
#define HUBFX_GUNFX_SERVICE_H

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
#include "gun_unit.h"
#include "gunfx_config.h"
#include "gunfx_protocol.h"
#include <audio/audio_mixer.h>                  // AudioMixer concept

namespace hubfx::effects::gunfx {

inline constexpr uint8_t kMaxGuns = 4;
// Verbose status broadcast cadence (matches instructions/22 §0.5 Risks
// item #2 — bench at 5 Hz before committing if the input dispatcher
// slows). Subscriptions are per-gun; only subscribed guns broadcast.
inline constexpr uint32_t kVerboseStatusIntervalMs = 100;     // 10 Hz

template <MixerLike                                   TMixer,
          hubfx::topology::TopologyService            TTopology,
          hubfx::effects::input::InputDispatcher      TInputDispatcher>
class GunFxServicePolicyT {
public:
    static constexpr uint32_t kCapabilityBits = CoreCapability::GUNFX;

    GunFxServicePolicyT() = default;

    void configure(const GunSpec* specs, uint8_t count) {
        _numSpecs = 0;
        for (uint8_t i = 0; i < count && i < kMaxGuns; ++i) {
            _specs[_numSpecs++] = specs[i];
        }
        // Re-bind GunUnits + dispatcher subscriptions to the new spec
        // list so a /gunfx.yaml reload picks up the changes without
        // requiring a reboot (Phase 2.9.x — stopgap §8 #4).  Safe to
        // call before begin() — _ctx / _topo / _dispatcher are null on
        // the first call and the guards inside subscribePerGunInputs
        // short-circuit cleanly.
        if (_ctx && _topo) reapplySpecs();
    }

    /// Runtime-enable flag — see LandingLightService for rationale.
    void setEnabled(bool v) { _enabled = v; }
    bool enabled() const    { return _enabled; }

    // ── SystemServicePolicy surface ──────────────────────────────────

    bool begin(sfx_core::BoardServerBase* ctx);

    bool ownsType(uint8_t type) const {
        return type == GunPacket::GUN_FIRE_ONCE
            || type == GunPacket::GUN_START_FIRING
            || type == GunPacket::GUN_STOP_FIRING
            || type == GunPacket::GUN_SMOKE_ARM
            || type == GunPacket::GUN_STATUS_REQ
            || type == GunPacket::GUN_MANUAL_SET
            || type == GunPacket::GUN_MANUAL_RELEASE
            || type == GunPacket::GUN_VERBOSE_STATUS_REQ;
    }

    CommandHandleResult handle(uint8_t type,
                               const uint8_t* payload, size_t len);

    void update();

    const char* getErrorMessage(uint8_t code) const {
        return GunError::getMessage(code);
    }

    GunUnit* findById(uint8_t id);
    int8_t   indexById(uint8_t id) const;
    uint8_t  count() const { return _numSpecs; }

private:
    void claimPorts();
    void subscribePerGunInputs();
    /// Re-bind GunUnits + dispatcher subscriptions to `_specs[]`.  Called
    /// from `configure()` after a reload so a /gunfx.yaml edit takes
    /// effect without a reboot.  Idempotent — re-running it just
    /// re-subscribes the same callbacks.
    void reapplySpecs();
    void unsubscribePerGunInputs();

    void handleFireOnce         (const uint8_t* p, size_t len);
    void handleStartFiring      (const uint8_t* p, size_t len);
    void handleStopFiring       (const uint8_t* p, size_t len);
    void handleSmokeArm         (const uint8_t* p, size_t len);
    void handleStatusReq        ();
    void handleManualSet        (const uint8_t* p, size_t len);
    void handleManualRelease    (const uint8_t* p, size_t len);
    void handleVerboseStatusReq (const uint8_t* p, size_t len);

    void emitVerboseStatus(uint8_t unitIdx, uint32_t nowMs);

    // Per-shot fan-out: visual broadcast (GUN_SHOT_EVENT) ONLY — used
    // for every tick (one-shot, auto-fire tick, sustained-fire start).
    void onShotEvent(uint8_t id);
    // Audio command — `soundPath != nullptr` starts playback (looped
    // when `loop=true`, one-shot when false); `soundPath == nullptr`
    // stops the channel.  Called from fireOnce / startFiring /
    // stopFiring on the gun unit so the same audio surface handles
    // single shots AND the looping sustained-fire sample.
    void onAudioCmd(const char* soundPath, uint8_t audioChannel,
                    uint8_t outputMask, bool loop);

    static bool sendRoleCmdTrampoline(void* ctx, const PortRef& addr,
                                      uint8_t innerType,
                                      const uint8_t* p, size_t len);
    static void beginBatchTrampoline (void* ctx);
    static void commitBatchTrampoline(void* ctx);
    static void shotEventTrampoline  (void* ctx, uint8_t id);
    static void audioCmdTrampoline   (void* ctx, const char* soundPath,
                                      uint8_t audioChannel,
                                      uint8_t outputMask,
                                      bool loop);

    // Per-gun input-callback contexts. One slot per (unit, input kind);
    // dispatcher subscriptions take a TriggerInput* + a void* ctx, so
    // the trampoline reads the ctx back to (svc, unitIdx, kind).
    enum class TrigKind : uint8_t { Trigger=0, Rof=1, Yaw=2, Pitch=3, _Count };
    struct InputCtx {
        GunFxServicePolicyT* svc;
        uint8_t              unitIdx;
        TrigKind             kind;
    };
    static void inputChangeTrampoline(void* ctx,
                                      const input::TriggerValue& v);

    sfx_core::BoardServerBase* _ctx        = nullptr;
    TTopology*                 _topo       = nullptr;
    TInputDispatcher*          _dispatcher = nullptr;

    GunSpec             _specs   [kMaxGuns] = {};
    GunUnit             _units   [kMaxGuns] = {};
    // 4 trigger inputs per gun (Trigger Boolean + ROF/Yaw/Pitch Raw).
    input::TriggerInput _triggers[kMaxGuns][(uint8_t)TrigKind::_Count] = {};
    InputCtx            _inputCtx[kMaxGuns][(uint8_t)TrigKind::_Count] = {};

    uint8_t  _numSpecs = 0;
    bool     _enabled  = false;

    // Verbose-status subscription map + last-broadcast timestamps.
    bool     _verbose      [kMaxGuns] = {};
    uint32_t _lastVerboseMs[kMaxGuns] = {};

    // EffectClock-tracked tick used by GunUnit::update(dtMs).
    uint32_t _lastUpdateMs = 0;
};

}  // namespace hubfx::effects::gunfx

#include "gunfx_service.ipp"

#endif  // HUBFX_GUNFX_SERVICE_H
