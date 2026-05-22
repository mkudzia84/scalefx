/*
 * gunfx_service.h — `GunFxServicePolicyT<TMixer, TTopology>`.
 *
 *   Master-side gun effect.  Holds a small registry of `GunUnit`s,
 *   each backed by (muzzle-flash LED, optional recoil servo,
 *   optional smoke heater, optional RC trigger input).
 *
 *   Wire surface 0xCC..0xD2: FIRE_ONCE / START / STOP / SMOKE_ARM /
 *   STATUS / SHOT_EVENT.  Listens to topology role events for
 *   RCIN_VALUE_BROADCAST and forwards to the matching unit's
 *   `onTriggerInput()`.  Optional audio plays through the local
 *   mixer (TMixer) on every shot.
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
#include "gunfx_protocol.h"
#include <audio/audio_mixer.h>                  // AudioMixer concept

namespace hubfx::effects::gunfx {

inline constexpr uint8_t kMaxGuns = 4;

template <MixerLike                                   TMixer,
          hubfx::topology::TopologyService            TTopology,
          hubfx::effects::input::InputDispatcher      TInputDispatcher>
class GunFxServicePolicyT {
public:
    static constexpr uint32_t kCapabilityBits = CoreCapability::GUNFX;

    GunFxServicePolicyT() = default;

    void configure(const GunDef* defs, uint8_t count) {
        _numDefs = 0;
        for (uint8_t i = 0; i < count && i < kMaxGuns; ++i) {
            _defs[_numDefs++] = defs[i];
        }
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
            || type == GunPacket::GUN_STATUS_REQ;
    }

    CommandHandleResult handle(uint8_t type,
                               const uint8_t* payload, size_t len);

    void update();

    const char* getErrorMessage(uint8_t code) const {
        return GunError::getMessage(code);
    }

    GunUnit* findById(uint8_t id);
    uint8_t  count() const { return _numDefs; }

private:
    void claimPorts();

    void handleFireOnce   (const uint8_t* p, size_t len);
    void handleStartFiring(const uint8_t* p, size_t len);
    void handleStopFiring (const uint8_t* p, size_t len);
    void handleSmokeArm   (const uint8_t* p, size_t len);
    void handleStatusReq  ();

    // Per-shot fan-out: emit SHOT_EVENT + optionally play firing sound.
    void onShotFired(uint8_t id, const char* soundPath,
                     uint8_t audioChannel, uint8_t outputMask);

    static bool sendRoleCmdTrampoline(void* ctx, const PortRef& addr,
                                      uint8_t innerType,
                                      const uint8_t* p, size_t len);
    static void beginBatchTrampoline (void* ctx);
    static void commitBatchTrampoline(void* ctx);
    static void shotEventTrampoline  (void* ctx, uint8_t id,
                                      const char* soundPath,
                                      uint8_t audioChannel,
                                      uint8_t outputMask);

    // Per-unit trigger-input callback — routes a TriggerValue
    // change into the matching GunUnit's onTriggerInput().
    struct TriggerCtx {
        GunFxServicePolicyT* svc;
        uint8_t              unitIdx;
    };
    static void triggerChangeTrampoline(void* ctx,
                                        const input::TriggerValue& v);

    sfx_core::BoardServerBase* _ctx        = nullptr;
    TTopology*                 _topo       = nullptr;
    TInputDispatcher*          _dispatcher = nullptr;

    GunDef             _defs    [kMaxGuns] = {};
    GunUnit            _units   [kMaxGuns] = {};
    input::TriggerInput _triggers[kMaxGuns] = {};
    TriggerCtx         _trigCtx [kMaxGuns] = {};
    uint8_t            _numDefs            = 0;
    bool               _enabled            = false;   // runtime enable flag (config-driven)
};

}  // namespace hubfx::effects::gunfx

#include "gunfx_service.ipp"

#endif  // HUBFX_GUNFX_SERVICE_H
