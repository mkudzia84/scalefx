/*
 * ExpanderServicePolicyT — HubFX-side expander board manager.
 *
 *  Brings up the platform `UsbHost` stack, hooks its mount / unmount
 *  callbacks, runs an `IDENTIFY` handshake over CDC against every
 *  fresh device, harvests the GUID + spec (firmware version, platform,
 *  capabilities, build number) from the response, and surfaces those
 *  events to:
 *
 *    1. master firmware code via `onConnect()`, `onIdentified()`,
 *       and `onDisconnect()` — the .ino sketch installs these from
 *       setup() to react to expanders attaching / leaving;
 *    2. the upstream host (CLI / Studio) via async wire packets
 *       `EXPANDER_CONNECTED`, `EXPANDER_IDENTIFIED`,
 *       `EXPANDER_DISCONNECTED`, plus the synchronous
 *       `EXPANDER_LIST_REQ` → `EXPANDER_LIST_RESP` enumeration.
 *
 *  GUID-keyed persistence — every IDENTIFY response is also written
 *  into a separate `_known[MaxKnownGuids]` cache keyed by the board's
 *  4-hex-char GUID suffix (see CLAUDE.md "Board GUID").  The cache
 *  survives disconnect events for the rest of the boot session, so
 *  the master can re-apply per-board state when the same physical
 *  board reconnects on a different USB port.
 *
 *  Capability bits: USB_HOST | SLAVE_BUS — broadcast in IDENTIFY so
 *  Studio knows the master can host expanders.
 *
 *  Packet ownership: 0x80..0x84 (expander enumeration + lifecycle).
 *
 *  Template parameters:
 *    `MaxExpanders`    Number of concurrently-attached USB CDC devices
 *                      the policy will track.  HubFX has two USB-OTG
 *                      ports → set to 2.  Default 4 matches the
 *                      `sfx_usb` `USB_HOST_MAX_CDC_DEVICES` cap.
 *    `MaxKnownGuids`   Number of distinct GUIDs the policy remembers
 *                      after disconnect.  Defaults to `2 × MaxExpanders`
 *                      so the user can swap-and-restore at least one
 *                      complete generation of boards without losing
 *                      cached spec entries.
 */

#ifndef HUBFX_EXPANDER_SERVICE_H
#define HUBFX_EXPANDER_SERVICE_H

#include <Arduino.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>

#include <serial/core/core.h>          // CommandHandleResult, CoreCapability, CorePayload::decodeInitReady
#include <serial/wire.h>               // SfxWire::putU16LE / putU32LE / TAG_ASYNC
#include <serial/diag_log.h>           // SFX_LOG_*
#include <serial/client/bus_client.h>  // BusClient + sendIdentify
#include <server/board_server.h>       // sfx_core::BoardServerBase
#include <usb/sfx_usb_host.h>          // UsbHost, CdcDeviceInfo, USB_VID_RASPBERRY_PI, USB_PID_*

#include "expander_protocol.h"

namespace hubfx::expanders {

// ============================================================================
// ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>
// ============================================================================

template <uint8_t MaxExpanders = 4, uint8_t MaxKnownGuids = (uint8_t)(2 * MaxExpanders)>
class ExpanderServicePolicyT {
public:
    static constexpr uint32_t kCapabilityBits =
        CoreCapability::USB_HOST | CoreCapability::SLAVE_BUS;

    static constexpr uint8_t kMaxExpanders   = MaxExpanders;
    static constexpr uint8_t kMaxKnownGuids  = MaxKnownGuids;

    /// IDENTIFY response timeout — boards typically reply in <100 ms;
    /// we wait a little longer to absorb USB enumeration latency.
    static constexpr uint32_t kIdentifyTimeoutMs = 1500;

    using ConnectCallback     = std::function<void(const ExpanderEntry&)>;
    using IdentifiedCallback  = std::function<void(const ExpanderEntry&)>;
    using DisconnectCallback  = std::function<void(const ExpanderEntry&)>;

    /// A single retained spec entry — what we know about a GUID we've
    /// seen at least once since boot.  `connectedSlot` is `0xFF` when
    /// the board is currently unplugged.
    struct KnownGuid {
        ExpanderSpec spec;
        uint8_t      kind          = ExpanderKind::Unknown;
        uint8_t      connectedSlot = 0xFF;   ///< index into _live[] or 0xFF
        uint32_t     lastSeenMs    = 0;
    };

    ExpanderServicePolicyT() = default;

    // ── User-side subscription (installed before board.begin()) ────────

    /// Fires on USB mount, BEFORE the IDENTIFY round-trip.  Entry has
    /// `kind` / `usbAddr` / `vid` / `pid`; `spec.valid` is still false.
    void onConnect(ConnectCallback cb)        { _onConnect    = std::move(cb); }

    /// Fires once per slot when the IDENTIFY response is decoded —
    /// `entry.spec` is now populated (GUID, firmwareVersion, capabilities).
    /// This is the right hook to push cached config or bind a typed
    /// BusClient to a specific (kind, guid) pair.
    void onIdentified(IdentifiedCallback cb)  { _onIdentified = std::move(cb); }

    /// Fires on USB unmount.  The entry is a snapshot of the slot
    /// before it was cleared, so callbacks see the full spec (including
    /// the GUID) of the board that just left.
    void onDisconnect(DisconnectCallback cb)  { _onDisconnect = std::move(cb); }

    // ── Live-table introspection ────────────────────────────────────────

    const ExpanderEntry& entry(uint8_t i) const { return _live[i]; }
    static constexpr uint8_t count()            { return kMaxExpanders; }

    bool isConnected(uint8_t kind) const {
        for (uint8_t i = 0; i < kMaxExpanders; ++i) {
            if (_live[i].connected && _live[i].kind == kind) return true;
        }
        return false;
    }

    // ── GUID-keyed history (survives disconnect for the session) ──────

    /// Look up the persisted spec for a GUID we've previously seen.
    /// Returns nullptr if this is a brand-new GUID.
    const KnownGuid* findByGuid(const char* guid) const {
        if (!guid || !guid[0]) return nullptr;
        for (uint8_t i = 0; i < kMaxKnownGuids; ++i) {
            if (_known[i].spec.valid &&
                std::strncmp(_known[i].spec.guid, guid, sizeof(_known[i].spec.guid)) == 0) {
                return &_known[i];
            }
        }
        return nullptr;
    }

    /// Iterate the persisted-spec cache.  Slots with `.spec.valid == false`
    /// are empty; callers should skip them.
    const KnownGuid& knownAt(uint8_t i) const { return _known[i]; }
    static constexpr uint8_t knownCount()     { return kMaxKnownGuids; }

    // ── SystemServicePolicy surface ─────────────────────────────────────

    bool begin(sfx_core::BoardServerBase* ctx);

    bool ownsType(uint8_t type) const {
        return type == ExpanderPacket::EXPANDER_LIST_REQ;
    }

    CommandHandleResult handle(uint8_t type, const uint8_t* payload, size_t len);

    /// Per-loop tick — pumps every active CDC client so IDENTIFY
    /// responses get parsed, and checks for IDENTIFY timeouts.
    void update();

    const char* getErrorMessage(uint8_t code) const {
        return ExpanderError::getMessage(code);
    }

private:
    // Per-slot state ----------------------------------------------------
    /// Live registry slot — pairs an ExpanderEntry with the CDC client
    /// used to talk to that specific device.  BusClient is non-copyable
    /// (deletes copy/move), so we keep _live as an array of structs.
    struct LiveSlot {
        ExpanderEntry entry;
        BusClient     client;
        uint32_t      identifyDeadlineMs = 0;   ///< 0 = no IDENTIFY pending
    };

    // USB-host callback adapters ----------------------------------------
    void _onUsbMount  (uint8_t devAddr, uint16_t vid, uint16_t pid);
    void _onUsbUnmount(uint8_t devAddr);

    // Spec capture from IDENTIFY callback ------------------------------
    void onIdentifyResponse(uint8_t slotIdx);

    // Wire handlers -----------------------------------------------------
    void handleListReq();
    void emitConnected   (const ExpanderEntry& e);
    void emitIdentified  (const ExpanderEntry& e);
    void emitDisconnected(const ExpanderEntry& e);

    // Table mutation ----------------------------------------------------
    static uint8_t  classifyByVidPid(uint16_t vid, uint16_t pid);
    LiveSlot*       findFreeSlot();
    LiveSlot*       findByUsbAddr(uint8_t usbAddr);
    int             slotIndex(LiveSlot* s) const {
        return (s >= &_live[0] && s < &_live[kMaxExpanders]) ? (int)(s - &_live[0]) : -1;
    }
    void            clearEntry(ExpanderEntry& e);

    /// Pull the 4-hex-char GUID suffix out of `deviceName` (everything
    /// after the last `-`).  Output buffer must hold at least 5 bytes.
    static void     extractGuid(const char* deviceName, char outGuid[5]);

    /// Find or allocate a KnownGuid slot for `guid`; never returns
    /// nullptr while there's a free / stale slot to evict.  Eviction
    /// picks the oldest disconnected entry.
    KnownGuid*      acquireKnown(const char* guid);

    sfx_core::BoardServerBase* _ctx       = nullptr;
    bool                       _usbReady  = false;

    LiveSlot   _live[kMaxExpanders];
    KnownGuid  _known[kMaxKnownGuids];

    ConnectCallback    _onConnect;
    IdentifiedCallback _onIdentified;
    DisconnectCallback _onDisconnect;
};

// ============================================================================
// Default alias used by HubFX (2 ports → 2 live slots, 4 history slots).
// ============================================================================
using ExpanderServicePolicy = ExpanderServicePolicyT<2, 4>;

}  // namespace hubfx::expanders

#include "expander_service.ipp"

#endif  // HUBFX_EXPANDER_SERVICE_H
