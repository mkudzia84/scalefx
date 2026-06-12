/*
 * gear_activation.h — RC-channel up/down driver for the undercarriage.
 *
 *   When `/gearcontrol.yaml` carries an `input:` block, the sketch wires
 *   one of these.  It owns a single Boolean `TriggerInput`, subscribes it
 *   to the named RC channel (resolved against `/hubfx.yaml`'s `inputs[]`,
 *   Rule 43), and on every edge issues the SAME fleet command the wire
 *   GEAR_ALL takes — `GearControlService::commandAll()` — so one stick
 *   switch deploys/retracts the WHOLE set, honouring the configured
 *   coordination mode (independent / door-sync / full-sync / sequenced).
 *
 *   Channel semantics: above threshold = RETRACT (gear up), below =
 *   DEPLOY (gear down); `invert: true` flips that.  Failsafe always
 *   forces the DEPLOY side — a dropped RC link lowers the gear (the safe
 *   state for a landing aircraft), mirroring trigger_input.h's "gear
 *   deploys on RC loss" convention.
 *
 *   This is the gear twin of `LandingActivationDriver`: a stand-alone
 *   config-layer driver so `GearControlServicePolicyT` keeps its two
 *   template parameters and never gains an InputDispatcher dependency
 *   (the explicit design note in landing_activation.h).  The sketch
 *   re-installs it on every CONFIG_RELOAD.
 */

#ifndef HUBFX_GEAR_ACTIVATION_H
#define HUBFX_GEAR_ACTIVATION_H

#include <cstdint>
#include <cstring>

#include <serial/diag_log.h>
#include <serial/ports.h>                       // PortKind::getName

#include "../effects/input/trigger_input.h"     // TriggerInput / TriggerValue
#include "../effects/gearcontrol/gearcontrol_protocol.h"  // GearAllAction
#include "gearcontrol_config.h"                 // GearControlConfig / GearActivation
#include "hubfx_config.h"                       // HubFxConfig / findInputByName

namespace hubfx::config {

class GearActivationDriver {
public:
    GearActivationDriver() = default;

    /// (Re)wire the up/down channel.  Idempotent: drops the prior
    /// subscription first, so a CONFIG_RELOAD or a cleared `input:` block
    /// cleanly detaches the stale trigger.  An empty `input.name` leaves
    /// the gear manual-only (Studio / CLI / wire commands).
    template <typename TInputDispatcher, typename TGearService>
    void install(TInputDispatcher* dispatcher,
                 TGearService* gear,
                 const GearControlConfig& cfg,
                 const HubFxConfig& hub) {
        if (dispatcher) dispatcher->unsubscribe(&_trigger);
        _bound      = false;
        _gear       = nullptr;
        _commandFn  = nullptr;
        _invert     = false;

        if (!dispatcher || !gear) return;                 // dormant
        if (!cfg.enabled || !cfg.activation.input[0]) return;   // manual-only

        const auto* b = findInputByName(hub, cfg.activation.input);
        if (!b) {
            SFX_LOG_WARN("[gear-activation] input '%s' not in /hubfx.yaml inputs[] — gear stays manual",
                         cfg.activation.input);
            return;
        }

        _gear      = gear;
        _invert    = cfg.activation.invert;
        _commandFn = [](void* p, uint8_t action) {
            static_cast<TGearService*>(p)->commandAll(action);
        };

        using namespace hubfx::effects::input;
        TriggerMapping m;
        m.kind         = TriggerKind::Boolean;
        m.thresholdUs  = cfg.activation.thresholdUs;
        m.hysteresisUs = cfg.activation.hysteresisUs;
        // Failsafe forces the DEPLOY side: default mapping deploys LOW, so
        // ForceLow lowers the gear on RC loss; inverted deploys HIGH, so
        // ForceHigh does.  Either way a dropped link = gear down (safe).
        m.failsafe     = _invert ? FailsafeBehaviour::ForceHigh
                                 : FailsafeBehaviour::ForceLow;
        _trigger.configure(m, &GearActivationDriver::onChange, this);

        const uint8_t channel = (b->channelId > 0)
                              ? (uint8_t)(b->channelId - 1) : 0;
        if (dispatcher->subscribe(&_trigger, b->port, channel)) {
            _bound = true;
            SFX_LOG_INFO("[gear-activation] bound to {%s,%u} ch=%u thresh=%uus hyst=%uus invert=%d",
                         PortKind::getName(b->port.portKind),
                         (unsigned)b->port.portIdx, (unsigned)channel,
                         (unsigned)cfg.activation.thresholdUs,
                         (unsigned)cfg.activation.hysteresisUs,
                         _invert ? 1 : 0);
        } else {
            SFX_LOG_WARN("[gear-activation] dispatcher subscribe failed");
        }
    }

private:
    using CommandFn = void (*)(void* gearPtr, uint8_t action);

    static void onChange(void* ctx,
                         const hubfx::effects::input::TriggerValue& v) {
        if (!ctx || v.kind != hubfx::effects::input::TriggerKind::Boolean) return;
        auto* self = static_cast<GearActivationDriver*>(ctx);
        if (!self->_commandFn || !self->_gear) return;
        // Default: HIGH = retract (gear up), LOW = deploy; invert flips.
        // Failsafe (signal lost) resolves to the deploy side — act on it.
        const bool retract = self->_invert ? !v.b : v.b;
        const uint8_t action = retract
            ? hubfx::effects::gearctrl::GearAllAction::Retract
            : hubfx::effects::gearctrl::GearAllAction::Deploy;
        self->_commandFn(self->_gear, action);
        SFX_LOG_INFO("[gear-activation] → %s (us=%u%s)",
                     retract ? "RETRACT" : "DEPLOY", (unsigned)v.us,
                     v.valid ? "" : ", failsafe");
    }

    hubfx::effects::input::TriggerInput _trigger{};
    void*     _gear      = nullptr;
    CommandFn _commandFn = nullptr;
    bool      _invert    = false;
    bool      _bound     = false;
};

}  // namespace hubfx::config

#endif  // HUBFX_GEAR_ACTIVATION_H
