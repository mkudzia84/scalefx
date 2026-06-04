/*
 * landing_activation.h — RC-channel auto-deploy driver for landing lights.
 *
 *   When a `/landing.yaml` entry carries an `activation:` block, the
 *   sketch wires one of these per board.  For every landing light with a
 *   non-empty `activation.input`, it owns a Boolean `TriggerInput`,
 *   subscribes it to the named RC channel (resolved against
 *   `/hubfx.yaml`'s `inputs[]`), and on every edge calls
 *   `landingService.setState(id, On/Off, EffectId::LandingLight)` —
 *   so the searchlight deploys above the threshold and retracts below.
 *
 *   This is the landing-light twin of `LightFxProgramSelector`: a
 *   stand-alone driver so `LandingLightServicePolicyT` keeps its single
 *   topology template parameter (LightFx + GearControl take it as a
 *   policy arg and must not gain an InputDispatcher dependency).  The
 *   sketch wires the dispatcher + the landing service into it at apply
 *   time and re-installs it on every CONFIG_RELOAD.
 *
 *   Failsafe is fail-LOW (retract on RC signal loss) — a dropped link
 *   stows the light rather than leaving it deployed.
 *
 *   Program-attach ("deploy while program X active") is NOT handled here
 *   — Studio injects a `landing_bindings:[{id,state}]` into the program
 *   YAML so the existing program→landing path drives it.
 */

#ifndef HUBFX_LANDING_ACTIVATION_H
#define HUBFX_LANDING_ACTIVATION_H

#include <cstdint>
#include <cstring>

#include <serial/diag_log.h>
#include <serial/ports.h>                       // PortKind::getName

#include "../effects/effect_id.h"               // EffectId
#include "../effects/input/trigger_input.h"     // TriggerInput / TriggerValue
#include "../effects/landing_lights/landing_light_protocol.h"  // LandingLightState
#include "landing_config.h"                      // LandingConfig / LandingActivation
#include "hubfx_config.h"                         // HubFxConfig / findInputByName

namespace hubfx::config {

class LandingActivationDriver {
public:
    LandingActivationDriver() = default;

    /// (Re)wire every channel-activated landing light.  Idempotent:
    /// drops all prior subscriptions first, then re-subscribes from the
    /// freshly-applied config — so a CONFIG_RELOAD or a disable cleanly
    /// detaches stale triggers.  Lights with an empty `activation.input`
    /// are skipped (manual / program-driven).
    template <typename TInputDispatcher, typename TLandingService>
    void install(TInputDispatcher* dispatcher,
                 TLandingService* landing,
                 const LandingConfig& cfg,
                 const HubFxConfig& hub) {
        // Detach the previous round up-front (typed dispatcher in hand).
        if (dispatcher) {
            for (uint8_t i = 0; i < _numSlots; ++i) {
                dispatcher->unsubscribe(&_slots[i].trigger);
            }
        }
        _numSlots   = 0;
        _landing    = nullptr;
        _setStateFn = nullptr;

        if (!dispatcher || !landing) return;     // dormant

        _landing    = landing;
        _setStateFn = [](void* p, uint8_t id, uint8_t state) {
            return static_cast<TLandingService*>(p)->setState(
                id, state, hubfx::effects::EffectId::LandingLight);
        };

        using namespace hubfx::effects::input;
        for (uint8_t i = 0; i < cfg.numLights
                            && _numSlots < hubfx::effects::landing::kMaxLandingLights; ++i) {
            const LandingActivation& act = cfg.activations[i];
            if (!act.input[0]) continue;          // manual / program-driven

            const auto* b = findInputByName(hub, act.input);
            if (!b) {
                SFX_LOG_WARN("[landing-activation] id=%u: input '%s' not in /hubfx.yaml inputs[] — skipping",
                             (unsigned)cfg.lights[i].id, act.input);
                continue;
            }

            Slot& s  = _slots[_numSlots];
            s.id     = cfg.lights[i].id;
            s.driver = this;

            TriggerMapping m;
            m.kind         = TriggerKind::Boolean;
            m.thresholdUs  = act.thresholdUs;
            m.hysteresisUs = act.hysteresisUs;
            m.failsafe     = FailsafeBehaviour::ForceLow;   // retract on RC loss
            s.trigger.configure(m, &LandingActivationDriver::onChange, &s);

            const uint8_t channel = (b->channelId > 0)
                                  ? (uint8_t)(b->channelId - 1) : 0;
            if (dispatcher->subscribe(&s.trigger, b->port, channel)) {
                SFX_LOG_INFO("[landing-activation] id=%u bound to {%s,%u} ch=%u thresh=%uus hyst=%uus",
                             (unsigned)s.id,
                             PortKind::getName(b->port.portKind),
                             (unsigned)b->port.portIdx,
                             (unsigned)channel,
                             (unsigned)act.thresholdUs,
                             (unsigned)act.hysteresisUs);
                _numSlots++;
            } else {
                SFX_LOG_WARN("[landing-activation] id=%u: dispatcher subscribe failed", (unsigned)s.id);
            }
        }
    }

private:
    using SetStateFn = bool (*)(void* landingPtr, uint8_t id, uint8_t state);

    /// One channel-bound landing light.  The trigger's callback ctx is
    /// the address of this slot (stable — `_slots` is a member array),
    /// so onChange recovers both the driver and the light id.
    struct Slot {
        hubfx::effects::input::TriggerInput trigger{};
        uint8_t                  id     = 0;
        LandingActivationDriver* driver = nullptr;
    };

    static void onChange(void* ctx,
                         const hubfx::effects::input::TriggerValue& v) {
        if (!ctx || v.kind != hubfx::effects::input::TriggerKind::Boolean) return;
        auto* s = static_cast<Slot*>(ctx);
        auto* self = s->driver;
        if (!self || !self->_setStateFn || !self->_landing) return;
        // Fail-low: an invalid (signal-lost) value resolves to b=false ⇒
        // retract, which is the safe state — so we act on it too.
        const uint8_t state = v.b
            ? hubfx::effects::landing::LandingLightState::On
            : hubfx::effects::landing::LandingLightState::Off;
        self->_setStateFn(self->_landing, s->id, state);
        SFX_LOG_INFO("[landing-activation] id=%u → %s (us=%u)",
                     (unsigned)s->id, v.b ? "DEPLOY" : "RETRACT",
                     (unsigned)v.us);
    }

    Slot       _slots[hubfx::effects::landing::kMaxLandingLights]{};
    uint8_t    _numSlots   = 0;
    void*      _landing    = nullptr;
    SetStateFn _setStateFn = nullptr;
};

}  // namespace hubfx::config

#endif  // HUBFX_LANDING_ACTIVATION_H
