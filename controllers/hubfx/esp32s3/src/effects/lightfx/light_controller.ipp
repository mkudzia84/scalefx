/*
 * light_controller.ipp — template-method definitions.
 *
 *   Included at the bottom of `light_controller.h`.
 */

#ifndef HUBFX_LIGHT_CONTROLLER_IPP
#define HUBFX_LIGHT_CONTROLLER_IPP

namespace hubfx::effects::lightfx {

// ─── Program switch — diff algorithm ────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
bool LightControllerT<TTopology, TLandingService>::selectProgram(uint8_t idx) {
    if (idx >= _numPrograms) {
        SFX_LOG_WARN("[lightfx] selectProgram: unknown idx %u", idx);
        return false;
    }
    const Program& neu = _programs[idx];

    // Entire program switch goes out as one wire batch so partial
    // states don't show up between the LED_STOP burst and the
    // LED_QUEUE_LOAD / START burst.
    if (_topo) _topo->beginBatch();

    // ── LED channels: stop anything in the OLD program not in the NEW.
    if (_activeIdx >= 0 && _activeIdx < (int8_t)_numPrograms) {
        const Program& old = _programs[_activeIdx];
        for (uint8_t i = 0; i < old.numChannels; ++i) {
            if (!programHasChannel(neu, old.channels[i].addr)) {
                stopChannel(old.channels[i].addr);
            }
        }
    }

    // ── LED channels: (re)apply every channel in the NEW program.
    //   We re-apply unconditionally so brightness / queue changes in
    //   the new program land even when the same port was in the old.
    for (uint8_t i = 0; i < neu.numChannels; ++i) {
        applyChannel(neu.channels[i]);
    }

    // ── Landing lights: any that are in OLD but not NEW go OFF.
    if (_activeIdx >= 0 && _activeIdx < (int8_t)_numPrograms) {
        const Program& old = _programs[_activeIdx];
        for (uint8_t i = 0; i < old.numLandings; ++i) {
            if (!programHasLanding(neu, old.landings[i].id)) {
                if (_ll) _ll->setState(old.landings[i].id,
                                       hubfx::effects::landing::LandingLightState::Off,
                                       EffectId::LightFx);
            }
        }
    }
    // ── Landing lights in NEW: set to requested state.
    applyLandings(neu.landings, neu.numLandings);

    _activeIdx = (int8_t)idx;
    SFX_LOG_INFO("[lightfx] program %u (%s) active", idx, neu.name);

    // Flush the batch — every queued LED / servo / landing-light
    // command goes out in one back-to-back burst here.
    if (_topo) _topo->commitBatch();
    return true;
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void LightControllerT<TTopology, TLandingService>::reset() {
    if (_activeIdx < 0 || _activeIdx >= (int8_t)_numPrograms) {
        _activeIdx = -1;
        return;
    }
    if (_topo) _topo->beginBatch();
    const Program& old = _programs[_activeIdx];
    for (uint8_t i = 0; i < old.numChannels; ++i) {
        stopChannel(old.channels[i].addr);
    }
    for (uint8_t i = 0; i < old.numLandings; ++i) {
        if (_ll) _ll->setState(old.landings[i].id,
                               hubfx::effects::landing::LandingLightState::Off,
                               EffectId::LightFx);
    }
    _activeIdx = -1;
    SFX_LOG_INFO("[lightfx] program reset — no active program");
    if (_topo) _topo->commitBatch();
}

// ─── Master brightness ──────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void LightControllerT<TTopology, TLandingService>::setMasterBrightness(
        uint8_t pct) {
    if (pct > 100) pct = 100;
    _masterBrightnessPct = pct;

    // Re-broadcast LED_SET_BRIGHTNESS for the active program's channels
    // so the change is visible immediately.  Batched so the visual
    // brightness step is atomic across all channels.
    if (!_topo || _activeIdx < 0 || _activeIdx >= (int8_t)_numPrograms) return;
    _topo->beginBatch();
    const Program& p = _programs[_activeIdx];
    for (uint8_t i = 0; i < p.numChannels; ++i) {
        const PortRef& addr = p.channels[i].addr;
        const uint8_t bright[2] = {
            addr.portIdx,
            scaledBrightness(p.channels[i].perChannelBrightnessPct),
        };
        _topo->sendRoleCommand(addr,
                               RolePacket::LED_SET_BRIGHTNESS,
                               bright, sizeof(bright));
    }
    _topo->commitBatch();
    SFX_LOG_INFO("[lightfx] master brightness → %u%%", (unsigned)pct);
}

// ─── Internal helpers ───────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void LightControllerT<TTopology, TLandingService>::applyChannel(
        const LedChannelSpec& ch) {
    if (!_topo) return;

    // Skip channels we don't own — these were either claimed by a
    // different effect family (logged at claim time) or never claimed
    // because the target was offline.
    if (_topo->ownerOf(ch.addr) != EffectId::LightFx) {
        SFX_LOG_DEBUG("[lightfx] skip channel %s:%u — not owned",
                      ch.addr.guid[0] ? ch.addr.guid : "hub",
                      (unsigned)ch.addr.portIdx);
        return;
    }

    // ── 1. LED_QUEUE_LOAD ──
    uint8_t queue[2 + kMaxEventsPerChannel * kEventWireSize];
    const size_t qlen = serializeQueueLoad(ch.addr.portIdx,
                                           ch.events, ch.numEvents,
                                           queue, sizeof(queue));
    if (qlen == 0) {
        SFX_LOG_WARN("[lightfx] queue serialize failed for %s:%u",
                     ch.addr.guid[0] ? ch.addr.guid : "hub",
                     (unsigned)ch.addr.portIdx);
        return;
    }
    // DEBUG: log the first event's wire bytes — handy when something
    // upstream re-serializes / changes the layout silently.
    SFX_LOG_DEBUG("[lightfx] LED_QUEUE_LOAD %s:%u  qlen=%u  ev0={kind=%u dur=%u cycle=%u bright=%u}",
                  ch.addr.guid[0] ? ch.addr.guid : "hub",
                  (unsigned)ch.addr.portIdx, (unsigned)qlen,
                  ch.numEvents ? (unsigned)ch.events[0].kind : 0u,
                  ch.numEvents ? (unsigned)ch.events[0].durationMs : 0u,
                  ch.numEvents ? (unsigned)ch.events[0].cycleMs : 0u,
                  ch.numEvents ? (unsigned)ch.events[0].brightnessPct : 0u);
    if (!_topo->sendRoleCommand(ch.addr,
                                RolePacket::LED_QUEUE_LOAD,
                                queue, qlen)) {
        SFX_LOG_WARN("[lightfx] LED_QUEUE_LOAD %s:%u failed",
                     ch.addr.guid[0] ? ch.addr.guid : "hub",
                     (unsigned)ch.addr.portIdx);
        return;
    }

    // ── 2. LED_SET_BRIGHTNESS (scaled by master) ──
    const uint8_t bright[2] = {
        ch.addr.portIdx,
        scaledBrightness(ch.perChannelBrightnessPct),
    };
    _topo->sendRoleCommand(ch.addr,
                           RolePacket::LED_SET_BRIGHTNESS,
                           bright, sizeof(bright));

    // ── 3. LED_START ──
    const uint8_t start[1] = { ch.addr.portIdx };
    _topo->sendRoleCommand(ch.addr,
                           RolePacket::LED_START,
                           start, sizeof(start));
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void LightControllerT<TTopology, TLandingService>::stopChannel(
        const PortRef& addr) {
    if (!_topo) return;
    const uint8_t stop[1]   = { addr.portIdx };
    const uint8_t bright[2] = { addr.portIdx, 0 };
    _topo->sendRoleCommand(addr, RolePacket::LED_STOP,
                           stop, sizeof(stop));
    _topo->sendRoleCommand(addr, RolePacket::LED_SET_BRIGHTNESS,
                           bright, sizeof(bright));
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void LightControllerT<TTopology, TLandingService>::applyLandings(
        const LandingLightBinding* defs, uint8_t n) {
    if (!_ll) return;
    for (uint8_t i = 0; i < n; ++i) {
        _ll->setState(defs[i].id, defs[i].state, EffectId::LightFx);
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
uint8_t LightControllerT<TTopology, TLandingService>::scaledBrightness(
        uint8_t channelPct) const {
    return static_cast<uint8_t>(
        (static_cast<uint16_t>(channelPct) *
         static_cast<uint16_t>(_masterBrightnessPct)) / 100);
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
bool LightControllerT<TTopology, TLandingService>::programHasChannel(
        const Program& prog, const PortRef& addr) {
    for (uint8_t i = 0; i < prog.numChannels; ++i) {
        if (prog.channels[i].addr.equals(addr)) return true;
    }
    return false;
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
bool LightControllerT<TTopology, TLandingService>::programHasLanding(
        const Program& prog, uint8_t id) {
    for (uint8_t i = 0; i < prog.numLandings; ++i) {
        if (prog.landings[i].id == id) return true;
    }
    return false;
}

}  // namespace hubfx::effects::lightfx

#endif  // HUBFX_LIGHT_CONTROLLER_IPP
