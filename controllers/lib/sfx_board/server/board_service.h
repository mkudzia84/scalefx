/*
 * BoardServicePolicy — lifecycle + identify + status protocol policy.
 *
 * Absorbs the wire-protocol responsibilities of the legacy
 * `CoreCommandServer`: INIT / SHUTDOWN / REBOOT / BOOTSEL / KEEPALIVE /
 * STATUS / STATUS_REQ / IDENTIFY / I2C_SCAN / DIAG_HISTORY /
 * STATUS_UPDATE / BATTERY_CONFIG (legacy 0xEE — chemistry + cell count).
 *
 * Satisfies `sfx_core::SystemServicePolicy` so it sits in a
 * `BoardServer<...>` policy pack alongside the rest of the board's
 * services.  Holds the board info (name / version / platform / cpuMHz
 * / freeRam / buildNumber / capabilities), the keepalive timer, the
 * status-broadcast machinery, and the per-event callbacks.
 *
 * Range owned: `0xEE..0xFF` (CorePacket lifecycle range + the legacy
 * BATTERY_CONFIG byte at 0xEE that pre-dates the component-side
 * BatteryServicePolicy).
 *
 * Wave-4 deliverable: this class replaces `CoreCommandServer` entirely
 * once `SfxServer` is templatised.  Wave 5 then deletes `BusServer` /
 * `ICommandHandler` / `CommandRouter` since `BoardServer` is the only
 * dispatcher and routing is fully compile-time-resolved through the
 * policy tuple.
 */

#ifndef SFX_BOARD_SERVICE_H
#define SFX_BOARD_SERVICE_H

#include <cstdint>
#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include <cstddef>

#include <serial/core/core.h>  // CorePacket / CoreError / CoreBoardInfo / I2CScanResult / callbacks

namespace sfx { class Stream; }

namespace sfx_core {

class BoardServerBase;  // defined in board_server.h

class BoardServicePolicy {
public:
    /// BoardServicePolicy contributes no capability bit of its own — the
    /// capability bitmask advertised in IDENTIFY is the OR of every
    /// OTHER policy's `kCapabilityBits`, which `BoardServer` aggregates
    /// at compile time and seeds via `setCapabilities()` below.
    static constexpr uint32_t kCapabilityBits = 0u;

    BoardServicePolicy() = default;

    // ── SystemServicePolicy surface ───────────────────────────────────

    bool begin(BoardServerBase* ctx) {
        _ctx = ctx;
        _initReceived = false;
        _lastActivityMs = 0;
        _prevActivityMs = 0;
        return _ctx != nullptr;
    }

    bool ownsType(uint8_t type) const {
        // 0xEF..0xFF lifecycle range only.  0xEE (BATTERY_CONFIG) is NOT
        // claimed here — BoardService never handled it, and the
        // auto-prepend put this policy first in the pack, so claiming 0xEE
        // ate BATTERY_CONFIG before BatteryServicePolicy (which owns it)
        // could see it.  Dropping 0xEE lets battery config dispatch.
        return type >= 0xEF && type <= 0xFF;
    }

    CommandHandleResult handle(uint8_t type, const uint8_t* payload, size_t len);

    /// Ticked by `BoardServer::update()` — drives the verbose-mode
    /// status broadcast at the configured interval.
    void update() { tickStatusBroadcast(); }

    const char* getErrorMessage(uint8_t code) const {
        // Resolve only codes inside the SerialError-allocated ranges
        // (CLAUDE.md: 0x00..0x1F generic + 0xF0..0xFF system).  For
        // codes outside those ranges, return nullptr so the
        // `aggregateErrorMessage` fold can ask the next policy
        // (EngineError 0x70, GearError 0x60, etc.).  Returning
        // SerialError::getMessage unconditionally short-circuited the
        // aggregator because its default case yielded the bogus
        // "Domain-specific error" string for 0x20..0x7F — the EngineFx
        // / GearControl policies never got asked.
        if (code <= 0x1F || code >= 0xF0) {
            return SerialError::getMessage(code);
        }
        return nullptr;
    }

    // ── Board info & capabilities ─────────────────────────────────────

    void setBoardInfo(const char* deviceName, const char* firmwareVersion,
                      const char* platform, uint32_t cpuMHz, uint32_t freeRam,
                      uint32_t buildNumber = 0);

    void     setCapabilities(uint32_t caps) { _boardInfo.capabilities = caps; }
    void     addCapability  (uint32_t bits) { _boardInfo.capabilities |= bits; }
    uint32_t capabilities() const           { return _boardInfo.capabilities; }

    /// Runtime-enabled subset of `capabilities`.  Seeded by
    /// `BoardServer::recomputeEnabledCapabilities()` which walks every
    /// user policy and checks the optional `bool enabled() const`
    /// accessor (defaults to "always enabled" when the policy doesn't
    /// expose one).  The HubFxConfigServicePolicy re-runs this after
    /// every `applyConfig()` so the host's view of which features are
    /// live tracks the YAML.
    void     setEnabledCapabilities(uint32_t caps) { _boardInfo.enabledCapabilities = caps; }
    void     addEnabledCapability  (uint32_t bits) { _boardInfo.enabledCapabilities |= bits; }
    uint32_t enabledCapabilities() const           { return _boardInfo.enabledCapabilities; }

    /// Read-only access to the locally-stored CoreBoardInfo (device
    /// name, version, platform, capabilities, build).  Lets other
    /// policies (e.g. ExpanderServicePolicy's SYSTEM_INFO packet) splice
    /// the hub's own identify block into a unified response without
    /// having to re-derive every field.
    const CoreBoardInfo& boardInfo() const { return _boardInfo; }

    void updateFreeRam(uint32_t freeRam) { _boardInfo.freeRamBytes = freeRam; }

    // ── Activity / keepalive ─────────────────────────────────────────

    void          updateActivity() { _lastActivityMs = SFX_MILLIS(); }
    bool          checkTimeout(unsigned long timeoutMs);
    unsigned long lastActivityMs() const { return _lastActivityMs; }
    bool          isInitialized()  const { return _initReceived; }
    bool          isVerbose()      const { return (_initFlags & InitFlags::VERBOSE) != 0; }

    // ── Board state ──────────────────────────────────────────────────

    uint8_t boardState() const           { return _boardState; }
    void    setBoardState(uint8_t state) { _boardState = state; }

    void setTransferActive(bool active)  { _transferActive = active; }
    bool isTransferActive() const        { return _transferActive; }

    void reset();

    // ── Callbacks ────────────────────────────────────────────────────

    void onInit     (CoreInitCallback      cb) { _initCallback      = cb; }
    void onShutdown (CoreShutdownCallback  cb) { _shutdownCallback  = cb; }
    void onReboot   (CoreRebootCallback    cb) { _rebootCallback    = cb; }
    void onBootsel  (CoreBootselCallback   cb) { _bootselCallback   = cb; }
    void onKeepalive(CoreKeepaliveCallback cb) { _keepaliveCallback = cb; }
    void onStatusData(StatusDataCallback   cb) { _statusDataCallback = cb; }
    void onI2CScan  (I2CScanCallback       cb) { _i2cScanCallback   = cb; }

    // ── Statistics ───────────────────────────────────────────────────

    uint32_t commandCounter()   const { return _commandCounter; }
    uint32_t keepaliveCounter() const { return _keepaliveCounter; }

    // ── Wire emitters ────────────────────────────────────────────────

    void sendI2CScanResult(const I2CScanResult& result);

    /// Verbose-mode async telemetry envelope (TAG_ASYNC).
    void sendStatusUpdate(uint8_t source, uint8_t updateType,
                          const uint8_t* data = nullptr, size_t dataLen = 0);

    void setStatusBroadcastInterval(uint32_t interval_ms) { _statusBroadcastInterval_ms = interval_ms; }
    void setStatusBroadcastSource(uint8_t source)         { _statusBroadcastSource      = source; }
    void tickStatusBroadcast();

protected:
    // Wire-helper wrappers — defined out-of-line in board_service.cpp
    // (BoardServerBase is forward-declared here).  Handler bodies +
    // SFX_REQUIRE_LEN / SFX_DISPATCH macros call these unchanged.
    int     sendAck();
    int     sendNack(uint8_t errorCode, const char* reason = nullptr);
    int     sendRawPacket(uint8_t type, uint8_t tag,
                          const uint8_t* payload = nullptr, size_t len = 0);
    uint8_t      currentTag() const;
    sfx::Stream* serial() const;

private:
    void sendInitReady();
    void sendIdentify();
    void handleInit(const uint8_t* payload, size_t len);
    void sendStatus();

    BoardServerBase* _ctx = nullptr;

    CoreBoardInfo _boardInfo;
    bool          _initReceived   = false;
    uint8_t       _initFlags      = InitFlags::NONE;
    uint8_t       _boardState     = BoardState::IDLE;
    bool          _transferActive = false;
    unsigned long _lastActivityMs = 0;
    unsigned long _prevActivityMs = 0;
    uint32_t      _commandCounter   = 0;
    uint32_t      _keepaliveCounter = 0;

    uint32_t      _statusBroadcastInterval_ms = 200;
    unsigned long _lastStatusBroadcast_ms     = 0;
    uint8_t       _statusBroadcastSource      = StatusUpdateSource::CORE;

    CoreInitCallback      _initCallback;
    CoreShutdownCallback  _shutdownCallback;
    CoreRebootCallback    _rebootCallback;
    CoreBootselCallback   _bootselCallback;
    CoreKeepaliveCallback _keepaliveCallback;
    StatusDataCallback    _statusDataCallback;
    I2CScanCallback       _i2cScanCallback;
};

}  // namespace sfx_core

#endif  // SFX_BOARD_SERVICE_H
