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

    // Connection-loss tracking (item 5) — stamp this source's liveness on every
    // frame, BEFORE the routing gate, so loss is detected even when RC routing
    // is off (effects holding) and even when the channels are invalid: a frame
    // ARRIVING at all = the link is up (a noisy line that still frames is "up";
    // a dead UART stops framing → that's the loss we catch).
    touchLink(guid, evtPortKind, evtPortIdx, SFX_MILLIS());

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
                SFX_LOG_DEBUG("[disp] evt type=0x%02X port=%u routing=%d binds=%u match=%u",
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
        case TelemetryPacket::TELEMETRY_GET_REQ: {
            // Snapshot the telemetry collection (item 4).  Bounded buffer; the
            // realistic collection (HubFx-own + one ESC, a handful of sensors)
            // is well under 1 KB — truncate gracefully if it ever isn't.
            uint8_t buf[1024];
            const size_t n = buildTelemetrySnapshot(buf, sizeof buf);
            _ctx->sendRawPacket(TelemetryPacket::TELEMETRY_RESP,
                                _ctx->currentTag(), buf, n);
            break;
        }
        case ConnectionPacket::CONNECTION_SET_CFG: {
            if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); break; }
            _linkLossMs = (uint16_t)(payload[0] | (payload[1] << 8));
            SFX_LOG_INFO("[link] link-loss interval = %u ms%s",
                         (unsigned)_linkLossMs, _linkLossMs ? "" : " (loss signalling OFF)");
            _ctx->sendAck();
            break;
        }
        case ConnectionPacket::CONNECTION_GET_REQ: {
            uint8_t buf[8 + kMaxLinks * 24];
            size_t w = 0;
            buf[w++] = (uint8_t)_linkLossMs;
            buf[w++] = (uint8_t)(_linkLossMs >> 8);
            const size_t cntPos = w++;             // count placeholder
            uint8_t n = 0;
            const uint32_t now = SFX_MILLIS();
            for (uint8_t i = 0; i < kMaxLinks; ++i) {
                const Link& l = _links[i];
                if (!l.occupied) continue;
                uint8_t gl = 0; while (l.guid[gl] && gl < 8) ++gl;
                buf[w++] = l.portKind;
                buf[w++] = l.portIdx;
                buf[w++] = gl;
                for (uint8_t k = 0; k < gl; ++k) buf[w++] = (uint8_t)l.guid[k];
                buf[w++] = l.state;
                buf[w++] = (uint8_t)l.brownouts;
                buf[w++] = (uint8_t)(l.brownouts >> 8);
                const uint32_t sil = now - l.lastMs;
                const uint16_t silc = sil > 0xFFFF ? 0xFFFF : (uint16_t)sil;
                buf[w++] = (uint8_t)silc;
                buf[w++] = (uint8_t)(silc >> 8);
                ++n;
            }
            buf[cntPos] = n;
            _ctx->sendRawPacket(ConnectionPacket::CONNECTION_RESP,
                                _ctx->currentTag(), buf, w);
            break;
        }
        default:
            return CommandHandleResult::NotMyCommand;
    }
    return CommandHandleResult::Handled;
}

// ─── Connection-loss state machine (item 5) ─────────────────────────

template <hubfx::topology::TopologyService TTopology>
void InputDispatcherServicePolicyT<TTopology>::touchLink(
        const char* guid, uint8_t portKind, uint8_t portIdx, uint32_t nowMs) {
    const char* g = guid ? guid : "";
    Link* slot = nullptr;
    for (uint8_t i = 0; i < kMaxLinks; ++i) {
        Link& l = _links[i];
        if (l.occupied && l.portKind == portKind && l.portIdx == portIdx &&
            std::strcmp(l.guid, g) == 0) { slot = &l; break; }
        if (!slot && !l.occupied) slot = &l;     // remember first free
    }
    if (!slot) return;                            // table full — ignore extra sources
    if (!slot->occupied) {
        slot->occupied = true;
        slot->portKind = portKind;
        slot->portIdx  = portIdx;
        uint8_t n = 0; for (; g[n] && n < 8; ++n) slot->guid[n] = g[n];
        slot->guid[n]  = '\0';
        slot->state    = 0;
    }
    const bool wasDown = (slot->state != 0);
    slot->lastMs = nowMs;
    if (wasDown) {                                // a frame after silence = recovery
        slot->state = 0;
        SFX_LOG_INFO("[link] %s port=%u RECOVERED", slot->guid[0] ? slot->guid : "hub", portIdx);
        fireConnection(*slot);
    }
}

template <hubfx::topology::TopologyService TTopology>
void InputDispatcherServicePolicyT<TTopology>::forgetLinksForGuid(const char* guid) {
    if (!guid || !guid[0]) return;                // hub-local links are never reclaimed
    for (uint8_t i = 0; i < kMaxLinks; ++i) {
        Link& l = _links[i];
        if (l.occupied && std::strcmp(l.guid, guid) == 0) {
            SFX_LOG_INFO("[link] %s port=%u FORGOTTEN (expander unmounted)", l.guid, l.portIdx);
            l = Link{};                            // release the slot for reuse
        }
    }
}

template <hubfx::topology::TopologyService TTopology>
void InputDispatcherServicePolicyT<TTopology>::update() {
    const uint32_t now = SFX_MILLIS();
    for (uint8_t i = 0; i < kMaxLinks; ++i) {
        Link& l = _links[i];
        if (!l.occupied) continue;
        const uint32_t silence = now - l.lastMs;
        // Stage 1: signal lost — hold outputs (effects already hold their last
        // value), count the brownout, (Jeti reset hook would fire here).
        if (l.state == 0 && silence > kDetectMs) {
            l.state = 1;
            ++l.brownouts;
            SFX_LOG_WARN("[link] %s port=%u SIGNAL LOST (brownout #%u) — holding outputs",
                         l.guid[0] ? l.guid : "hub", l.portIdx, (unsigned)l.brownouts);
            fireConnection(l);
        }
        // Stage 2: loss confirmed past the configurable interval → DOWN signal
        // (the actionable event effects react to).  _linkLossMs == 0 disables it.
        else if (l.state == 1 && _linkLossMs != 0 && silence > _linkLossMs) {
            l.state = 2;
            SFX_LOG_ERROR("[link] %s port=%u CONNECTION DOWN (%u ms) — emitting loss signal",
                          l.guid[0] ? l.guid : "hub", l.portIdx, (unsigned)silence);
            fireConnection(l);
        }
    }
}

template <hubfx::topology::TopologyService TTopology>
void InputDispatcherServicePolicyT<TTopology>::fireConnection(const Link& l) {
    // In-firmware subscriber (GearControl emergency deploy, item 6).
    if (_connCb) _connCb(_connCtx, l.guid, l.portKind, l.portIdx, l.state);
    // Async wire event for the GUI / CLI subscribe stream.
    uint8_t guidLen = 0; while (l.guid[guidLen] && guidLen < 8) ++guidLen;
    uint8_t buf[16];
    size_t w = 0;
    buf[w++] = l.portKind;
    buf[w++] = l.portIdx;
    buf[w++] = guidLen;
    for (uint8_t k = 0; k < guidLen; ++k) buf[w++] = (uint8_t)l.guid[k];
    buf[w++] = l.state;
    buf[w++] = (uint8_t)l.brownouts;
    buf[w++] = (uint8_t)(l.brownouts >> 8);
    _ctx->sendRawPacket(ConnectionPacket::CONNECTION_EVENT, SfxWire::TAG_ASYNC, buf, w);
}

// ─── Telemetry-collection serializer (item 4) ───────────────────────

template <hubfx::topology::TopologyService TTopology>
size_t InputDispatcherServicePolicyT<TTopology>::buildTelemetrySnapshot(
        uint8_t* buf, size_t cap) const {
    using Hub = sfx_telemetry::TelemetryHub;
    Hub& hub = Hub::instance();

    // Helpers — bounds-checked little-endian writers.  `w` is the cursor;
    // every writer no-ops once the buffer is full so we never overrun.
    size_t w = 0;
    auto putU8  = [&](uint8_t v)  { if (w + 1 <= cap) buf[w] = v; w += 1; };
    auto putU16 = [&](uint16_t v) { if (w + 2 <= cap) { buf[w]=(uint8_t)v; buf[w+1]=(uint8_t)(v>>8); } w += 2; };
    auto putI32 = [&](int32_t v)  { uint32_t u=(uint32_t)v; if (w+4<=cap){ buf[w]=(uint8_t)u; buf[w+1]=(uint8_t)(u>>8); buf[w+2]=(uint8_t)(u>>16); buf[w+3]=(uint8_t)(u>>24);} w += 4; };
    auto putStr = [&](const char* s) {
        uint8_t n = 0; while (s && s[n] && n < 31) ++n;     // cap label/name length
        putU8(n);
        for (uint8_t i = 0; i < n; ++i) putU8((uint8_t)s[i]);
    };

    Hub::ScopedLock lk(hub);

    const uint8_t devN  = hub.deviceCount();
    const uint8_t sensN = hub.activeSensorCount();

    // Header.
    putU16(targetPubIntervalMs(sensN));                    // pubIntervalMs
    const uint16_t hz_x10 = (uint16_t)(10000u / (targetPubIntervalMs(sensN) ? targetPubIntervalMs(sensN) : 1));
    putU16(hz_x10);                                        // respHz × 10
    putU8(sensN);                                          // activeSensors
    const size_t devCountPos = w;                          // patched after the loop
    putU8(0);                                              // deviceCount placeholder

    uint8_t emitted = 0;
    for (uint8_t i = 0; i < devN; ++i) {
        const auto* d = hub.device(i);
        if (!d) continue;
        // Reserve a rough worst-case for this device's fixed header so a
        // half-written device never lands in the buffer.
        if (w + 8 + 32 > cap) break;
        putU16(d->usn);
        putU16(d->lsn);
        const uint8_t dflags = (uint8_t)((d->local ? 0x01 : 0) | (d->active ? 0x02 : 0));
        putU8(dflags);
        const size_t senCountPos = w;
        putU8(0);                                          // sensorCount placeholder
        putStr(d->name);

        uint8_t sEmitted = 0;
        for (uint8_t j = 0; j < d->sensorCount; ++j) {
            const auto& s = d->sensors[j];
            // worst-case sensor entry ≈ 8 + 32 + 32; stop if it won't fit.
            if (w + 8 + 64 > cap) break;
            putU8(s.id);
            putU8((uint8_t)s.kind);
            putU8(s.decimals);
            putU8((uint8_t)(s.active ? 0x01 : 0));
            putI32(s.value);
            putStr(s.label);
            putStr(s.unit);
            ++sEmitted;
        }
        if (senCountPos < cap) buf[senCountPos] = sEmitted;
        ++emitted;
    }
    if (devCountPos < cap) buf[devCountPos] = emitted;

    return (w <= cap) ? w : cap;
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
