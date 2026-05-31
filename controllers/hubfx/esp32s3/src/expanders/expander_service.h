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

#include <concepts>
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

// ────────────────────────────────────────────────────────────────────
//  ExpanderService concept
// ────────────────────────────────────────────────────────────────────
//
// Contract every type passed as `TExpander` to `TopologyServicePolicyT`
// must satisfy.  `ExpanderServicePolicyT<MaxExpanders, MaxKnownGuids>`
// is the canonical model.  Topology calls into the expander service
// to walk live USB slots, look up a slot by GUID, and re-emit async
// CDC events into the unified `TOPOLOGY_ROLE_EVENT` stream.
//
template <typename T>
concept ExpanderService = requires(T& e, uint8_t slotIdx, const char* guid) {
    // Static traits — sizing parameters topology needs at compile time.
    { T::kMaxExpanders }         -> std::convertible_to<uint8_t>;
    { T::kMaxRolesPerExpander }  -> std::convertible_to<uint8_t>;

    // Slot access.  Topology iterates live slots for `TOPOLOGY_PORT_LIST`
    // and `TOPOLOGY_ROLE_LIST` enumeration responses.
    { e.liveSlot(slotIdx) };
    { e.findLiveIdxByGuid(guid) } -> std::convertible_to<uint8_t>;

    // Async-event subscription — topology re-emits expander asyncs.
    typename T::Handshake;
};

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
    using ReadyCallback       = std::function<void(const ExpanderEntry&)>;
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

    /// Fires once the roster harvest (PORT_LIST → ROLE_LIST) completes and
    /// the slot reaches `Handshake::Ready` — i.e. the board is now
    /// accepting forwarded commands (the outbound queue gates on Ready).
    /// This is the right hook to (re)apply configured role attachments to
    /// an expander, since `onIdentified` fires BEFORE Ready and any
    /// command sent then is dropped.
    void onReady(ReadyCallback cb)            { _onReady = std::move(cb); }

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
        return type == ExpanderPacket::EXPANDER_LIST_REQ
            || type == ExpanderPacket::EXPANDER_SYSTEM_INFO_REQ;
    }

    CommandHandleResult handle(uint8_t type, const uint8_t* payload, size_t len);

    /// Per-loop tick — pumps every active CDC client so IDENTIFY
    /// responses get parsed, and checks for IDENTIFY timeouts.
    void update();

    const char* getErrorMessage(uint8_t code) const {
        return ExpanderError::getMessage(code);
    }

public:
    /// Cached port-roster entry — one per port the expander reports
    /// in its `PORT_LIST_RESP`.  Mirrored on the master so we don't
    /// re-query the expander every time Studio asks for the topology.
    /// `voltageMv` was added in Phase 0 of the GunFX rollout
    /// (instructions/22) — the rail voltage the expander declared for
    /// this port; 0 = unknown.
    struct PortRosterEntry {
        uint8_t  kind;       ///< PortKind::Servo / Pwm / HBridge / Input
        uint8_t  idx;
        uint8_t  flags;      ///< capability/sense flags (per-kind semantics)
        uint16_t voltageMv;  ///< rail voltage (mV), 0 = unknown
    };

    /// Cached role attachment — one per port that currently has a role
    /// emplaced on the expander side.  Refreshed after every successful
    /// remote ROLE_ATTACH / ROLE_DETACH command we forward.
    struct RoleRosterEntry {
        uint8_t portKind;
        uint8_t portIdx;
        uint8_t roleKind;
        uint8_t flags;
    };

    /// Per-slot rosters.  Port roster has a fixed cap that comfortably
    /// holds the largest expander (16 PWM + 4 servo + 2 HBridge + 1
    /// input = 23 entries) with headroom.  Role roster cap is the same
    /// since each port can host at most one role.
    static constexpr uint8_t kMaxPortsPerExpander = 32;
    static constexpr uint8_t kMaxRolesPerExpander = 32;

    /// Outbound queued command — drained at the end of every tick into
    /// individual CDC packets (Phase 6 will fold these into a single
    /// `EXPANDER_BATCH` wrapper).  Real-time effect paths push here;
    /// Studio-driven config paths bypass the queue and forward
    /// synchronously with timeout.
    static constexpr uint8_t kMaxQueuedPerSlot   = 16;
    static constexpr uint8_t kMaxQueuedPayload   = 24;
    struct QueuedCommand {
        uint8_t type;
        uint8_t len;
        uint8_t payload[kMaxQueuedPayload];
    };

    /// Handshake state — drives the post-IDENTIFY roster harvest.
    enum class Handshake : uint8_t {
        Idle = 0,        ///< slot empty
        Identifying,     ///< IDENTIFY round-trip in flight
        FetchingPorts,   ///< PORT_LIST_REQ in flight
        FetchingRoles,   ///< ROLE_LIST_REQ in flight
        Ready,           ///< spec + ports + roles all cached
        Failed,          ///< handshake errored out (logged + flagged)
    };

    /// Live registry slot — pairs an ExpanderEntry with the CDC client
    /// used to talk to that specific device.  The `gen` counter bumps
    /// on every mount / unmount transition so callers that cache a
    /// `(slotIdx, gen)` token can detect a reused slot after disconnect
    /// without scanning the live table again.  (See the slot-token
    /// pattern in the topology design notes.)
    struct LiveSlot {
        ExpanderEntry entry;
        BusClient     client;
        uint32_t      identifyDeadlineMs = 0;   ///< 0 = no IDENTIFY pending
        uint16_t      gen                = 0;   ///< slot generation (mount/unmount edge counter)

        // ── Post-IDENTIFY handshake state machine ────────────────────
        Handshake     handshake          = Handshake::Idle;
        uint32_t      handshakeDeadlineMs = 0;
        SerialPacket  pendingResp;              ///< capture target for non-blocking sendQuery

        // ── Cached rosters ───────────────────────────────────────────
        PortRosterEntry ports[kMaxPortsPerExpander];
        uint8_t         numPorts = 0;
        RoleRosterEntry roles[kMaxRolesPerExpander];
        uint8_t         numRoles = 0;

        // ── Battery telemetry (BATTERY-capable expanders only) ───────
        // The hub polls each Ready expander's CorePacket::STATUS_REQ on a
        // slow cadence and decodes the battery section the expander's
        // onStatusData appended (offset = STATUS_CORE_HEADER_SIZE).
        struct BatteryInfo {
            bool     valid      = false;   ///< a STATUS with a battery tail was decoded
            bool     present    = false;
            uint16_t voltage_mV = 0;
            uint8_t  cellCount  = 0;
            uint8_t  pct        = 0;
            uint8_t  flags      = 0;        ///< bit0 = low, bit1 = critical
        } battery;
        SerialPacket batteryResp;               ///< capture target for the battery STATUS poll
        bool         batteryQueryInflight = false;
        uint32_t     batteryPollDueMs     = 0;  ///< next poll time (millis)
        uint32_t     batteryQueryDeadlineMs = 0;

        // ── Outbound command queue (effect path) ─────────────────────
        QueuedCommand   queue[kMaxQueuedPerSlot];
        uint8_t         qHead = 0;
        uint8_t         qTail = 0;
        uint32_t        droppedCount = 0;       ///< queue overflows since boot

        bool queueEmpty() const { return qHead == qTail; }
        bool queueFull()  const { return (uint8_t)((qTail + 1) % kMaxQueuedPerSlot) == qHead; }
    };

    /// Direct access for higher-level routing layers (TopologyService).
    /// Returns nullptr if `slotIdx >= kMaxExpanders`.
    LiveSlot*       liveSlot(uint8_t slotIdx)       {
        return (slotIdx < kMaxExpanders) ? &_live[slotIdx] : nullptr;
    }
    const LiveSlot* liveSlot(uint8_t slotIdx) const {
        return (slotIdx < kMaxExpanders) ? &_live[slotIdx] : nullptr;
    }

    /// Resolve `guid` → live slot index (0..kMaxExpanders-1) for use
    /// in slot-tokens.  Returns 0xFF when the GUID isn't currently
    /// connected.  Returned slot's `gen` is the token's expected gen.
    uint8_t findLiveIdxByGuid(const char* guid) const {
        if (!guid || !guid[0]) return 0xFF;
        for (uint8_t i = 0; i < kMaxExpanders; ++i) {
            const auto& s = _live[i];
            if (s.entry.connected && s.entry.spec.valid &&
                std::strncmp(s.entry.spec.guid, guid, sizeof(s.entry.spec.guid)) == 0) {
                return i;
            }
        }
        return 0xFF;
    }

    /// Push an outbound role/port command onto a slot's queue.  The
    /// flusher in `update()` drains the queue at the end of every tick.
    /// Returns false (and bumps the slot's drop counter) when the queue
    /// is full or `slotIdx` is invalid / disconnected.
    bool queueCommand(uint8_t slotIdx, uint8_t type,
                      const uint8_t* payload, uint8_t len);

    /// Async-event hook — fires for every TAG_ASYNC packet received
    /// from any expander.  Used by `TopologyServicePolicy` to re-emit
    /// role events (SERVO_TARGET_REACHED, RCIN_VALUE_BROADCAST, ...)
    /// upstream wrapped in a `TOPOLOGY_ROLE_EVENT` packet.
    using ExpanderAsyncCallback =
        std::function<void(uint8_t slotIdx, uint8_t type,
                           const uint8_t* payload, size_t len)>;
    void onExpanderAsync(ExpanderAsyncCallback cb) { _onExpanderAsync = std::move(cb); }

    /// Resolve `guid` → live slot pointer (currently connected only).
    /// Returns nullptr when the GUID isn't online right now — callers
    /// hitting this path should consult `findByGuid()` for the cached
    /// history entry.
    LiveSlot* findLiveByGuid(const char* guid) {
        if (!guid || !guid[0]) return nullptr;
        for (uint8_t i = 0; i < kMaxExpanders; ++i) {
            const auto& s = _live[i];
            if (s.entry.connected && s.entry.spec.valid &&
                std::strncmp(s.entry.spec.guid, guid, sizeof(s.entry.spec.guid)) == 0) {
                return &_live[i];
            }
        }
        return nullptr;
    }

private:

    // USB-host callback adapters ----------------------------------------
    void _onUsbMount  (uint8_t devAddr, uint16_t vid, uint16_t pid);
    void _onUsbUnmount(uint8_t devAddr);

    // Spec capture from IDENTIFY callback ------------------------------
    void onIdentifyResponse(uint8_t slotIdx);

    // Roster harvest ----------------------------------------------------
    void kickFetchPorts (uint8_t slotIdx);
    void kickFetchRoles (uint8_t slotIdx);
    void onPortListResp (uint8_t slotIdx, const uint8_t* p, size_t len);
    void onRoleListResp (uint8_t slotIdx, const uint8_t* p, size_t len);

    // Battery telemetry poll (BATTERY-capable Ready slots) --------------
    // Slow STATUS_REQ cadence; decodes the battery tail the expander's
    // onStatusData appends after the 22-byte core STATUS header.
    static constexpr uint32_t kBatteryPollIntervalMs = 3000;
    static constexpr uint32_t kBatteryPollTimeoutMs  = 500;
    void tickBatteryPoll   (uint8_t slotIdx);
    void onBatteryStatus   (uint8_t slotIdx, const uint8_t* p, size_t len);

    // Outbound queue flush ---------------------------------------------
    void flushQueue     (uint8_t slotIdx);

    // Wire handlers -----------------------------------------------------
    void handleListReq();
    void handleSystemInfoReq();
    void emitConnected   (const ExpanderEntry& e);
    void emitIdentified  (const ExpanderEntry& e);
    void emitDisconnected(const ExpanderEntry& e);
    void emitCollision   (const char* guid, uint8_t addrA, uint8_t addrB);

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

    ConnectCallback       _onConnect;
    IdentifiedCallback    _onIdentified;
    ReadyCallback         _onReady;
    DisconnectCallback    _onDisconnect;
    ExpanderAsyncCallback _onExpanderAsync;
};

// ============================================================================
// Default alias used by HubFX (2 ports → 2 live slots, 4 history slots).
// ============================================================================
using ExpanderServicePolicy = ExpanderServicePolicyT<2, 4>;

}  // namespace hubfx::expanders

#include "expander_service.ipp"

#endif  // HUBFX_EXPANDER_SERVICE_H
