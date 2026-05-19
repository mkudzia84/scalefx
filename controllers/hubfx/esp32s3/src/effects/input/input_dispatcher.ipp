/*
 * input_dispatcher.ipp — frame decode + binding-table dispatch.
 */

#ifndef HUBFX_INPUT_DISPATCHER_IPP
#define HUBFX_INPUT_DISPATCHER_IPP

#include <serial/wire.h>           // SfxWire::getU16LE

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
            evtPortKind = PortKind::Input;     // RC PWM lives on an InputPort
            extract     = &extractRcPwm;
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

template <hubfx::topology::TopologyService TTopology>
void InputDispatcherServicePolicyT<TTopology>::roleEventTrampoline(
        void* ctx, const char* guid, uint8_t innerType,
        const uint8_t* p, size_t len) {
    auto* self = static_cast<InputDispatcherServicePolicyT*>(ctx);
    if (self) self->onRoleEvent(guid, innerType, p, len);
}

}  // namespace hubfx::effects::input

#endif  // HUBFX_INPUT_DISPATCHER_IPP
