/*
 * ExpanderServicePolicyT — template-method definitions.
 *
 *   Included at the bottom of expander_service.h.  Kept separate so the
 *   header stays readable and the IDE can navigate signatures without
 *   wading through the implementation.
 */

#ifndef HUBFX_EXPANDER_SERVICE_IPP
#define HUBFX_EXPANDER_SERVICE_IPP

namespace hubfx::expanders {

// ─── Lifecycle ──────────────────────────────────────────────────────────

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
bool ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::begin(
        sfx_core::BoardServerBase* ctx) {
    _ctx = ctx;
    if (!_ctx) return false;

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

    if (slot->client.sendIdentify() < 0) {
        SFX_LOG_WARN("[Expander] sendIdentify() failed for addr=%u", devAddr);
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
        // and (later) any other typed traffic routed through this slot.
        s.client.process();

        // IDENTIFY watchdog.
        if (s.entry.identifying && s.identifyDeadlineMs &&
            (int32_t)(now - s.identifyDeadlineMs) > 0) {
            SFX_LOG_WARN("[Expander] IDENTIFY timeout for addr=%u — retrying",
                         s.entry.usbAddr);
            s.entry.identifying     = false;
            s.identifyDeadlineMs    = 0;
            // One retry attempt — if it fails again we leave the slot
            // un-identified; a host can still discover it via the
            // `EXPANDER_CONNECTED` event + USB vid/pid.
            if (s.client.sendIdentify() >= 0) {
                s.entry.identifying     = true;
                s.identifyDeadlineMs    = now + kIdentifyTimeoutMs;
            }
        }
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

    // Persist into the GUID-keyed history (replaces or evicts as needed).
    if (KnownGuid* k = acquireKnown(spec.guid)) {
        k->spec          = spec;
        k->kind          = s.entry.kind;
        k->connectedSlot = slotIdx;
        k->lastSeenMs    = millis();
    }

    SFX_LOG_INFO("[Expander] IDENTIFIED %s guid=%s fw=%s caps=0x%08lx build=%lu",
                 ExpanderKind::getName(s.entry.kind),
                 spec.guid,
                 spec.firmwareVersion,
                 (unsigned long)spec.capabilities,
                 (unsigned long)spec.buildNumber);

    if (_onIdentified) _onIdentified(s.entry);
    emitIdentified(s.entry);
}

// ─── Wire dispatch ──────────────────────────────────────────────────────

template <uint8_t MaxExpanders, uint8_t MaxKnownGuids>
CommandHandleResult ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>::handle(
        uint8_t type, const uint8_t* /*payload*/, size_t /*len*/) {
    switch (type) {
        case ExpanderPacket::EXPANDER_LIST_REQ:
            handleListReq();
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
        buf[off++] = e.spec.valid ? 1 : 0;

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

}  // namespace hubfx::expanders

#endif  // HUBFX_EXPANDER_SERVICE_IPP
