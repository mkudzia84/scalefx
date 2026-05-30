/*
 * input_dispatcher.ipp — frame decode + binding-table dispatch.
 */

#ifndef HUBFX_INPUT_DISPATCHER_IPP
#define HUBFX_INPUT_DISPATCHER_IPP

#include <serial/wire.h>           // SfxWire::getU16LE
#include <platform/sfx_platform.h>   // SFX_MILLIS()

namespace hubfx::effects::input {

// ─── Lifecycle ──────────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology>
bool InputDispatcherServicePolicyT<TTopology>::begin(
        sfx_core::BoardServerBase* ctx) {
    _ctx = ctx;
    if (!_ctx) return false;
    _topo = ctx->template findPolicy<TTopology>();
    if (!_topo) {
        SFX_LOG_ERROR("[input] TopologyService not found in policy pack");
        return false;
    }
    _topo->onRoleEvent(&InputDispatcherServicePolicyT::roleEventTrampoline,
                       static_cast<void*>(this));
    SFX_LOG_INFO("[input] dispatcher ready (max %u bindings)",
                 (unsigned)kMaxBindings);
    return true;
}

// ─── Registration ───────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology>
bool InputDispatcherServicePolicyT<TTopology>::subscribe(
        TriggerInput* input, const PortRef& source, uint8_t channel) {
    if (!input) return false;
    // De-dupe: if `input` is already registered, overwrite its slot.
    for (uint8_t i = 0; i < kMaxBindings; ++i) {
        if (_bindings[i].occupied && _bindings[i].input == input) {
            _bindings[i].source  = source;
            _bindings[i].channel = channel;
            return true;
        }
    }
    for (uint8_t i = 0; i < kMaxBindings; ++i) {
        if (!_bindings[i].occupied) {
            _bindings[i] = { input, source, channel, true };
            if (i >= _numBindings) _numBindings = i + 1;
            return true;
        }
    }
    SFX_LOG_ERROR("[input] subscribe: binding table full (%u entries)",
                  (unsigned)kMaxBindings);
    return false;
}

template <hubfx::topology::TopologyService TTopology>
void InputDispatcherServicePolicyT<TTopology>::unsubscribe(TriggerInput* input) {
    if (!input) return;
    for (uint8_t i = 0; i < kMaxBindings; ++i) {
        if (_bindings[i].occupied && _bindings[i].input == input) {
            _bindings[i] = {};
        }
    }
}

// ─── Source-match helper ────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology>
bool InputDispatcherServicePolicyT<TTopology>::sourceMatches(
        const PortRef& bindSource,
        uint8_t evtPortKind, uint8_t evtPortIdx,
        const char* evtGuid) {
    if (bindSource.portKind != evtPortKind)  return false;
    if (bindSource.portIdx  != evtPortIdx)   return false;
    const bool bindLocal = (bindSource.guid[0] == 0);
    const bool evtLocal  = (!evtGuid || evtGuid[0] == 0);
    if (bindLocal && evtLocal) return true;
    if (bindLocal != evtLocal) return false;
    return std::strcmp(bindSource.guid, evtGuid) == 0;
}

// ─── Per-protocol channel extractors ────────────────────────────────

// RCIN_VALUE_BROADCAST: [portIdx:u8][us:u16LE][valid:u8] — single channel
template <hubfx::topology::TopologyService TTopology>
bool InputDispatcherServicePolicyT<TTopology>::extractRcPwm(
        const uint8_t* p, size_t len, uint8_t channel,
        uint16_t& outUs, bool& outValid) {
    if (channel != 0) return false;        // PPM port has only one channel
    if (len < 4) return false;
    outUs    = SfxWire::getU16LE(&p[1]);
    outValid = p[3] != 0;
    return true;
}

// SBUS_FRAME_BROADCAST: [portIdx:u8][count:u8][flags:u8][u16LE × count]
//   flags bits: 0=valid, 1=failsafe, 2=frameLost, 3=ch17, 4=ch18
//   channel 16 / 17 are the digital ch17 / ch18 bits.
template <hubfx::topology::TopologyService TTopology>
bool InputDispatcherServicePolicyT<TTopology>::extractSbus(
        const uint8_t* p, size_t len, uint8_t channel,
        uint16_t& outUs, bool& outValid) {
    if (len < 3) return false;
    const uint8_t count = p[1];
    const uint8_t flags = p[2];
    const bool    valid = (flags & 0x01) != 0 && (flags & 0x02) == 0;

    if (channel < count) {
        const size_t off = 3 + static_cast<size_t>(channel) * 2;
        if (off + 2 > len) return false;
        outUs    = SfxWire::getU16LE(&p[off]);
        outValid = valid;
        return true;
    }
    // Digital channels — modelled as 1000 µs (off) / 2000 µs (on).
    if (channel == 16) {
        outUs    = (flags & 0x08) ? 2000 : 1000;
        outValid = valid;
        return true;
    }
    if (channel == 17) {
        outUs    = (flags & 0x10) ? 2000 : 1000;
        outValid = valid;
        return true;
    }
    return false;
}

// JETIEX_FRAME_BROADCAST: [portIdx:u8][count:u8][valid:u8][u16LE × count]
template <hubfx::topology::TopologyService TTopology>
bool InputDispatcherServicePolicyT<TTopology>::extractJetiEx(
        const uint8_t* p, size_t len, uint8_t channel,
        uint16_t& outUs, bool& outValid) {
    if (len < 3) return false;
    const uint8_t count = p[1];
    const bool    valid = p[2] != 0;
    if (channel >= count) return false;
    const size_t off = 3 + static_cast<size_t>(channel) * 2;
    if (off + 2 > len) return false;
    outUs    = SfxWire::getU16LE(&p[off]);
    outValid = valid;
    return true;
}

// PPM_FRAME_BROADCAST: [portIdx:u8][count:u8][valid:u8][u16LE × count].
// Same layout as Jeti EX — the pulse-capture role now emits a full
// multi-channel frame so an effect can bind to any PPM channel by name.
template <hubfx::topology::TopologyService TTopology>
bool InputDispatcherServicePolicyT<TTopology>::extractPpm(
        const uint8_t* p, size_t len, uint8_t channel,
        uint16_t& outUs, bool& outValid) {
    if (len < 3) return false;
    const uint8_t count = p[1];
    const bool    valid = p[2] != 0;
    if (channel >= count) return false;
    const size_t off = 3 + static_cast<size_t>(channel) * 2;
    if (off + 2 > len) return false;
    outUs    = SfxWire::getU16LE(&p[off]);
    outValid = valid;
    return true;
}

// ─── Dispatch ───────────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology>
void InputDispatcherServicePolicyT<TTopology>::onRoleEvent(
        const char* guid, uint8_t innerType,
        const uint8_t* p, size_t len) {
    // Filter to the three frame-bearing types this service decodes.
    uint8_t evtPortKind = 0;
    using   ExtractFn   = bool (*)(const uint8_t*, size_t, uint8_t,
                                   uint16_t&, bool&);
    ExtractFn extract = nullptr;

    switch (innerType) {
        case RolePacket::RCIN_VALUE_BROADCAST:
            evtPortKind = PortKind::Input;     // legacy single-channel RC PWM
            extract     = &extractRcPwm;
            break;
        case RolePacket::PPM_FRAME_BROADCAST:
            evtPortKind = PortKind::Input;     // PPM multi-channel frame
            extract     = &extractPpm;
            break;
        case RolePacket::SBUS_FRAME_BROADCAST:
            evtPortKind = PortKind::Input;
            extract     = &extractSbus;
            break;
        case RolePacket::JETIEX_FRAME_BROADCAST:
            evtPortKind = PortKind::Input;
            extract     = &extractJetiEx;
            break;
        default:
            return;     // not an input event
    }

    if (len < 1) return;

    const uint8_t evtPortIdx = p[0];

#if SFX_INSTRUMENTATION
    // Rate-limited dispatch health: confirms onRoleEvent fires (local path OK)
    // and shows WHY effects may not move — routing off, no subscriptions, or
    // source mismatch (binds>0 but match=0 = the bound PortRef's GUID/port
    // doesn't match the frame's, e.g. hub-GUID vs "" trap).
    {
        static uint32_t lastLog = 0;
        const uint32_t nowMs = SFX_MILLIS();
        if (nowMs - lastLog >= 2000) {
            uint8_t occ = 0, matched = 0;
            for (uint8_t i = 0; i < kMaxBindings; ++i) {
                if (!_bindings[i].occupied) continue;
                occ++;
                if (sourceMatches(_bindings[i].source, evtPortKind, evtPortIdx, guid)) matched++;
            }
            // Only log when there's a binding to diagnose — with nothing bound
            // (occ==0) the dispatch is a no-op and a per-frame line is pure
            // spam (the "[disp] evt … binds=0 match=0" flood).
            if (occ > 0) {
                lastLog = nowMs;
                SFX_LOG_INFO("[disp] evt type=0x%02X port=%u routing=%d binds=%u match=%u",
                             innerType, evtPortIdx, _routingEnabled ? 1 : 0, occ, matched);
            }
        }
    }
#endif

    // Global RC-routing gate (protocol-agnostic — we're past the
    // per-protocol extractor selection).  When off, RC stops driving
    // effects; the wire broadcast that monitors read already went out
    // separately, so live channel bars keep updating.
    if (!_routingEnabled) return;

    for (uint8_t i = 0; i < kMaxBindings; ++i) {
        Binding& b = _bindings[i];
        if (!b.occupied) continue;
        if (!sourceMatches(b.source, evtPortKind, evtPortIdx, guid)) continue;

        uint16_t us = 0;
        bool     v  = false;
        if (!extract(p, len, b.channel, us, v)) continue;
        b.input->feed(us, v);
    }
}

// ─── Routing-gate command handler ───────────────────────────────────

template <hubfx::topology::TopologyService TTopology>
CommandHandleResult InputDispatcherServicePolicyT<TTopology>::handle(
        uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case InputRoutingPacket::INPUT_ROUTING_SET_ENABLED: {
            if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); break; }
            _routingEnabled = (payload[0] != 0);
            SFX_LOG_INFO("[input] RC routing %s", _routingEnabled ? "ENABLED" : "DISABLED (manual)");
            _ctx->sendAck();
            break;
        }
        case InputRoutingPacket::INPUT_ROUTING_GET_REQ: {
            const uint8_t out = _routingEnabled ? 1 : 0;
            _ctx->sendRawPacket(InputRoutingPacket::INPUT_ROUTING_RESP,
                                _ctx->currentTag(), &out, 1);
            break;
        }
        default:
            return CommandHandleResult::NotMyCommand;
    }
    return CommandHandleResult::Handled;
}

template <hubfx::topology::TopologyService TTopology>
void InputDispatcherServicePolicyT<TTopology>::roleEventTrampoline(
        void* ctx, const char* guid, uint8_t innerType,
        const uint8_t* p, size_t len) {
    auto* self = static_cast<InputDispatcherServicePolicyT*>(ctx);
    if (self) self->onRoleEvent(guid, innerType, p, len);
}

}  // namespace hubfx::effects::input

#endif  // HUBFX_INPUT_DISPATCHER_IPP
