/*
 * ExpanderServicePolicyT — template-method definitions.
 *
 *   Included at the bottom of expander_service.h.  Kept separate so the
 *   header stays readable and the IDE can navigate signatures without
 *   wading through the implementation.
 */

#ifndef HUBFX_EXPANDER_SERVICE_IPP
#define HUBFX_EXPANDER_SERVICE_IPP

#include <serial/ports.h>     // PortPacket / PortKind
#include <serial/roles.h>     // RolePacket

namespace hubfx::expanders {

// Roster harvest timeout (per request) — generous since the IDENTIFY
// itself can lag a few hundred ms on a fresh hub.
static constexpr uint32_t kRosterRequestTimeoutMs = 1500;

// ─── Lifecycle ──────────────────────────────────────────────────────────

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
bool ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::begin(
        sfx_core::BoardServerBase* ctx) {
    _ctx = ctx;
    if (!_ctx) return false;

    // Tag every slot with its own initial generation so that any
    // pre-bound topology handles see the same "first epoch" value.
    for (uint8_t i = 0; i < kMaxExpanders; ++i) _live[i].gen = 1;

    UsbHost& usb = UsbHost::instance();

    usb.onMount([this](uint8_t addr, uint16_t vid, uint16_t pid) {
        this->_onUsbMount(addr, vid, pid);
    });
    usb.onUnmount([this](uint8_t addr) {
        this->_onUsbUnmount(addr);
    });

    if (!usb.begin()) {
        SFX_LOG_WARN("[Expander] USB host begin() failed — OTG port disabled");
        _usbReady = false;
        return true;   // policy itself is fine, just no expanders will be seen
    }

    _usbReady = true;
    SFX_LOG_INFO("[Expander] USB host up (%s), %u slots, %u known-GUID cache",
                 usb.backendName(),
                 (unsigned)kMaxExpanders,
                 (unsigned)kMaxKnownGuids);
    return true;
}

// ─── USB callback adapters ──────────────────────────────────────────────

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::_onUsbMount(
        uint8_t devAddr, uint16_t vid, uint16_t pid) {
    const uint8_t kind = classifyByVidPid(vid, pid);

    // Re-bind if devAddr was already present (defensive); else take a free slot.
    LiveSlot* slot = findByUsbAddr(devAddr);
    if (!slot) slot = findFreeSlot();
    if (!slot) {
        SFX_LOG_WARN("[Expander] table full — ignoring mount addr=%u vid=0x%04X pid=0x%04X",
                     devAddr, vid, pid);
        return;
    }

    // Bump the slot generation BEFORE filling the entry — any topology
    // handle still pointing at the previous occupant will see the
    // mismatch on its next call and drop or re-resolve.
    ++slot->gen;

    ExpanderEntry& e = slot->entry;
    e.kind        = kind;
    e.connected   = true;
    e.identifying = false;
    e.usbAddr     = devAddr;
    e.usbIndex    = 0xFF;
    e.vid         = vid;
    e.pid         = pid;
    e.spec        = ExpanderSpec{};

    // Map devAddr → UsbHost CDC index so the BusClient can talk to it.
    UsbHost& usb = UsbHost::instance();
    for (int i = 0; i < usb.cdcDeviceCount(); ++i) {
        const CdcDeviceInfo* info = usb.getCdcDevice(i);
        if (info && info->connected && info->dev_addr == devAddr) {
            e.usbIndex = (uint8_t)i;
            break;
        }
    }

    SFX_LOG_INFO("[Expander] CONNECT %s (addr=%u vid=0x%04X pid=0x%04X usbIdx=%u)",
                 ExpanderKind::getName(kind),
                 devAddr, vid, pid, (unsigned)e.usbIndex);

    if (_onConnect) _onConnect(e);
    emitConnected(e);

    // Kick off the IDENTIFY round-trip.  Non-blocking — the response
    // lands in BusClient::handlePacket() and triggers our ready
    // callback below, which copies the spec out.
    if (e.usbIndex == 0xFF) {
        SFX_LOG_WARN("[Expander] no CDC index for addr=%u — skipping IDENTIFY",
                     devAddr);
        return;
    }

    const uint8_t slotIdx = (uint8_t)slotIndex(slot);

    slot->client.setBlockingMode(false);
    if (!slot->client.begin((int)e.usbIndex)) {
        SFX_LOG_WARN("[Expander] BusClient.begin(%u) failed for addr=%u",
                     (unsigned)e.usbIndex, devAddr);
        return;
    }
    slot->client.onReady([this, slotIdx](const char* /*name*/) {
        this->onIdentifyResponse(slotIdx);
    });
    // Re-emit every TAG_ASYNC packet from the expander upward.  The
    // topology layer subscribes via onExpanderAsync() and wraps these
    // into TOPOLOGY_ROLE_EVENT packets so Studio sees a unified stream.
    slot->client.onAsyncPacket([this, slotIdx](uint8_t type,
                                               const uint8_t* p, size_t len) {
        if (_onExpanderAsync) _onExpanderAsync(slotIdx, type, p, len);
    });

    slot->handshake          = Handshake::Identifying;
    slot->handshakeDeadlineMs = 0;
    slot->numPorts           = 0;
    slot->numRoles           = 0;
    slot->qHead = slot->qTail = 0;
    slot->droppedCount        = 0;

    if (slot->client.sendIdentify() < 0) {
        SFX_LOG_WARN("[Expander] sendIdentify() failed for addr=%u", devAddr);
        slot->handshake = Handshake::Failed;
        return;
    }
    e.identifying        = true;
    slot->identifyDeadlineMs = millis() + kIdentifyTimeoutMs;
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::_onUsbUnmount(
        uint8_t devAddr) {
    LiveSlot* slot = findByUsbAddr(devAddr);
    if (!slot) return;   // wasn't tracked

    ExpanderEntry snapshot = slot->entry;

    // Mark the cached spec disconnected (but keep it cached).
    if (snapshot.spec.valid) {
        if (KnownGuid* k = acquireKnown(snapshot.spec.guid)) {
            k->connectedSlot = 0xFF;
            k->lastSeenMs    = millis();
        }
    }

    // Tear down the CDC client + clear the live slot.
    slot->client.end();
    slot->identifyDeadlineMs = 0;
    clearEntry(slot->entry);
    ++slot->gen;   // any cached slot-token from before this unmount is now stale

    SFX_LOG_INFO("[Expander] DISCONNECT %s (addr=%u guid=%s)",
                 ExpanderKind::getName(snapshot.kind),
                 devAddr,
                 snapshot.spec.valid ? snapshot.spec.guid : "?");

    if (_onDisconnect) _onDisconnect(snapshot);
    emitDisconnected(snapshot);
}

// ─── Per-loop tick ──────────────────────────────────────────────────────

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::update() {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < kMaxExpanders; ++i) {
        LiveSlot& s = _live[i];
        if (!s.entry.connected) continue;

        // Pump the CDC client — drives IDENTIFY response decoding,
        // any pending sendQuery response capture, and async role events.
        s.client.process();

        // ── IDENTIFY watchdog (retries once on timeout) ───────────────
        if (s.entry.identifying && s.identifyDeadlineMs &&
            (int32_t)(now - s.identifyDeadlineMs) > 0) {
            SFX_LOG_WARN("[Expander] IDENTIFY timeout for addr=%u — retrying",
                         s.entry.usbAddr);
            s.entry.identifying     = false;
            s.identifyDeadlineMs    = 0;
            if (s.client.sendIdentify() >= 0) {
                s.entry.identifying     = true;
                s.identifyDeadlineMs    = now + kIdentifyTimeoutMs;
            } else {
                s.handshake = Handshake::Failed;
            }
        }

        // ── Roster harvest state machine ──────────────────────────────
        // pendingResp.len becomes non-zero once BusClient's sendQuery
        // capture fires — see bus_client.cpp:204.
        if (s.handshake == Handshake::FetchingPorts &&
            s.pendingResp.len > 0) {
            onPortListResp(i, s.pendingResp.payload, s.pendingResp.len);
            s.pendingResp = SerialPacket{};
        }
        if (s.handshake == Handshake::FetchingRoles &&
            s.pendingResp.len > 0) {
            onRoleListResp(i, s.pendingResp.payload, s.pendingResp.len);
            s.pendingResp = SerialPacket{};
        }
        // Per-state timeout — if the response never lands, mark Failed
        // so we don't poll forever.
        if ((s.handshake == Handshake::FetchingPorts ||
             s.handshake == Handshake::FetchingRoles) &&
            s.handshakeDeadlineMs &&
            (int32_t)(now - s.handshakeDeadlineMs) > 0) {
            SFX_LOG_WARN("[Expander] roster timeout in state=%u addr=%u",
                         (unsigned)s.handshake, s.entry.usbAddr);
            s.handshake          = Handshake::Failed;
            s.handshakeDeadlineMs = 0;
        }

        // ── Drain outbound queue ──────────────────────────────────────
        if (s.handshake == Handshake::Ready) flushQueue(i);

        // ── Battery telemetry poll (BATTERY-capable Ready slots) ──────
        if (s.handshake == Handshake::Ready) tickBatteryPoll(i);
    }
}

// ─── IDENTIFY response capture ──────────────────────────────────────────

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::onIdentifyResponse(
        uint8_t slotIdx) {
    if (slotIdx >= kMaxExpanders) return;
    LiveSlot& s = _live[slotIdx];
    if (!s.entry.connected) return;

    // BusClient.boardInfo() is already decoded from the INIT_READY /
    // IDENTIFY payload.  Copy the fields we care about into the spec.
    const BusClientBoardInfo& info = s.client.boardInfo();

    ExpanderSpec& spec = s.entry.spec;
    spec.valid             = true;
    std::strncpy(spec.deviceName,      info.deviceName,      sizeof(spec.deviceName) - 1);
    std::strncpy(spec.firmwareVersion, info.firmwareVersion, sizeof(spec.firmwareVersion) - 1);
    std::strncpy(spec.platform,        info.platform,        sizeof(spec.platform) - 1);
    spec.cpuFrequencyMHz   = info.cpuFrequencyMHz;
    spec.freeRamBytes      = info.freeRamBytes;
    spec.buildNumber       = info.buildNumber;
    spec.capabilities      = info.capabilities;
    extractGuid(spec.deviceName, spec.guid);

    s.entry.identifying    = false;
    s.identifyDeadlineMs   = 0;

    // ── GUID collision detection (Rule 32) ───────────────────────────
    // Scan every OTHER live slot — if any other identified expander
    // already holds this GUID, flag both as collisions and emit a
    // wire event so Studio can prompt the user to set a guid override
    // on one of them.  Both boards stay connected (so the user can
    // still talk to them to fix the override); routing layers
    // (TopologyService) refuse to bind roles to collided slots.
    spec.collision = false;
    for (uint8_t j = 0; j < kMaxExpanders; ++j) {
        if (j == slotIdx) continue;
        LiveSlot& other = _live[j];
        if (!other.entry.connected || !other.entry.spec.valid) continue;
        if (std::strncmp(other.entry.spec.guid, spec.guid,
                         sizeof(spec.guid)) != 0) continue;

        spec.collision           = true;
        other.entry.spec.collision = true;
        SFX_LOG_ERROR("[Expander] GUID COLLISION %s — addr=%u vs addr=%u "
                      "(set board.guid override on one of them)",
                      spec.guid, s.entry.usbAddr, other.entry.usbAddr);
        emitCollision(spec.guid, s.entry.usbAddr, other.entry.usbAddr);
        break;
    }

    // Persist into the GUID-keyed history (replaces or evicts as needed).
    // Collision flag is intentionally carried into the cache so that
    // unplug + replug doesn't silently "fix" the conflict.
    if (KnownGuid* k = acquireKnown(spec.guid)) {
        k->spec          = spec;
        k->kind          = s.entry.kind;
        k->connectedSlot = slotIdx;
        k->lastSeenMs    = millis();
    }

    SFX_LOG_INFO("[Expander] IDENTIFIED %s guid=%s fw=%s caps=0x%08lx build=%lu%s",
                 ExpanderKind::getName(s.entry.kind),
                 spec.guid,
                 spec.firmwareVersion,
                 (unsigned long)spec.capabilities,
                 (unsigned long)spec.buildNumber,
                 spec.collision ? " [COLLISION]" : "");

    if (_onIdentified) _onIdentified(s.entry);
    emitIdentified(s.entry);

    // ── Advance to roster harvest (PORT_LIST → ROLE_LIST → Ready) ────
    // Collided slots skip the harvest — we still know who they claim
    // to be but won't bind anything until the user resolves the GUID.
    if (spec.collision) {
        s.handshake = Handshake::Failed;
    } else {
        kickFetchPorts(slotIdx);
    }
}

// ─── Wire dispatch ──────────────────────────────────────────────────────

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
CommandHandleResult ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::handle(
        uint8_t type, const uint8_t* /*payload*/, size_t /*len*/) {
    switch (type) {
        case ExpanderPacket::EXPANDER_LIST_REQ:
            handleListReq();
            return CommandHandleResult::Handled;
        case ExpanderPacket::EXPANDER_SYSTEM_INFO_REQ:
            handleSystemInfoReq();
            return CommandHandleResult::Handled;
        default:
            return CommandHandleResult::NotMyCommand;
    }
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::handleListReq() {
    // Worst-case sized; per entry max ≈ 1+1+2+2+1+1+4+1+32+1+16+4+4 ≈ 70 bytes
    constexpr size_t kPerEntryMax = 1 + 1 + 2 + 2 + 1 + 1 + 4 + 1 + 32 + 1 + 16 + 4 + 4;
    uint8_t buf[1 + kMaxExpanders * kPerEntryMax];
    size_t  off = 1;
    uint8_t count = 0;

    for (uint8_t i = 0; i < kMaxExpanders; ++i) {
        const ExpanderEntry& e = _live[i].entry;
        if (!e.connected) continue;

        buf[off++] = e.kind;
        buf[off++] = e.usbAddr;
        SfxWire::putU16LE(&buf[off], e.vid); off += 2;
        SfxWire::putU16LE(&buf[off], e.pid); off += 2;
        buf[off++] = e.spec.valid     ? 1 : 0;
        buf[off++] = e.spec.collision ? 1 : 0;

        if (e.spec.valid) {
            const uint8_t glen = (uint8_t)std::strlen(e.spec.guid);
            buf[off++] = glen;
            std::memcpy(&buf[off], e.spec.guid, glen); off += glen;

            const uint8_t nlen = (uint8_t)std::strlen(e.spec.deviceName);
            buf[off++] = nlen;
            std::memcpy(&buf[off], e.spec.deviceName, nlen); off += nlen;

            const uint8_t vlen = (uint8_t)std::strlen(e.spec.firmwareVersion);
            buf[off++] = vlen;
            std::memcpy(&buf[off], e.spec.firmwareVersion, vlen); off += vlen;

            SfxWire::putU32LE(&buf[off], e.spec.capabilities); off += 4;
            SfxWire::putU32LE(&buf[off], e.spec.buildNumber);  off += 4;
        }

        ++count;
    }
    buf[0] = count;

    _ctx->sendRawPacket(ExpanderPacket::EXPANDER_LIST_RESP,
                        _ctx->currentTag(), buf, off);
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::handleSystemInfoReq() {
    // Hub block + per-expander block — Studio reads one packet to populate
    // its full system view on connect (instead of IDENTIFY + LIST_REQ).
    // Worst-case sizing:
    //   hub block ≈ 5 + 1+32 + 1+16 + 1+24 + 4×4   ≈ 95 bytes
    //   per-expander block ≈ 1+1+1+1 + 1+4 + 1+32 + 1+16 + 4 + 4 + 7  ≈ 74 bytes
    //   total ≈ 95 + 1 + kMaxExpanders * 74
    constexpr size_t kHubBlockMax        = 1+5 + 1+32 + 1+16 + 1+24 + 4*4;
    constexpr size_t kPerExpanderMax     = 1+1+1+1 + 1+5 + 1+32 + 1+16 + 4 + 4 + 7;
    uint8_t buf[kHubBlockMax + 1 + kMaxExpanders * kPerExpanderMax];
    size_t  off = 0;

    // ── Hub block ────────────────────────────────────────────────────
    char hubGuid[5] = {0};
    if (_ctx && _ctx->deviceName()) {
        extractGuid(_ctx->deviceName(), hubGuid);
    }
    const uint8_t hglen = (uint8_t)std::strlen(hubGuid);
    buf[off++] = hglen;
    std::memcpy(&buf[off], hubGuid, hglen); off += hglen;

    const CoreBoardInfo* hub = _ctx ? _ctx->hubBoardInfo() : nullptr;
    if (hub) {
        const uint8_t nlen = (uint8_t)std::strlen(hub->deviceName);
        buf[off++] = nlen;
        std::memcpy(&buf[off], hub->deviceName, nlen); off += nlen;

        const uint8_t vlen = (uint8_t)std::strlen(hub->firmwareVersion);
        buf[off++] = vlen;
        std::memcpy(&buf[off], hub->firmwareVersion, vlen); off += vlen;

        const uint8_t plen = (uint8_t)std::strlen(hub->platform);
        buf[off++] = plen;
        std::memcpy(&buf[off], hub->platform, plen); off += plen;

        SfxWire::putU32LE(&buf[off], hub->cpuFrequencyMHz); off += 4;
        SfxWire::putU32LE(&buf[off], hub->freeRamBytes);    off += 4;
        SfxWire::putU32LE(&buf[off], hub->buildNumber);     off += 4;
        SfxWire::putU32LE(&buf[off], hub->capabilities);    off += 4;
    } else {
        // Hub info not yet bound — emit empty strings + zero numerics so
        // the decoder still has a parseable block.
        buf[off++] = 0;  // nameLen
        buf[off++] = 0;  // verLen
        buf[off++] = 0;  // platLen
        SfxWire::putU32LE(&buf[off], 0); off += 4;
        SfxWire::putU32LE(&buf[off], 0); off += 4;
        SfxWire::putU32LE(&buf[off], 0); off += 4;
        SfxWire::putU32LE(&buf[off], 0); off += 4;
    }

    // ── Expander list ───────────────────────────────────────────────
    const size_t countOff = off++;     // placeholder, filled after walk
    uint8_t count = 0;
    for (uint8_t i = 0; i < kMaxExpanders; ++i) {
        const ExpanderEntry& e = _live[i].entry;
        if (!e.connected) continue;

        buf[off++] = e.kind;
        buf[off++] = e.usbAddr;
        buf[off++] = e.spec.valid     ? 1 : 0;
        buf[off++] = e.spec.collision ? 1 : 0;

        if (e.spec.valid) {
            const uint8_t glen = (uint8_t)std::strlen(e.spec.guid);
            buf[off++] = glen;
            std::memcpy(&buf[off], e.spec.guid, glen); off += glen;

            const uint8_t nlen = (uint8_t)std::strlen(e.spec.deviceName);
            buf[off++] = nlen;
            std::memcpy(&buf[off], e.spec.deviceName, nlen); off += nlen;

            const uint8_t vlen = (uint8_t)std::strlen(e.spec.firmwareVersion);
            buf[off++] = vlen;
            std::memcpy(&buf[off], e.spec.firmwareVersion, vlen); off += vlen;

            SfxWire::putU32LE(&buf[off], e.spec.capabilities); off += 4;
            SfxWire::putU32LE(&buf[off], e.spec.buildNumber);  off += 4;

            // Battery section (Rule 11 append-only) — the latest poll of
            // this expander's STATUS_REQ.  `valid=0` ⇒ no reading yet (or
            // board has no BATTERY capability):
            //   [valid:u8][present:u8][voltage_mV:u16LE][cells:u8][pct:u8][flags:u8]
            const auto& bat = _live[i].battery;
            buf[off++] = bat.valid   ? 1 : 0;
            buf[off++] = bat.present ? 1 : 0;
            SfxWire::putU16LE(&buf[off], bat.voltage_mV); off += 2;
            buf[off++] = bat.cellCount;
            buf[off++] = bat.pct;
            buf[off++] = bat.flags;
        }
        ++count;
    }
    buf[countOff] = count;

    _ctx->sendRawPacket(ExpanderPacket::EXPANDER_SYSTEM_INFO_RESP,
                        _ctx->currentTag(), buf, off);
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::emitConnected(
        const ExpanderEntry& e) {
    if (!_ctx) return;
    uint8_t buf[1 + 1 + 2 + 2];
    buf[0] = e.kind;
    buf[1] = e.usbAddr;
    SfxWire::putU16LE(&buf[2], e.vid);
    SfxWire::putU16LE(&buf[4], e.pid);
    _ctx->sendRawPacket(ExpanderPacket::EXPANDER_CONNECTED,
                        SfxWire::TAG_ASYNC, buf, sizeof buf);
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::emitIdentified(
        const ExpanderEntry& e) {
    if (!_ctx || !e.spec.valid) return;

    // [kind:u8][usbAddr:u8][guidLen:u8][guid][nameLen:u8][name][verLen:u8][ver]
    // [capabilities:u32LE][buildNumber:u32LE]
    uint8_t buf[1 + 1 + 1 + 5 + 1 + 32 + 1 + 16 + 4 + 4];
    size_t  off = 0;

    buf[off++] = e.kind;
    buf[off++] = e.usbAddr;

    const uint8_t glen = (uint8_t)std::strlen(e.spec.guid);
    buf[off++] = glen;
    std::memcpy(&buf[off], e.spec.guid, glen); off += glen;

    const uint8_t nlen = (uint8_t)std::strlen(e.spec.deviceName);
    buf[off++] = nlen;
    std::memcpy(&buf[off], e.spec.deviceName, nlen); off += nlen;

    const uint8_t vlen = (uint8_t)std::strlen(e.spec.firmwareVersion);
    buf[off++] = vlen;
    std::memcpy(&buf[off], e.spec.firmwareVersion, vlen); off += vlen;

    SfxWire::putU32LE(&buf[off], e.spec.capabilities); off += 4;
    SfxWire::putU32LE(&buf[off], e.spec.buildNumber);  off += 4;

    _ctx->sendRawPacket(ExpanderPacket::EXPANDER_IDENTIFIED,
                        SfxWire::TAG_ASYNC, buf, off);
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::emitDisconnected(
        const ExpanderEntry& e) {
    if (!_ctx) return;
    // [kind:u8][usbAddr:u8][guidLen:u8][guid]
    uint8_t buf[1 + 1 + 1 + 5];
    size_t  off = 0;
    buf[off++] = e.kind;
    buf[off++] = e.usbAddr;
    const uint8_t glen = e.spec.valid ? (uint8_t)std::strlen(e.spec.guid) : 0;
    buf[off++] = glen;
    if (glen) { std::memcpy(&buf[off], e.spec.guid, glen); off += glen; }
    _ctx->sendRawPacket(ExpanderPacket::EXPANDER_DISCONNECTED,
                        SfxWire::TAG_ASYNC, buf, off);
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::emitCollision(
        const char* guid, uint8_t addrA, uint8_t addrB) {
    if (!_ctx) return;
    // [guidLen:u8][guid][addrA:u8][addrB:u8]
    uint8_t buf[1 + 5 + 1 + 1];
    size_t  off = 0;
    const uint8_t glen = guid ? (uint8_t)std::strlen(guid) : 0;
    buf[off++] = glen;
    if (glen) { std::memcpy(&buf[off], guid, glen); off += glen; }
    buf[off++] = addrA;
    buf[off++] = addrB;
    _ctx->sendRawPacket(ExpanderPacket::EXPANDER_COLLISION,
                        SfxWire::TAG_ASYNC, buf, off);
}

// ─── Helpers ────────────────────────────────────────────────────────────

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
uint8_t ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::classifyByVidPid(
        uint16_t vid, uint16_t pid) {
    if (vid != USB_VID_RASPBERRY_PI) return ExpanderKind::Unknown;
    switch (pid) {
        case USB_PID_GUNFX:        return ExpanderKind::GunFX;
        case USB_PID_LIGHTFX:      return ExpanderKind::LightFX;
        case USB_PID_GEARCONTROL:  return ExpanderKind::GearControl;
        case USB_PID_PICO_DEFAULT: return ExpanderKind::PicoDefault;
        default:                   return ExpanderKind::Unknown;
    }
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
typename ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::LiveSlot*
ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::findFreeSlot() {
    for (uint8_t i = 0; i < kMaxExpanders; ++i) {
        if (!_live[i].entry.connected) return &_live[i];
    }
    return nullptr;
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
typename ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::LiveSlot*
ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::findByUsbAddr(
        uint8_t usbAddr) {
    for (uint8_t i = 0; i < kMaxExpanders; ++i) {
        if (_live[i].entry.connected && _live[i].entry.usbAddr == usbAddr) {
            return &_live[i];
        }
    }
    return nullptr;
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::clearEntry(
        ExpanderEntry& e) {
    e.kind        = ExpanderKind::Unknown;
    e.connected   = false;
    e.identifying = false;
    e.usbAddr     = 0;
    e.usbIndex    = 0xFF;
    e.vid         = 0;
    e.pid         = 0;
    e.spec        = ExpanderSpec{};
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::extractGuid(
        const char* deviceName, char outGuid[5]) {
    outGuid[0] = 0;
    if (!deviceName) return;
    const char* dash = std::strrchr(deviceName, '-');
    if (!dash || !dash[1]) return;
    const char* suffix = dash + 1;
    size_t n = std::strlen(suffix);
    if (n > 4) n = 4;
    std::memcpy(outGuid, suffix, n);
    outGuid[n] = 0;
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
typename ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::KnownGuid*
ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::acquireKnown(
        const char* guid) {
    if (!guid || !guid[0]) return nullptr;

    // 1. Exact match?
    for (uint8_t i = 0; i < kMaxKnownGuids; ++i) {
        if (_known[i].spec.valid &&
            std::strncmp(_known[i].spec.guid, guid, sizeof(_known[i].spec.guid)) == 0) {
            return &_known[i];
        }
    }
    // 2. Free slot?
    for (uint8_t i = 0; i < kMaxKnownGuids; ++i) {
        if (!_known[i].spec.valid) return &_known[i];
    }
    // 3. Evict oldest *disconnected* slot.
    int     evictIdx = -1;
    uint32_t oldest  = UINT32_MAX;
    for (uint8_t i = 0; i < kMaxKnownGuids; ++i) {
        if (_known[i].connectedSlot != 0xFF) continue;   // skip live boards
        if (_known[i].lastSeenMs < oldest) {
            oldest   = _known[i].lastSeenMs;
            evictIdx = i;
        }
    }
    if (evictIdx >= 0) {
        SFX_LOG_DEBUG("[Expander] evicting known guid=%s for %s",
                      _known[evictIdx].spec.guid, guid);
        _known[evictIdx] = KnownGuid{};
        return &_known[evictIdx];
    }
    // Everything live (very rare) — no eviction allowed.
    return nullptr;
}

// ─── Roster harvest ─────────────────────────────────────────────────────

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::kickFetchPorts(
        uint8_t slotIdx) {
    if (slotIdx >= kMaxExpanders) return;
    LiveSlot& s = _live[slotIdx];

    s.pendingResp = SerialPacket{};
    auto rc = s.client.sendQuery(PortPacket::PORT_LIST_REQ,
                                 nullptr, 0,
                                 PortPacket::PORT_LIST_RESP,
                                 s.pendingResp);
    if (!rc.success) {
        SFX_LOG_WARN("[Expander] PORT_LIST_REQ send failed addr=%u",
                     s.entry.usbAddr);
        s.handshake = Handshake::Failed;
        return;
    }
    s.handshake          = Handshake::FetchingPorts;
    s.handshakeDeadlineMs = millis() + kRosterRequestTimeoutMs;
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::kickFetchRoles(
        uint8_t slotIdx) {
    if (slotIdx >= kMaxExpanders) return;
    LiveSlot& s = _live[slotIdx];

    s.pendingResp = SerialPacket{};
    auto rc = s.client.sendQuery(RolePacket::ROLE_LIST_REQ,
                                 nullptr, 0,
                                 RolePacket::ROLE_LIST_RESP,
                                 s.pendingResp);
    if (!rc.success) {
        SFX_LOG_WARN("[Expander] ROLE_LIST_REQ send failed addr=%u",
                     s.entry.usbAddr);
        s.handshake = Handshake::Failed;
        return;
    }
    s.handshake          = Handshake::FetchingRoles;
    s.handshakeDeadlineMs = millis() + kRosterRequestTimeoutMs;
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::onPortListResp(
        uint8_t slotIdx, const uint8_t* p, size_t len) {
    if (slotIdx >= kMaxExpanders) return;
    LiveSlot& s = _live[slotIdx];

    // Wire layout (see PortServicePolicy::handlePortListReq) — 4 bytes
    // per entry post-Phase-0 of the GunFX rollout (instructions/22):
    //   [numServo:u8]   × [idx:u8][flags:u8][voltageMv:u16LE]
    //   [numPwm:u8]     × [idx:u8][flags:u8][voltageMv:u16LE]
    //   [numHBridge:u8] × [idx:u8][flags:u8][voltageMv:u16LE]
    //   [numInput:u8]   × [idx:u8][flags:u8][voltageMv:u16LE]
    auto consume = [&](uint8_t kind, size_t& off, uint8_t count) {
        for (uint8_t k = 0; k < count && off + 4 <= len; ++k) {
            if (s.numPorts >= kMaxPortsPerExpander) {
                SFX_LOG_WARN("[Expander] port roster overflow addr=%u",
                             s.entry.usbAddr);
                off = len;   // bail out cleanly
                return;
            }
            const uint16_t v = SfxWire::getU16LE(&p[off + 2]);
            s.ports[s.numPorts++] = { kind, p[off], p[off + 1], v };
            off += 4;
        }
    };

    s.numPorts = 0;
    size_t off = 0;
    if (off + 1 > len) { s.handshake = Handshake::Failed; return; }
    uint8_t nServo = p[off++]; consume(PortKind::Servo,   off, nServo);
    if (off + 1 > len) { s.handshake = Handshake::Failed; return; }
    uint8_t nPwm   = p[off++]; consume(PortKind::Pwm,     off, nPwm);
    if (off + 1 > len) { s.handshake = Handshake::Failed; return; }
    uint8_t nHbr   = p[off++]; consume(PortKind::HBridge, off, nHbr);
    // Inputs are append-only (Rule 11) — older expanders may omit the count.
    if (off + 1 <= len) {
        uint8_t nIn = p[off++]; consume(PortKind::Input, off, nIn);
    }

    SFX_LOG_INFO("[Expander] roster: %s has %u ports (S=%u P=%u H=%u I=%u)",
                 s.entry.spec.guid, (unsigned)s.numPorts,
                 nServo, nPwm, nHbr,
                 (uint8_t)(s.numPorts - nServo - nPwm - nHbr));

    kickFetchRoles(slotIdx);
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::onRoleListResp(
        uint8_t slotIdx, const uint8_t* p, size_t len) {
    if (slotIdx >= kMaxExpanders) return;
    LiveSlot& s = _live[slotIdx];

    // Wire layout (see RoleServicePolicy::handleList):
    //   [count:u8] × [portKind:u8][portIdx:u8][roleKind:u8][flags:u8]
    s.numRoles = 0;
    if (len < 1) { s.handshake = Handshake::Failed; return; }
    uint8_t count = p[0];
    size_t  off   = 1;
    for (uint8_t k = 0; k < count && off + 4 <= len; ++k) {
        if (s.numRoles >= kMaxRolesPerExpander) {
            SFX_LOG_WARN("[Expander] role roster overflow addr=%u",
                         s.entry.usbAddr);
            break;
        }
        s.roles[s.numRoles++] = { p[off], p[off + 1], p[off + 2], p[off + 3] };
        off += 4;
    }

    SFX_LOG_INFO("[Expander] %s ready (%u ports / %u pre-attached roles)",
                 s.entry.spec.guid,
                 (unsigned)s.numPorts, (unsigned)s.numRoles);

    s.handshake          = Handshake::Ready;
    s.handshakeDeadlineMs = 0;
    s.batteryPollDueMs   = millis();   // first battery poll ASAP after Ready

    // Board is now accepting forwarded commands — let the master (re)apply
    // any configured role attachments for this GUID.  Fires AFTER the
    // Ready transition so callbacks may safely call attachRole / forward.
    if (_onReady) _onReady(s.entry);
}

// ─── Battery telemetry poll ─────────────────────────────────────────────
//
// For each Ready expander that advertised CoreCapability::BATTERY, issue a
// CorePacket::STATUS_REQ on a slow cadence and decode the battery section
// the board's onStatusData appended after the 22-byte core header.  Uses a
// dedicated capture target (`batteryResp`) so it never collides with the
// roster harvest's `pendingResp` (which is idle once Ready anyway).

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::tickBatteryPoll(
        uint8_t slotIdx) {
    if (slotIdx >= kMaxExpanders) return;
    LiveSlot& s = _live[slotIdx];
    if (!s.entry.spec.valid) return;
    if (!(s.entry.spec.capabilities & CoreCapability::BATTERY)) return;

    const uint32_t now = millis();

    // Capture a landed response.
    if (s.batteryQueryInflight && s.batteryResp.len > 0) {
        onBatteryStatus(slotIdx, s.batteryResp.payload, s.batteryResp.len);
        s.batteryResp          = SerialPacket{};
        s.batteryQueryInflight = false;
    }
    // In-flight timeout — give up on this round, retry next interval.
    if (s.batteryQueryInflight &&
        (int32_t)(now - s.batteryQueryDeadlineMs) > 0) {
        s.batteryQueryInflight = false;
    }
    // Kick a new poll when due and none in flight.
    if (!s.batteryQueryInflight && (int32_t)(now - s.batteryPollDueMs) >= 0) {
        s.batteryResp = SerialPacket{};
        auto rc = s.client.sendQuery(CorePacket::STATUS_REQ, nullptr, 0,
                                     CorePacket::STATUS, s.batteryResp);
        if (rc.success) {
            s.batteryQueryInflight   = true;
            s.batteryQueryDeadlineMs = now + kBatteryPollTimeoutMs;
        }
        s.batteryPollDueMs = now + kBatteryPollIntervalMs;
    }
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::onBatteryStatus(
        uint8_t slotIdx, const uint8_t* p, size_t len) {
    if (slotIdx >= kMaxExpanders) return;
    LiveSlot& s = _live[slotIdx];

    // STATUS = 22-byte core header + module-callback tail.  On our expander
    // boards onStatusData appends ONLY the battery section:
    //   [present:u8][voltage_mV:u16LE][cellCount:u8][pct:u8][flags:u8]
    constexpr size_t kHdr = CorePacket::STATUS_CORE_HEADER_SIZE;   // 22
    if (len < kHdr + 6) return;                                    // no battery tail
    const uint8_t* b = p + kHdr;
    s.battery.present    = b[0] != 0;
    s.battery.voltage_mV = (uint16_t)(b[1] | (b[2] << 8));
    s.battery.cellCount  = b[3];
    s.battery.pct        = b[4];
    s.battery.flags      = b[5];
    s.battery.valid      = true;
}

// ─── Outbound queue ─────────────────────────────────────────────────────

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
bool ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::queueCommand(
        uint8_t slotIdx, uint8_t type,
        const uint8_t* payload, uint8_t len) {
    if (slotIdx >= kMaxExpanders) return false;
    LiveSlot& s = _live[slotIdx];
    if (!s.entry.connected || s.handshake != Handshake::Ready) {
        ++s.droppedCount;
        return false;
    }
    if (len > kMaxQueuedPayload) {
        ++s.droppedCount;
        return false;
    }
    if (s.queueFull()) {
        ++s.droppedCount;
        return false;
    }
    QueuedCommand& c = s.queue[s.qTail];
    c.type = type;
    c.len  = len;
    if (len && payload) std::memcpy(c.payload, payload, len);
    s.qTail = (uint8_t)((s.qTail + 1) % kMaxQueuedPerSlot);
    return true;
}

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
void ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::flushQueue(
        uint8_t slotIdx) {
    if (slotIdx >= kMaxExpanders) return;
    LiveSlot& s = _live[slotIdx];

    // Drain in submission order — each command becomes its own
    // fire-and-forget CDC packet (effect path is non-blocking; ACK
    // tagging would just buffer-bloat the result queue).  Phase 6
    // wraps these into one `EXPANDER_BATCH` packet.
    while (!s.queueEmpty()) {
        const QueuedCommand& c = s.queue[s.qHead];
        // Tag 0 = TAG_ASYNC — server treats as fire-and-forget, no
        // ACK/NACK round-trip.  Failure surfaces only via drop counter
        // (process() will catch any inbound async errors separately).
        s.client.sendPacket(c.type, c.payload, c.len, SfxWire::TAG_ASYNC);
        s.qHead = (uint8_t)((s.qHead + 1) % kMaxQueuedPerSlot);
    }
}

}  // namespace hubfx::expanders

#endif  // HUBFX_EXPANDER_SERVICE_IPP
