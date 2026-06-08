/*
 * TopologyServicePolicyT — template-method definitions.
 *
 *   Included at the bottom of topology_service.h.
 */

#ifndef HUBFX_TOPOLOGY_SERVICE_IPP
#define HUBFX_TOPOLOGY_SERVICE_IPP

namespace hubfx::topology {

// ─── Top-level dispatch ─────────────────────────────────────────────────

template <hubfx::expanders::ExpanderService TExpander>
CommandHandleResult TopologyServicePolicyT<TExpander>::handle(
        uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case TopologyPacket::TOPOLOGY_PORT_LIST_REQ:
            handlePortListReq(payload, len);
            return CommandHandleResult::Handled;
        case TopologyPacket::TOPOLOGY_ROLE_LIST_REQ:
            handleRoleListReq(payload, len);
            return CommandHandleResult::Handled;
        case TopologyPacket::TOPOLOGY_ROLE_ATTACH:
            handleRoleAttach(payload, len);
            return CommandHandleResult::Handled;
        case TopologyPacket::TOPOLOGY_ROLE_DETACH:
            handleRoleDetach(payload, len);
            return CommandHandleResult::Handled;
        case TopologyPacket::TOPOLOGY_ROLE_FORWARD:
            handleRoleForward(payload, len);
            return CommandHandleResult::Handled;
        case TopologyPacket::TOPOLOGY_ROLE_QUERY:
            handleRoleQuery(payload, len);
            return CommandHandleResult::Handled;
        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ─── TOPOLOGY_ROLE_FORWARD — generic pass-through router ───────────────
//
// Envelope:  [guidLen:u8][guid:str][innerType:u8][innerLen:u16LE][inner:N]
//
// `sendRoleCommand` already implements the dispatch policy (capture-mode
// local handle vs. CDC forward to expander) — the wire handler is just
// the protocol-layer adapter that decodes the envelope, builds a
// `PortRef{guid, kind=0, idx=0}` (kind/idx are unused by sendRoleCommand
// for routing — only `guid` matters), and forwards the inner packet.
template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::handleRoleForward(
        const uint8_t* p, size_t len) {
    char   guid[5] = {};
    size_t off     = 0;
    if (!readGuidPrefix(p, len, guid, off)) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    if (off + 1 + 2 > len) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    const uint8_t  innerType = p[off++];
    const uint16_t innerLen  =
        static_cast<uint16_t>(p[off]) | (static_cast<uint16_t>(p[off + 1]) << 8);
    off += 2;
    if (off + innerLen > len) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    PortRef addr{};
    std::strncpy(addr.guid, guid, sizeof(addr.guid) - 1);
    addr.portKind = 0;        // not used by sendRoleCommand for routing
    addr.portIdx  = 0;
    const bool ok = sendRoleCommand(addr, innerType,
                                    innerLen > 0 ? &p[off] : nullptr, innerLen);
    if (ok) {
        _ctx->sendAck();
    } else {
        // sendRoleCommand already logged the specific failure
        // (unknown GUID, expander not ready, role NACK, CDC timeout).
        // The wire-side NACK carries the FORWARD_FAILED bucket
        // because we don't have visibility into the role-layer error
        // code from this layer — Studio shows the toast as "Cross-
        // board forward failed; check the hub diag log for details".
        _ctx->sendNack(TopologyError::FORWARD_FAILED);
    }
}

// ─── TOPOLOGY_ROLE_QUERY — generic request-response router ─────────────
// Envelope: [guidLen][guid][reqType][reqLen:u16LE][req].  Routes the inner
// *_GET_*_REQ to a GUID (local capture or remote forward), re-wraps the typed
// RESP as TOPOLOGY_ROLE_RESPONSE.  Role-agnostic — the inner packet is opaque,
// so any present/future role query works with zero changes here.
template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::handleRoleQuery(
        const uint8_t* p, size_t len) {
    char guid[5] = {}; size_t off = 0;
    if (!readGuidPrefix(p, len, guid, off)) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    if (off + 1 + 2 > len) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t  reqType = p[off++];
    const uint16_t reqLen  = (uint16_t)p[off] | ((uint16_t)p[off + 1] << 8); off += 2;
    if (off + reqLen > len) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t* req = reqLen ? &p[off] : nullptr;

    uint8_t respType = 0;
    uint8_t respBuf[sfx_core::BoardServerBase::kCaptureRespMax];
    size_t  respLen = 0;
    if (!queryRole(guid, reqType, req, reqLen, respType, respBuf, sizeof respBuf, respLen)) {
        _ctx->sendNack(TopologyError::FORWARD_FAILED);
        return;
    }
    // Re-wrap: [guidLen][guid][respType][respLen:u16LE][resp]
    uint8_t out[1 + 5 + 1 + 2 + sfx_core::BoardServerBase::kCaptureRespMax];
    size_t  o = 0;
    const uint8_t glen = (uint8_t)std::strlen(guid);
    out[o++] = glen; std::memcpy(&out[o], guid, glen); o += glen;
    out[o++] = respType;
    out[o++] = (uint8_t)(respLen & 0xFF);
    out[o++] = (uint8_t)((respLen >> 8) & 0xFF);
    if (respLen) { std::memcpy(&out[o], respBuf, respLen); o += respLen; }
    _ctx->sendRawPacket(TopologyPacket::TOPOLOGY_ROLE_RESPONSE, _ctx->currentTag(), out, o);
}

template <hubfx::expanders::ExpanderService TExpander>
bool TopologyServicePolicyT<TExpander>::queryRole(
        const char* guid, uint8_t reqType, const uint8_t* req, size_t reqLen,
        uint8_t& respType, uint8_t* respBuf, size_t respMax, size_t& respLen) {
    if (isLocalTarget(guid)) {
        // Local: run the role handler in capture mode; its typed RESP is
        // snapshotted (captureRawIfNeeded) instead of hitting the wire.
        _ctx->beginCapture();
        _roleSvc->handle(reqType, req, reqLen);
        const bool got = _ctx->capturedResp();
        if (got) {
            respType = _ctx->capturedRespType();
            respLen  = _ctx->capturedRespLen();
            if (respLen > respMax) respLen = respMax;
            std::memcpy(respBuf, _ctx->capturedRespData(), respLen);
        }
        _ctx->endCapture();
        return got;
    }
    const int slotIdx = slotIdxByGuid(guid);
    if (slotIdx < 0) return false;
    auto* slot = _exp->liveSlot((uint8_t)slotIdx);
    if (!slot || slot->entry.spec.collision ||
        slot->handshake != TExpander::Handshake::Ready) {
        return false;
    }
    return forwardQuery((uint8_t)slotIdx, reqType, req, reqLen,
                        respType, respBuf, respMax, respLen);
}

template <hubfx::expanders::ExpanderService TExpander>
bool TopologyServicePolicyT<TExpander>::forwardQuery(
        uint8_t slotIdx, uint8_t reqType, const uint8_t* req, size_t reqLen,
        uint8_t& respType, uint8_t* respBuf, size_t respMax, size_t& respLen) {
    auto* slot = _exp->liveSlot(slotIdx);
    if (!slot) return false;
    SerialPacket resp;
    const CommandResult rc =
        slot->client.sendQueryAnyBlocking(reqType, req, reqLen, resp);
    if (!rc.success || resp.type == 0 || resp.payload == nullptr) {
        SFX_LOG_DEBUG("[topology] forwardQuery 0x%02X slot %u failed", reqType, slotIdx);
        return false;
    }
    respType = resp.type;
    respLen  = (resp.len > respMax) ? respMax : resp.len;
    std::memcpy(respBuf, resp.payload, respLen);
    return true;
}

// ─── GUID prefix helpers ────────────────────────────────────────────────

template <hubfx::expanders::ExpanderService TExpander>
bool TopologyServicePolicyT<TExpander>::readGuidPrefix(
        const uint8_t* p, size_t len,
        char outGuid[5], size_t& outOff) {
    outGuid[0] = 0;
    outOff     = 0;
    if (len < 1) return false;
    uint8_t glen = p[0];
    if (glen > 4)              return false;
    if (1 + (size_t)glen > len) return false;
    if (glen > 0) {
        std::memcpy(outGuid, &p[1], glen);
    }
    outGuid[glen] = 0;
    outOff = 1 + glen;
    return true;
}

template <hubfx::expanders::ExpanderService TExpander>
bool TopologyServicePolicyT<TExpander>::isLocalTarget(const char* guid) const {
    if (!guid || !guid[0]) return true;
    if (!_ctx || !_ctx->deviceName()) return false;
    const char* dash = std::strrchr(_ctx->deviceName(), '-');
    if (!dash || !dash[1]) return false;
    return std::strncmp(dash + 1, guid, 4) == 0;
}

template <hubfx::expanders::ExpanderService TExpander>
int TopologyServicePolicyT<TExpander>::slotIdxByGuid(const char* guid) const {
    if (!_exp) return -1;
    uint8_t idx = _exp->findLiveIdxByGuid(guid);
    return (idx == 0xFF) ? -1 : (int)idx;
}

// ─── TOPOLOGY_PORT_LIST_REQ ────────────────────────────────────────────

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::handlePortListReq(
        const uint8_t* p, size_t len) {
    char   guid[5] = {0};
    size_t off     = 0;
    if (!readGuidPrefix(p, len, guid, off)) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    // Reasonable upper bound: hub block ~150 bytes, per-expander block
    // ~150 bytes.  At MaxExpanders=2 + hub = 3 boards → ~600 bytes max.
    uint8_t buf[768];
    size_t  bufOff   = 1;        // reserve [boardCount]
    uint8_t boardCnt = 0;

    if (guid[0] == 0 || isLocalTarget(guid)) {
        appendHubPortBlock(buf, bufOff, sizeof buf);
        ++boardCnt;
    }
    if (guid[0] == 0) {
        // "all boards" — include every live, identified, non-collision
        // expander.  Slots in the middle of post-IDENTIFY harvest are
        // included with whatever ports they've cached so far.
        for (uint8_t i = 0; i < TExpander::kMaxExpanders; ++i) {
            auto* slot = _exp->liveSlot(i);
            if (!slot) continue;
            const auto& e = slot->entry;
            if (!e.connected || !e.spec.valid || e.spec.collision) continue;
            appendExpanderPortBlock(buf, bufOff, sizeof buf, i);
            ++boardCnt;
        }
    } else if (!isLocalTarget(guid)) {
        int idx = slotIdxByGuid(guid);
        if (idx < 0) {
            _ctx->sendNack(TopologyError::UNKNOWN_GUID);
            return;
        }
        appendExpanderPortBlock(buf, bufOff, sizeof buf, (uint8_t)idx);
        ++boardCnt;
    }
    buf[0] = boardCnt;
    _ctx->sendRawPacket(TopologyPacket::TOPOLOGY_PORT_LIST_RESP,
                        _ctx->currentTag(), buf, bufOff);
}

// ─── TOPOLOGY_ROLE_LIST_REQ ────────────────────────────────────────────

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::handleRoleListReq(
        const uint8_t* p, size_t len) {
    char   guid[5] = {0};
    size_t off     = 0;
    if (!readGuidPrefix(p, len, guid, off)) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    uint8_t buf[768];
    size_t  bufOff   = 1;
    uint8_t boardCnt = 0;

    if (guid[0] == 0 || isLocalTarget(guid)) {
        appendHubRoleBlock(buf, bufOff, sizeof buf);
        ++boardCnt;
    }
    if (guid[0] == 0) {
        for (uint8_t i = 0; i < TExpander::kMaxExpanders; ++i) {
            auto* slot = _exp->liveSlot(i);
            if (!slot) continue;
            const auto& e = slot->entry;
            if (!e.connected || !e.spec.valid || e.spec.collision) continue;
            appendExpanderRoleBlock(buf, bufOff, sizeof buf, i);
            ++boardCnt;
        }
    } else if (!isLocalTarget(guid)) {
        int idx = slotIdxByGuid(guid);
        if (idx < 0) {
            _ctx->sendNack(TopologyError::UNKNOWN_GUID);
            return;
        }
        appendExpanderRoleBlock(buf, bufOff, sizeof buf, (uint8_t)idx);
        ++boardCnt;
    }
    buf[0] = boardCnt;
    _ctx->sendRawPacket(TopologyPacket::TOPOLOGY_ROLE_LIST_RESP,
                        _ctx->currentTag(), buf, bufOff);
}

// ─── Hub-side block emitters ───────────────────────────────────────────

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::appendHubPortBlock(
        uint8_t* buf, size_t& off, size_t cap) {
    using namespace sfx_core;

    // GUID is EMPTY for the hub's own block (canonical hub-local address —
    // "" everywhere, see instructions/31). The hub used to emit its own
    // deviceName-suffix GUID here, which leaked a third "is this the hub"
    // form into the Go device model + forced hubLocal()/lookupProfile()
    // dual-form workarounds. The hub's identity GUID is still carried in
    // `deviceName` below for display; it is NEVER a port address.
    const char* name = _ctx->deviceName();
    if (off + 1 > cap) return;
    buf[off++] = 0;                                   // glen = 0 → hub-local

    // deviceName (identity / display — e.g. "HubFX-6D60")
    const uint8_t nlen = name ? (uint8_t)std::strlen(name) : 0;
    if (off + 1 + nlen > cap) return;
    buf[off++] = nlen;
    if (nlen) { std::memcpy(&buf[off], name, nlen); off += nlen; }

    // Per-entry layout (4 bytes): [idx:u8][flags:u8][voltageMv:u16LE].
    // Matches the standalone PORT_LIST_RESP encoder in
    // sfx_board/server/port_service.cpp (Phase 0 of GunFX rollout,
    // instructions/22). The Go side decodes both via the same shape.
    auto emitKind = [&](uint8_t (PortRegistryBase::*counter)() const,
                        auto                              fetchAt,
                        auto                              flagsFor) {
        const uint8_t n = (_reg->*counter)();
        // Pass 1 — count occupied ports (skip empty slots).
        uint8_t live = 0;
        for (uint8_t i = 0; i < n; ++i) {
            auto* b = (_reg->*fetchAt)(i);
            if (b && b->occupied()) ++live;
        }
        if (off + 1 + (size_t)live * 4 > cap) return;
        buf[off++] = live;
        for (uint8_t i = 0; i < n; ++i) {
            auto* b = (_reg->*fetchAt)(i);
            if (!b || !b->occupied()) continue;
            buf[off++] = i;
            buf[off++] = flagsFor(b);
            SfxWire::putU16LE(&buf[off], b->voltageMv);
            off += 2;
        }
    };

    emitKind(&PortRegistryBase::numServoPorts,
             &PortRegistryBase::servoAt,
             [](const ServoBinding* /*b*/) -> uint8_t {
                 return ServoPortFlags::EMITS;
             });

    emitKind(&PortRegistryBase::numPwmPorts,
             &PortRegistryBase::pwmAt,
             [](const PwmBinding* b) -> uint8_t {
                 uint8_t f = 0;
                 if (b->vSense) f |= PortSenseFlags::VOLTAGE;
                 if (b->iSense) f |= PortSenseFlags::CURRENT;
                 if (b->tSense) f |= PortSenseFlags::TEMPERATURE;
                 return f;
             });

    emitKind(&PortRegistryBase::numHBridgePorts,
             &PortRegistryBase::hbridgeAt,
             [](const HBridgeBinding* b) -> uint8_t {
                 uint8_t f = 0;
                 if (b->vSense) f |= PortSenseFlags::VOLTAGE;
                 if (b->iSense) f |= PortSenseFlags::CURRENT;
                 if (b->tSense) f |= PortSenseFlags::TEMPERATURE;
                 return f;
             });

    emitKind(&PortRegistryBase::numInputPorts,
             &PortRegistryBase::inputAt,
             [](const InputBinding* b) -> uint8_t {
                 return b->port ? b->port->capabilities() : 0;
             });
}

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::appendHubRoleBlock(
        uint8_t* buf, size_t& off, size_t cap) {
    using namespace sfx_core;

    // GUID is EMPTY for the hub's own block (canonical hub-local address;
    // see appendHubPortBlock + instructions/31).
    if (off + 1 + 1 > cap) return;
    buf[off++] = 0;                                   // glen = 0 → hub-local

    // Reserve roleCount byte; fill after walking the registry.
    const size_t cntOff = off++;
    uint8_t cnt = 0;

    // One walk; the role-type → RoleKind mapping lives in role_registry.h
    // (forEachAttachedRole), shared with RoleServicePolicy. Rule 58 — the hub
    // no longer hand-rolls a per-role-kind ladder here.
    forEachAttachedRole(*_reg, [&](uint8_t portKind, uint8_t idx, uint8_t roleKind) {
        if (off + 4 > cap) return;
        buf[off++] = portKind;
        buf[off++] = idx;
        buf[off++] = roleKind;
        buf[off++] = 0;            // flags reserved
        ++cnt;
    });

    buf[cntOff] = cnt;
}

// ─── Expander-side block emitters (from cached roster) ────────────────

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::appendExpanderPortBlock(
        uint8_t* buf, size_t& off, size_t cap, uint8_t slotIdx) {
    auto* slot = _exp->liveSlot(slotIdx);
    if (!slot) return;
    const auto& spec = slot->entry.spec;

    const uint8_t glen = (uint8_t)std::strlen(spec.guid);
    const uint8_t nlen = (uint8_t)std::strlen(spec.deviceName);
    if (off + 1 + glen + 1 + nlen + 4 > cap) return;

    buf[off++] = glen;
    std::memcpy(&buf[off], spec.guid, glen); off += glen;
    buf[off++] = nlen;
    std::memcpy(&buf[off], spec.deviceName, nlen); off += nlen;

    // Count per kind, then emit per kind in (Servo, Pwm, HBridge, Input)
    // order. Entry layout is 4 bytes (matches the hub-local emitter
    // in appendHubPortBlock + sfx_board/port_service.cpp): see
    // instructions/22 §0.7.
    for (uint8_t kind = PortKind::Servo; kind <= PortKind::Input; ++kind) {
        uint8_t cnt = 0;
        for (uint8_t i = 0; i < slot->numPorts; ++i) {
            if (slot->ports[i].kind == kind) ++cnt;
        }
        if (off + 1 + (size_t)cnt * 4 > cap) return;
        buf[off++] = cnt;
        for (uint8_t i = 0; i < slot->numPorts; ++i) {
            if (slot->ports[i].kind != kind) continue;
            buf[off++] = slot->ports[i].idx;
            buf[off++] = slot->ports[i].flags;
            SfxWire::putU16LE(&buf[off], slot->ports[i].voltageMv);
            off += 2;
        }
    }
}

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::appendExpanderRoleBlock(
        uint8_t* buf, size_t& off, size_t cap, uint8_t slotIdx) {
    auto* slot = _exp->liveSlot(slotIdx);
    if (!slot) return;
    const auto& spec = slot->entry.spec;

    const uint8_t glen = (uint8_t)std::strlen(spec.guid);
    if (off + 1 + glen + 1 > cap) return;
    buf[off++] = glen;
    std::memcpy(&buf[off], spec.guid, glen); off += glen;

    // Reserve count byte
    const size_t cntOff = off++;
    uint8_t cnt = 0;
    for (uint8_t i = 0; i < slot->numRoles; ++i) {
        if (off + 4 > cap) break;
        buf[off++] = slot->roles[i].portKind;
        buf[off++] = slot->roles[i].portIdx;
        buf[off++] = slot->roles[i].roleKind;
        buf[off++] = slot->roles[i].flags;
        ++cnt;
    }
    buf[cntOff] = cnt;
}

// ─── TOPOLOGY_ROLE_ATTACH / DETACH ─────────────────────────────────────

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::handleRoleAttach(
        const uint8_t* p, size_t len) {
    char   guid[5] = {0};
    size_t off     = 0;
    if (!readGuidPrefix(p, len, guid, off)) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    if (off + 4 > len) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    const uint8_t portKind = p[off++];
    const uint8_t portIdx  = p[off++];
    const uint8_t roleKind = p[off++];
    const uint8_t cfgLen   = p[off++];
    if (off + cfgLen > len) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    const uint8_t* cfg = &p[off];

    // Construct the inner ROLE_ATTACH payload — same wire format the
    // local RoleServicePolicy already speaks: [kind][idx][role][cfgLen][cfg].
    uint8_t inner[4 + 64];
    if ((size_t)cfgLen > sizeof(inner) - 4) {
        _ctx->sendNack(SerialError::INVALID_PARAM);
        return;
    }
    size_t innerLen = 0;
    inner[innerLen++] = portKind;
    inner[innerLen++] = portIdx;
    inner[innerLen++] = roleKind;
    inner[innerLen++] = cfgLen;
    if (cfgLen) { std::memcpy(&inner[innerLen], cfg, cfgLen); innerLen += cfgLen; }

    if (isLocalTarget(guid)) {
        // Local hub — call the existing RoleServicePolicy::handle().
        // It will issue ACK/NACK against _ctx->currentTag(), which IS
        // our topology request's tag.
        _roleSvc->handle(RolePacket::ROLE_ATTACH, inner, innerLen);
        return;
    }

    // Remote expander — forward synchronously.
    int slotIdx = slotIdxByGuid(guid);
    if (slotIdx < 0) {
        _ctx->sendNack(TopologyError::UNKNOWN_GUID);
        return;
    }
    auto* slot = _exp->liveSlot((uint8_t)slotIdx);
    if (slot->entry.spec.collision) {
        _ctx->sendNack(TopologyError::GUID_COLLISION);
        return;
    }
    if (slot->handshake != TExpander::Handshake::Ready) {
        _ctx->sendNack(TopologyError::HANDSHAKE_PENDING);
        return;
    }

    CommandResult rc = forwardToExpander((uint8_t)slotIdx,
                                         RolePacket::ROLE_ATTACH,
                                         inner, innerLen);
    if (rc.success) {
        // Update cached role roster — add or replace the (kind, idx) entry.
        bool replaced = false;
        for (uint8_t i = 0; i < slot->numRoles; ++i) {
            if (slot->roles[i].portKind == portKind &&
                slot->roles[i].portIdx  == portIdx) {
                slot->roles[i].roleKind = roleKind;
                slot->roles[i].flags    = 0;
                replaced = true;
                break;
            }
        }
        if (!replaced && slot->numRoles < TExpander::kMaxRolesPerExpander) {
            slot->roles[slot->numRoles++] = { portKind, portIdx, roleKind, 0 };
        }
        _ctx->sendAck();
    } else if (rc.errorCode != 0) {
        // Expander NACKed — pass the inner error code through unchanged
        // so Studio can render the original RoleError / PortError text.
        _ctx->sendNack(rc.errorCode);
    } else {
        _ctx->sendNack(TopologyError::FORWARD_FAILED);
    }
}

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::handleRoleDetach(
        const uint8_t* p, size_t len) {
    char   guid[5] = {0};
    size_t off     = 0;
    if (!readGuidPrefix(p, len, guid, off)) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    if (off + 2 > len) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    const uint8_t portKind = p[off++];
    const uint8_t portIdx  = p[off++];

    uint8_t inner[2] = { portKind, portIdx };

    if (isLocalTarget(guid)) {
        _roleSvc->handle(RolePacket::ROLE_DETACH, inner, sizeof inner);
        return;
    }

    int slotIdx = slotIdxByGuid(guid);
    if (slotIdx < 0) {
        _ctx->sendNack(TopologyError::UNKNOWN_GUID);
        return;
    }
    auto* slot = _exp->liveSlot((uint8_t)slotIdx);
    if (slot->entry.spec.collision) {
        _ctx->sendNack(TopologyError::GUID_COLLISION);
        return;
    }
    if (slot->handshake != TExpander::Handshake::Ready) {
        _ctx->sendNack(TopologyError::HANDSHAKE_PENDING);
        return;
    }

    CommandResult rc = forwardToExpander((uint8_t)slotIdx,
                                         RolePacket::ROLE_DETACH,
                                         inner, sizeof inner);
    if (rc.success) {
        // Drop the matching entry from the role cache.
        for (uint8_t i = 0; i < slot->numRoles; ++i) {
            if (slot->roles[i].portKind == portKind &&
                slot->roles[i].portIdx  == portIdx) {
                slot->roles[i] = slot->roles[slot->numRoles - 1];
                --slot->numRoles;
                break;
            }
        }
        _ctx->sendAck();
    } else if (rc.errorCode != 0) {
        _ctx->sendNack(rc.errorCode);
    } else {
        _ctx->sendNack(TopologyError::FORWARD_FAILED);
    }
}

// ─── Synchronous forward to an expander ────────────────────────────────

template <hubfx::expanders::ExpanderService TExpander>
CommandResult TopologyServicePolicyT<TExpander>::forwardToExpander(
        uint8_t slotIdx, uint8_t innerType,
        const uint8_t* payload, size_t len) {
    auto* slot = _exp->liveSlot(slotIdx);
    if (!slot) return CommandResult::SendFailed();

    uint8_t tag = slot->client.resultQueue().nextTag();
    int sent = slot->client.sendPacket(innerType, payload, len, tag);
    if (sent < 0) return CommandResult::SendFailed();

    // Block this serial loop until the expander ACKs / NACKs (or the
    // ResultQueue's default timeout fires).  Config commands are
    // infrequent and the round-trip is bounded — acceptable trade.
    return slot->client.resultQueue().waitForTag(tag, [slot]() {
        slot->client.process();
    });
}

// ─── Async re-emit ─────────────────────────────────────────────────────

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::onExpanderAsync(
        uint8_t slotIdx, uint8_t type, const uint8_t* p, size_t len) {
    if (!_ctx) return;
    auto* slot = _exp->liveSlot(slotIdx);
    if (!slot || !slot->entry.spec.valid) return;

    // [guidLen:u8][guid:str][innerType:u8][innerPayload:N]
    uint8_t buf[1 + 5 + 1 + 128];
    size_t  off = 0;
    const uint8_t glen = (uint8_t)std::strlen(slot->entry.spec.guid);
    buf[off++] = glen;
    std::memcpy(&buf[off], slot->entry.spec.guid, glen); off += glen;
    buf[off++] = type;
    size_t copy = len;
    if (off + copy > sizeof buf) copy = sizeof buf - off;
    if (copy) std::memcpy(&buf[off], p, copy);
    off += copy;

    // Smart routing: only re-emit expander telemetry to the PC wire when a
    // host is actually listening (keepalive-active).  With no PC attached we
    // don't flood a dead port — but the master-internal fan-out below STILL
    // runs, so on-hub effects keep consuming expander telemetry locally.
    if (_ctx->hostVerboseActive()) {
        _ctx->sendRawPacket(TopologyPacket::TOPOLOGY_ROLE_EVENT,
                            SfxWire::TAG_ASYNC, buf, off);
    }

    // Fan-out to master-internal subscribers with the unwrapped
    // payload + source GUID.
    for (uint8_t i = 0; i < _numRoleEventSubs; ++i) {
        const auto& s = _roleEventSubs[i];
        if (s.fn) s.fn(s.ctx, slot->entry.spec.guid, type, p, len);
    }

    // Auto-release claims on ROLE_DETACHED (expander reboot or manual
    // detach).  Payload is `[portKind:u8][portIdx:u8]`.
    if (type == RolePacket::ROLE_DETACHED && len >= 2) {
        PortRef ref = PortRef::remote(slot->entry.spec.guid, p[0], p[1]);
        release(ref);
    }
}

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::onLocalRoleAsyncTrampoline(
        void* ctx, uint8_t innerType, const uint8_t* p, size_t len) {
    auto* self = static_cast<TopologyServicePolicyT*>(ctx);
    if (!self) return;
    // These are HUB-LOCAL role events (from the local RoleService's
    // fireLocalAsync).  The internal routing convention addresses hub ports
    // as local (empty GUID — PortRef::local), and that's how effects resolve
    // their input PortRef (findInputByName → b->port = local).  Passing the
    // hub GUID here made sourceMatches() see bindLocal != evtLocal and reject
    // every binding (match=0), so RC never reached the effects.  Pass "" so
    // local events match local bindings.
    for (uint8_t i = 0; i < self->_numRoleEventSubs; ++i) {
        const auto& s = self->_roleEventSubs[i];
        if (s.fn) s.fn(s.ctx, "", innerType, p, len);
    }
    if (innerType == RolePacket::ROLE_DETACHED && len >= 2) {
        PortRef ref = PortRef::local(p[0], p[1]);
        self->release(ref);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Effect-facing API
// ═════════════════════════════════════════════════════════════════════

template <hubfx::expanders::ExpanderService TExpander>
bool TopologyServicePolicyT<TExpander>::sendRoleCommand(
        const PortRef& addr, uint8_t innerType,
        const uint8_t* payload, size_t len) {
    if (!_ctx) return false;

    // Batch mode: queue this command into the pooled buffer and
    // return optimistically.  Real dispatch happens in commitBatch().
    if (_batchDepth > 0) {
        if (_batchCount >= kMaxBatchEntries) {
            SFX_LOG_ERROR("[topology] batch queue full (%u entries)",
                          (unsigned)kMaxBatchEntries);
            return false;
        }
        if (_batchBufferUsed + len > kBatchBufferSize) {
            SFX_LOG_ERROR("[topology] batch payload buffer full (need %zu, have %u)",
                          len, (unsigned)(kBatchBufferSize - _batchBufferUsed));
            return false;
        }
        BatchEntry& e = _batch[_batchCount++];
        e.addr      = addr;
        e.innerType = innerType;
        e.offset    = _batchBufferUsed;
        e.len       = static_cast<uint16_t>(len);
        if (len) std::memcpy(&_batchBuffer[_batchBufferUsed], payload, len);
        _batchBufferUsed = static_cast<uint16_t>(_batchBufferUsed + len);
        return true;
    }

    if (isLocalTarget(addr.guid)) {
        // Local dispatch: invoke RoleServicePolicy::handle() in capture
        // mode so its sendAck()/sendNack() never reach the serial port.
        _ctx->beginCapture();
        CommandHandleResult r = _roleSvc->handle(innerType, payload, len);
        const bool    ack = _ctx->capturedAck();
        const uint8_t err = _ctx->capturedErr();
        _ctx->endCapture();
        if (r == CommandHandleResult::NotMyCommand) {
            SFX_LOG_WARN("[topology] local sendRoleCommand: role does not own type 0x%02X",
                         innerType);
            return false;
        }
        if (!ack) {
            SFX_LOG_DEBUG("[topology] local role command 0x%02X NACK 0x%02X",
                          innerType, err);
        }
        return ack;
    }

    // Remote dispatch — forward over CDC.
    int slotIdx = slotIdxByGuid(addr.guid);
    if (slotIdx < 0) {
        SFX_LOG_WARN("[topology] sendRoleCommand: unknown GUID %s", addr.guid);
        return false;
    }
    auto* slot = _exp->liveSlot((uint8_t)slotIdx);
    if (!slot || slot->entry.spec.collision ||
        slot->handshake != TExpander::Handshake::Ready) {
        return false;
    }
    CommandResult rc = forwardToExpander((uint8_t)slotIdx,
                                         innerType, payload, len);
    if (!rc.success) {
        SFX_LOG_DEBUG("[topology] forwardToExpander(%s, 0x%02X) failed err=0x%02X",
                      addr.guid, innerType, rc.errorCode);
    }
    return rc.success;
}

template <hubfx::expanders::ExpanderService TExpander>
bool TopologyServicePolicyT<TExpander>::attachRole(
        const PortRef& addr, uint8_t roleKind,
        const uint8_t* cfg, uint8_t cfgLen) {
    uint8_t inner[4 + 64];
    if (cfgLen > sizeof(inner) - 4) return false;
    inner[0] = addr.portKind;
    inner[1] = addr.portIdx;
    inner[2] = roleKind;
    inner[3] = cfgLen;
    if (cfgLen && cfg) std::memcpy(&inner[4], cfg, cfgLen);
    return sendRoleCommand(addr, RolePacket::ROLE_ATTACH,
                           inner, 4 + (size_t)cfgLen);
}

template <hubfx::expanders::ExpanderService TExpander>
bool TopologyServicePolicyT<TExpander>::detachRole(const PortRef& addr) {
    uint8_t inner[2] = { addr.portKind, addr.portIdx };
    bool ok = sendRoleCommand(addr, RolePacket::ROLE_DETACH, inner, 2);
    if (ok) release(addr);
    return ok;
}

template <hubfx::expanders::ExpanderService TExpander>
bool TopologyServicePolicyT<TExpander>::applyRoleConfig(
        const char* guid, const uint8_t* block, size_t len) {
    if (!guid || !guid[0] || !block || len < 1) return false;
    const uint8_t count = block[0];
    if (count == 0) return true;   // unconfigured board — nothing to push

    const int slotIdx = slotIdxByGuid(guid);
    if (slotIdx < 0) return false;
    auto* slot = _exp->liveSlot((uint8_t)slotIdx);
    if (!slot || slot->entry.spec.collision ||
        slot->handshake != TExpander::Handshake::Ready) {
        return false;
    }

    const CommandResult rc = forwardToExpander((uint8_t)slotIdx,
                                               RolePacket::ROLE_BULK_ATTACH, block, len);
    if (!rc.success) {
        SFX_LOG_WARN("[topology] bulk role-config to %s failed err=0x%02X",
                     guid, rc.errorCode);
        return false;
    }

    // Update the cached roster from the block we just applied — the expander
    // ACKed, so its roles now match the config.  The old per-port forward
    // (sendRoleCommand) NEVER did this, so topo-roles / Studio kept showing the
    // board's own boot defaults (e.g. 7 servos auto-attached, 0 hbridge) even
    // though the roles were applied.  This is the stale-roster fix.
    size_t  off     = 1;
    uint8_t updated = 0;
    for (uint8_t i = 0; i < count && off + 4 <= len; ++i) {
        const uint8_t pk = block[off], pi = block[off + 1], rk = block[off + 2],
                      cl = block[off + 3];
        off += (size_t)4 + cl;
        if (off > len) break;
        bool replaced = false;
        for (uint8_t j = 0; j < slot->numRoles; ++j) {
            if (slot->roles[j].portKind == pk && slot->roles[j].portIdx == pi) {
                slot->roles[j].roleKind = rk;
                slot->roles[j].flags    = 0;
                replaced = true;
                break;
            }
        }
        if (!replaced && slot->numRoles < TExpander::kMaxRolesPerExpander) {
            slot->roles[slot->numRoles++] = { pk, pi, rk, 0 };
        }
        ++updated;
    }
    SFX_LOG_INFO("[topology] bulk role-config to %s: %u role(s) applied + cached",
                 guid, (unsigned)updated);
    return true;
}

template <hubfx::expanders::ExpanderService TExpander>
size_t TopologyServicePolicyT<TExpander>::portsByRole(
        uint8_t roleKind, PortRef* out, size_t maxOut) const {
    if (!_reg || !_exp) return 0;
    size_t n = 0;
    auto push = [&](const char* guid, uint8_t pk, uint8_t pi) {
        if (n >= maxOut) return;
        out[n] = PortRef::remote(guid, pk, pi);
        if (!guid || !guid[0]) out[n].guid[0] = 0;
        ++n;
    };

    // ── Hub-local ── one filtered walk; role-kind map lives in role_registry.h.
    using namespace sfx_core;
    forEachAttachedRole(*_reg, [&](uint8_t pk, uint8_t pi, uint8_t rk) {
        if (rk == roleKind) push("", pk, pi);
    });

    // ── Every live expander's cached role roster ──
    for (uint8_t s = 0; s < TExpander::kMaxExpanders && n < maxOut; ++s) {
        auto* slot = _exp->liveSlot(s);
        if (!slot) continue;
        const auto& e = slot->entry;
        if (!e.connected || !e.spec.valid || e.spec.collision) continue;
        for (uint8_t r = 0; r < slot->numRoles && n < maxOut; ++r) {
            if (slot->roles[r].roleKind == roleKind) {
                push(e.spec.guid, slot->roles[r].portKind,
                     slot->roles[r].portIdx);
            }
        }
    }
    return n;
}

// ═════════════════════════════════════════════════════════════════════
//  Batch dispatch
// ═════════════════════════════════════════════════════════════════════

template <hubfx::expanders::ExpanderService TExpander>
bool TopologyServicePolicyT<TExpander>::commitBatch() {
    if (_batchDepth == 0) return true;
    --_batchDepth;
    if (_batchDepth > 0) {
        // Nested commit — let the outer level dispatch the queue.
        return true;
    }
    // Outermost commit: dispatch everything in queued order.  We
    // toggle batching off via the depth counter; sendRoleCommand
    // now sees depth==0 and runs the immediate path.
    const uint8_t  count = _batchCount;
    BatchEntry     local[kMaxBatchEntries];
    uint8_t        buffer[kBatchBufferSize];
    // Snapshot the queue + buffer so re-entry from inside dispatch
    // (e.g. a role's handle() calling back into topology) is safe.
    for (uint8_t i = 0; i < count; ++i) local[i] = _batch[i];
    std::memcpy(buffer, _batchBuffer, _batchBufferUsed);
    _batchCount        = 0;
    _batchBufferUsed   = 0;

    bool allOk = true;
    for (uint8_t i = 0; i < count; ++i) {
        const BatchEntry& e = local[i];
        if (!sendRoleCommand(e.addr, e.innerType,
                             &buffer[e.offset], e.len)) {
            allOk = false;
        }
    }
    return allOk;
}

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::discardBatch() {
    if (_batchDepth == 0) return;
    --_batchDepth;
    if (_batchDepth > 0) return;   // inner discard — outer wins
    _batchCount      = 0;
    _batchBufferUsed = 0;
}

// ═════════════════════════════════════════════════════════════════════
//  Claim registry
// ═════════════════════════════════════════════════════════════════════

template <hubfx::expanders::ExpanderService TExpander>
bool TopologyServicePolicyT<TExpander>::claim(
        const PortRef& addr, EffectId who, uint8_t roleKind) {
    if (who == EffectId::None) return false;
    // Existing claim?
    for (uint8_t i = 0; i < _numClaims; ++i) {
        if (!_claims[i].occupied) continue;
        if (_claims[i].port.equals(addr)) {
            if (_claims[i].owner == who) {
                _claims[i].roleKind = roleKind;
                return true;   // idempotent
            }
            SFX_LOG_WARN("[topology] claim conflict on %s:%u/%u — held by %s, requested by %s",
                         addr.guid[0] ? addr.guid : "hub",
                         (unsigned)addr.portKind, (unsigned)addr.portIdx,
                         hubfx::effects::effectIdName(_claims[i].owner),
                         hubfx::effects::effectIdName(who));
            return false;
        }
    }
    if (_numClaims >= kMaxClaims) {
        SFX_LOG_ERROR("[topology] claim table full (%u entries)", (unsigned)kMaxClaims);
        return false;
    }
    _claims[_numClaims] = { addr, roleKind, who, true };
    ++_numClaims;
    return true;
}

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::release(const PortRef& addr) {
    for (uint8_t i = 0; i < _numClaims; ++i) {
        if (_claims[i].occupied && _claims[i].port.equals(addr)) {
            releaseAt(i);
            return;
        }
    }
}

template <hubfx::expanders::ExpanderService TExpander>
EffectId TopologyServicePolicyT<TExpander>::ownerOf(const PortRef& addr) const {
    for (uint8_t i = 0; i < _numClaims; ++i) {
        if (_claims[i].occupied && _claims[i].port.equals(addr)) {
            return _claims[i].owner;
        }
    }
    return EffectId::None;
}

template <hubfx::expanders::ExpanderService TExpander>
void TopologyServicePolicyT<TExpander>::releaseAt(uint8_t i) {
    if (i >= _numClaims) return;
    const uint8_t last = _numClaims - 1;
    if (i != last) _claims[i] = _claims[last];
    _claims[last] = {};
    --_numClaims;
}

}  // namespace hubfx::topology

#endif  // HUBFX_TOPOLOGY_SERVICE_IPP
