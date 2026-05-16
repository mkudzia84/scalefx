/*
 * SlaveServer — generic BusServer for component-collection slaves.
 *
 * One slave board firmware:
 *
 *   #include <slave/slave_server.h>
 *   #include <slave/board_identifier.h>
 *   #include <collections/servo_collection.h>
 *   #include <collections/pwm_collection.h>
 *   #include <collections/led_collection.h>
 *
 *   // Compile-time component layout — this is what a "board" actually IS now.
 *   ServoCollection<4>              servos;
 *   PwmCollection<4>                pwms;
 *   LedCollection<8, NativeGpio>    leds;
 *   BoardIdentifier                 boardIdent;
 *
 *   SlaveServer<decltype(servos), decltype(pwms), decltype(leds)> slave;
 *
 *   void setup() {
 *       servos.configure({ ServoSpec{...}, ServoSpec{...}, ... });
 *       pwms  .configure({ PwmSpec{...},   ... });
 *       leds  .configure({ LedSpec{...},   ... }, &NativeGpio::instance());
 *
 *       boardIdent.load(readFileFn);                    // restore from /board.yaml
 *       server.begin("MyBoard", FW_VERSION, BUILD_NUM); // sfx core/server
 *       slave.bind(&servos, &pwms, &leds, &boardIdent);
 *       server.addModuleHandler(&slave);
 *
 *       // No autonomous activity — collections stay detached.  attach()
 *       // is called from inside slave on INIT(SLAVE)/INIT(DIRECT).
 *   }
 *
 *   void loop() {
 *       server.poll();
 *       slave.update();   // drives servo motion + LED runtime when attached
 *   }
 *
 * SlaveServer owns:
 *   - dispatch of all SlavePacket::* IDs
 *   - lifecycle: IDLE → SLAVE/DIRECT → IDLE (no STANDALONE — boards never
 *     run autonomously per the 2026-05-06 architectural pivot)
 *   - IDENT_GET / IDENT_SET (with persistence via injected file callbacks)
 *   - COMPONENT_LIST_RESP marshalling from the three collections'
 *     describe() helpers
 *
 * The collection types are templated, so any board can plug in different
 * counts (and a different LED GPIO provider) at compile time without
 * touching this file.
 */

#ifndef SFX_SLAVE_SERVER_H
#define SFX_SLAVE_SERVER_H

#include <cstdint>
#include <functional>
#include <type_traits>

#include <core.h>                          // CommandHandleResult, SerialError, CorePacket constants
#include <bus_server.h>                    // BusServer base
#include <serial/slave/slave.h>
#include <serial/slave/component_kind.h>   // ComponentKind / ComponentInfo (wire-format types)

#include <power/no_battery.h>              // NoBattery stub (default TBattery)
#include <platform/diag_log.h>             // DiagLog — LOG_MESSAGE / DIAG_HISTORY ring buffer

#include "board_identifier.h"              // sibling — sfx_slave/slave/

namespace sfx_slave {

/// Storage policy for identifier persistence — supplied by the board
/// firmware (LittleFS on Pico, equivalent on other targets).
struct IdentStorage {
    sfx_slave::BoardIdentifier::ReadFile  read;
    sfx_slave::BoardIdentifier::WriteFile write;
};

template <typename TServos, typename TPwms, typename TLeds, typename TBattery = NoBattery>
class SlaveServer : public BusServer {
public:
    /// Wire the server to its component collections + identifier handle.
    /// Call once after configure()-ing each collection.
    void bind(TServos*                       servos,
              TPwms*                         pwms,
              TLeds*                         leds,
              sfx_slave::BoardIdentifier*    ident,
              IdentStorage                   identStorage) {
        _servos = servos;
        _pwms   = pwms;
        _leds   = leds;
        _ident  = ident;
        _identStorage = identStorage;
    }

    /// Battery-aware overload — boards with `TBattery != NoBattery`
    /// pass the bound policy here so the server can dispatch
    /// BATTERY_INFO_REQ / BATTERY_RECONFIGURE, append the battery
    /// section to SLAVE_STATUS_BROADCAST, and emit BATTERY_ALERT
    /// async packets when the policy's state machine fires its
    /// low / critical callbacks.  The board's setup() should also
    /// `server.core().addCapability(CoreCapability::BATTERY)` so
    /// INIT_READY advertises the presence to the master.
    void bindBattery(TBattery* battery) { _battery = battery; }

    /// Drive the component runtimes — call frequently from loop() while
    /// the board is attached (i.e., after INIT).  Also polls the
    /// keepalive watchdog and triggers `enterSafeState()` if the
    /// timeout elapses while in SLAVE mode (DIRECT mode does not
    /// enforce keepalive — see refactor plan §"Safe-state on
    /// keepalive timeout").
    void update() {
        if (!_attached) return;
        if (_servos) _servos->update();
        if (_pwms)   _pwms  ->update();
        if (_leds)   _leds  ->update();
        // Battery sampling runs even outside attached state — it's
        // useful for the master to read voltage during enumeration
        // (before INIT) so it knows whether the slave is plausibly
        // powered.  The policy class owns its own rate-limiting.
        if constexpr (!std::is_same_v<TBattery, NoBattery>) {
            if (_battery) _battery->update();
        }
        checkKeepaliveTimeout();
        checkStatusBroadcast();
    }

    /// Park every output at its safe-state value and return the slave
    /// to IDLE.  Idempotent + atomic from the protocol's view.
    /// Triggered by:
    ///   - keepalive timeout in SLAVE mode
    ///   - explicit SHUTDOWN packet
    ///   - INIT(SLAVE) arriving while already in SLAVE (master rejoin)
    /// All collections are commanded to neutral synchronously; the
    /// motion profile then runs the servos there over the next ticks.
    /// Each collection emits ComponentEvent::SafeStateEntered for any
    /// per-channel hooks the board has registered (typically
    /// indicator LEDs going amber to flag "dead-master mode").
    void enterSafeState() {
        // Logged at warn so it shows up in DIAG_HISTORY without being
        // noisy during normal operation.  The cause (keepalive
        // timeout vs explicit SHUTDOWN vs re-INIT) is logged at the
        // call site so this single line carries every transition
        // without duplicating the cause.
        DiagLog::instance().warn("slave: enterSafeState (was %s)",
                                 _attached ? "attached" : "idle");
        if (_servos) _servos->parkAtNeutral();
        if (_pwms)   _pwms  ->allOff();
        if (_leds)   _leds  ->allOff();
        _attached = false;   // back to IDLE; component .update() becomes a no-op
    }

    /// Wire async-event callbacks from the component collections to
    /// the wire.  Called once during onInit() before attach().  The
    /// callbacks fire from the collection `update()` paths (i.e.,
    /// from the slave's main loop), so they run on the protocol task
    /// without locking and emit packets directly.
    void wireAsyncEvents() {
        if (_servos) {
            _servos->setTargetReachedCallback(
                [this](uint8_t idx, uint16_t pos_us) {
                    emitServoTargetReached(idx, pos_us);
                });
            _servos->setMotionUpdateCallback(
                [this](uint8_t idx, uint16_t pos, uint16_t target, int16_t vel) {
                    emitServoMotionUpdate(idx, pos, target, vel);
                });
        }
        if (_leds) {
            _leds->setProgramDoneCallback(
                [this](uint8_t addr, uint8_t progId) {
                    emitLedProgramDone(addr, progId);
                });
        }
        if (_pwms) {
            _pwms->setStallCallback(
                [this](uint8_t idx, uint16_t peak_mA, uint16_t duration_ms) {
                    emitPwmStall(idx, peak_mA, duration_ms);
                });
        }
        if constexpr (!std::is_same_v<TBattery, NoBattery>) {
            if (_battery) {
                _battery->onLowVoltage(
                    [this](uint16_t voltage_mV, uint8_t cellCount) {
                        emitBatteryAlert(SlavePacket::BatteryAlertLevel::LOW,
                                         voltage_mV, cellCount);
                    });
                _battery->onCriticalVoltage(
                    [this](uint16_t voltage_mV, uint8_t cellCount) {
                        emitBatteryAlert(SlavePacket::BatteryAlertLevel::CRITICAL,
                                         voltage_mV, cellCount);
                    });
            }
        }
    }

    // ── BusServer overrides ───────────────────────────────────────────
    const char* handlerName()      const override { return "SlaveServer"; }
    uint8_t     moduleRangeLow()   const override { return 0x01; }   // identity / status broadcast
    uint8_t     moduleRangeHigh()  const override { return 0x7F; }   // top of LED block
    CommandHandleResult handleModulePacket(uint8_t type,
                                           const uint8_t* payload,
                                           size_t len) override;
    const char* getModuleErrorMessage(uint8_t code) override;

    /// INIT lifecycle hook — wired via `SfxServer::onInit(callback)` in
    /// the board firmware.  `mode` is `BoardMode::SLAVE` or `DIRECT`.
    /// Attaches the component collections + wires async-event callbacks.
    void handleInit(uint8_t mode, uint8_t flags) {
        // Re-INIT while already attached is a master-rejoin signal —
        // park first so the slave starts the new session in a known
        // good state.
        if (_attached) {
            DiagLog::instance().info("slave: re-INIT received while attached — parking first");
            enterSafeState();
        }
        DiagLog::instance().info("slave: INIT mode=%u flags=0x%02x — attaching collections",
                                 (unsigned)mode, (unsigned)flags);
        _initMode          = mode;
        _lastKeepalive_ms  = millis();
        wireAsyncEvents();
        if (_servos) _servos->attach();
        if (_pwms)   _pwms  ->attach();
        if (_leds)   _leds  ->attach();
        _attached = true;
    }

    /// SHUTDOWN lifecycle hook — wired via `SfxServer::onShutdown(callback)`.
    /// Triggers safe-state and detaches.
    void handleShutdown() {
        DiagLog::instance().info("slave: SHUTDOWN received");
        enterSafeState();
        if (_servos) _servos->detach();
        if (_pwms)   _pwms  ->detach();
        if (_leds)   _leds  ->detach();
    }

    /// Note keepalive packet receipt — wired via `SfxServer::onKeepalive(...)`.
    void handleKeepalive() { noteMasterTraffic(); }

    /// Configurable rate for the unified SLAVE_STATUS_BROADCAST.
    ///   0 Hz       → disabled (default)
    ///   1..10 Hz   → emit periodic broadcast
    /// `kindsMask` filters which sections appear (see SlavePacket::
    /// StatusKinds — 0 = all).  Master writes via SLAVE_STATUS_RATE.
    void setStatusBroadcastRate(uint8_t hz, uint8_t kindsMask = 0) {
        _statusBroadcastRate_hz = hz;
        _statusKindsMask        = kindsMask;
    }

private:
    TServos*                            _servos       = nullptr;
    TPwms*                              _pwms         = nullptr;
    TLeds*                              _leds         = nullptr;
    TBattery*                           _battery      = nullptr;   ///< nullptr unless bindBattery() was called
    sfx_slave::BoardIdentifier*    _ident        = nullptr;
    IdentStorage                        _identStorage{};

    bool _attached = false;

    // Keepalive watchdog state.  Default timeout is set from the core
    // server's `keepaliveTimeout_ms` field at attach time so the policy
    // remains uniform across slaves.  Only enforced in SLAVE mode.
    uint8_t  _initMode               = 0;       ///< last INIT mode byte (SLAVE/DIRECT)
    uint32_t _lastKeepalive_ms       = 0;
    uint32_t _keepaliveTimeout_ms    = 2000;    ///< default — overridden by core

    // Unified-status broadcast state.
    uint8_t  _statusBroadcastRate_hz = 0;       ///< 0 = disabled
    uint8_t  _statusKindsMask        = 0;       ///< 0 = all kinds; otherwise StatusKinds bitmask
    uint32_t _lastStatusBroadcast_ms = 0;

    /// Called from `update()` once per loop iteration.  If we're in
    /// SLAVE mode and no master traffic has been seen for
    /// `_keepaliveTimeout_ms`, transitions to safe state.
    void checkKeepaliveTimeout();

    /// Reset the keepalive watchdog.  Called whenever any module-range
    /// or core-range packet is received from the master.
    void noteMasterTraffic();

    // Async-event emitters — TAG_ASYNC packets to master.
    void emitServoTargetReached(uint8_t idx, uint16_t pos_us);
    void emitServoMotionUpdate (uint8_t idx, uint16_t pos, uint16_t target, int16_t vel);
    void emitLedProgramDone    (uint8_t addr, uint8_t progId);
    void emitPwmStall          (uint8_t idx, uint16_t peak_mA, uint16_t duration_ms);
    void emitBatteryAlert      (uint8_t level, uint16_t voltage_mV, uint8_t cellCount);

    /// Build the status payload into `buf` and return the byte count.
    /// Used by both `emitStatusBroadcast()` (TAG_ASYNC) and
    /// `handleStatusReq()` (sync, with the master's tag).  `kindsMask`
    /// filters which sections are included; 0 = all.
    size_t buildStatusPayload(uint8_t* buf, size_t bufSize, uint8_t kindsMask) const;
    void   emitStatusBroadcast();
    void   checkStatusBroadcast();    ///< polled from update()
    bool   handleStatusReq(uint8_t kindsMask);   ///< 0x08 — sync, returns same payload as broadcast

    // Dispatch helpers (one per packet ID range).
    bool handleComponentList (const SerialPacket& pkt);
    bool handleIdentGet      (const SerialPacket& pkt);
    bool handleIdentSet      (const SerialPacket& pkt);
    bool handleServo         (const SerialPacket& pkt);
    /// Dispatches the entire 0x20..0x29 PWM range — including
    /// PWM_RECONFIGURE (0x27) and PWM_GET_CONFIG (0x28).  All non-
    /// reconfigure setters are convenience paths that call
    /// `_pwms->reconfigure()` under the hood with the relevant field
    /// updated, so the runtime never sees a half-applied state.
    bool handlePwm           (const SerialPacket& pkt);
    bool handleLed           (const SerialPacket& pkt);

    // Battery dispatch.  When TBattery == NoBattery the
    // BATTERY_INFO_REQ branch returns a stub `present=0` payload and
    // BATTERY_RECONFIGURE NACKs with `BATTERY_NOT_PRESENT`.  When
    // TBattery is a live policy these forward to the bound instance.
    bool handleBatteryInfo       (const SerialPacket& pkt);
    bool handleBatteryReconfigure(const SerialPacket& pkt);

    /// Append the battery section to the unified status payload.
    /// Always writes at least one byte (`present` flag).  Returns the
    /// number of bytes written, or 0 if `bufSize` is exhausted.
    size_t appendBatterySection(uint8_t* buf, size_t bufSize) const;
};

}  // namespace sfx_slave

#endif  // SFX_SLAVE_SERVER_H
