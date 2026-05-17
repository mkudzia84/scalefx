/*
 * ComponentServicePolicy — generic SystemServicePolicy for component-collection boards.
 *
 * Each slave board is one instantiation of this template parameterised
 * on its component types:
 *
 *   ServoCollection<N>               servos;
 *   PwmCollection<M, MySense>        pwms;
 *   LedRuntime<K, NativeGpio>        dedLeds;    // dedicated LED pool
 *   PwmDutyAdapter<decltype(pwms)>   pwmDuty(pwms);
 *   LedRuntime<M, decltype(pwmDuty)> borLeds;   // PWM-borrowed LED pool (optional)
 *   BoardIdentifier                  boardIdent;
 *
 *   ComponentServicePolicy<decltype(servos),
 *               decltype(pwms),
 *               decltype(dedLeds),
 *               decltype(borLeds)> slave;
 *
 *   slave.bind(&servos, &pwms, &dedLeds, &borLeds, &boardIdent, storage);
 *
 * Two LED pools:
 *   - dedicated   — always-on LEDs wired directly to a PwmOutput backend
 *                   (NativeGpio pins, PCA9685 channels, …)
 *   - PWM-borrowed — PwmCollection channels currently in PwmLed mode,
 *                    addressed via PwmDutyAdapter
 *
 * The wire-format LED address byte (`ComponentPacket::LedAddr`) carries
 * bit 7 = pool selector: `0` → dedicated, `1` → borrowed.  CoreServer
 * parses that bit, looks up the index, and dispatches into the right
 * LedRuntime.
 *
 * Mode-change coupling: when a PWM channel leaves PwmLed mode (master
 * sends PWM_SET_MODE to something other than PwmLed), CoreServer's
 * PwmCollection-event hook calls `borLeds.stopQueue(idx)` +
 * `borLeds.clearQueue(idx)` so no stale queue tries to drive a channel
 * that's now a motor or heater.
 *
 * Boards without PWM-borrowed LEDs declare `LedRuntime<0, void>` or
 * just `NoBorrowedLeds` (default).  All borrowed-pool branches compile
 * away via `if constexpr`.
 */

#ifndef SFX_COMPONENT_SERVICE_H
#define SFX_COMPONENT_SERVICE_H

#include <cstdint>
#include <functional>
#include <type_traits>

#include <core.h>                          // CommandHandleResult, SerialError, CorePacket constants
#include <serial/core/system_service.h>    // SystemServicePolicy concept + ServiceContext
#include <serial/components/components.h>
#include <serial/components/led_status.h>
#include <serial/components/component_kind.h>   // ComponentKind / ComponentInfo

#include <power/no_battery.h>              // NoBattery stub (default TBattery)
#include <platform/diag_log.h>             // DiagLog — LOG_MESSAGE / DIAG_HISTORY ring buffer

#include "board_identifier.h"              // sibling — sfx_core/core/

namespace sfx_core {

/// Stub type for boards without PWM-borrowed LED slots.  `if constexpr`
/// in CoreServer detects this and short-circuits every borrowed-pool
/// branch.  Zero memory, zero generated code.
struct NoBorrowedLeds {
    static constexpr size_t COUNT = 0;
};

/// Storage policy for identifier persistence — supplied by the board
/// firmware (LittleFS on Pico, equivalent on other targets).
struct IdentStorage {
    sfx_core::BoardIdentifier::ReadFile  read;
    sfx_core::BoardIdentifier::WriteFile write;
};

template <typename TServos, typename TPwms,
          typename TLedsDed,
          typename TLedsBor = NoBorrowedLeds,
          typename TBattery = NoBattery>
class ComponentServicePolicy {
public:
    /// Component service contributes no capability bit — the master
    /// discovers what components exist via COMPONENT_LIST_REQ.
    static constexpr uint32_t kCapabilityBits = 0u;

    /// Wire the server to its component collections + identifier handle.
    void bind(TServos*                       servos,
              TPwms*                         pwms,
              TLedsDed*                      ledsDed,
              TLedsBor*                      ledsBor,
              sfx_core::BoardIdentifier*    ident,
              IdentStorage                   identStorage) {
        _servos     = servos;
        _pwms       = pwms;
        _ledsDed    = ledsDed;
        _ledsBor    = ledsBor;
        _ident      = ident;
        _identStorage = identStorage;
    }

    void bindBattery(TBattery* battery) { _battery = battery; }

    /// Drive the component runtimes — call frequently from loop() while
    /// the board is attached.
    void update() {
        if (!_attached) return;
        if (_servos)   _servos ->update();
        if (_pwms)     _pwms   ->update();
        if (_ledsDed)  _ledsDed->update();
        if constexpr (!std::is_same_v<TLedsBor, NoBorrowedLeds>) {
            if (_ledsBor) _ledsBor->update();
        }
        if constexpr (!std::is_same_v<TBattery, NoBattery>) {
            if (_battery) _battery->update();
        }
        checkKeepaliveTimeout();
        checkStatusBroadcast();
    }

    /// Park every output at its safe-state value and return to IDLE.
    void enterSafeState() {
        DiagLog::instance().warn("expander: enterSafeState (was %s)",
                                 _attached ? "attached" : "idle");
        if (_servos)   _servos ->parkAtNeutral();
        if (_pwms)     _pwms   ->allOff();
        if (_ledsDed)  _ledsDed->allOff();
        if constexpr (!std::is_same_v<TLedsBor, NoBorrowedLeds>) {
            if (_ledsBor) _ledsBor->allOff();
        }
        _attached = false;
    }

    /// Wire async-event callbacks from the component collections to the
    /// wire.  Called once during onInit() before attach().
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
        if (_ledsDed) {
            _ledsDed->setQueueDoneCallback(
                [this](uint8_t idx) {
                    emitLedQueueDone(ComponentPacket::LedAddr::dedicated(idx));
                });
        }
        if constexpr (!std::is_same_v<TLedsBor, NoBorrowedLeds>) {
            if (_ledsBor) {
                _ledsBor->setQueueDoneCallback(
                    [this](uint8_t idx) {
                        emitLedQueueDone(ComponentPacket::LedAddr::pwmBorrowed(idx));
                    });
            }
        }
        if (_pwms) {
            _pwms->setStallCallback(
                [this](uint8_t idx, uint16_t peak_mA, uint16_t duration_ms) {
                    emitPwmStall(idx, peak_mA, duration_ms);
                });
            // Mode-change cleanup: when a PWM channel leaves PwmLed
            // mode, stop and clear any queue running on the
            // corresponding borrowed LED slot.  Without this, an
            // active LedEventSeq would keep ticking against a channel
            // now configured as a motor / heater.
            if constexpr (!std::is_same_v<TLedsBor, NoBorrowedLeds>) {
                _pwms->setEventCallback(
                    [this](uint8_t idx, sfx_peripherals::ComponentEvent ev, uint16_t data) {
                        if (ev != sfx_peripherals::ComponentEvent::ModeChanged) return;
                        const auto newMode =
                            static_cast<sfx_peripherals::ComponentKind>(data);
                        if (newMode != sfx_peripherals::ComponentKind::PwmLed
                            && _ledsBor) {
                            _ledsBor->stopQueue(idx);
                            _ledsBor->clearQueue(idx);
                        }
                    });
            }
        }
        if constexpr (!std::is_same_v<TBattery, NoBattery>) {
            if (_battery) {
                _battery->onLowVoltage(
                    [this](uint16_t voltage_mV, uint8_t cellCount) {
                        emitBatteryAlert(ComponentPacket::BatteryAlertLevel::LOW,
                                         voltage_mV, cellCount);
                    });
                _battery->onCriticalVoltage(
                    [this](uint16_t voltage_mV, uint8_t cellCount) {
                        emitBatteryAlert(ComponentPacket::BatteryAlertLevel::CRITICAL,
                                         voltage_mV, cellCount);
                    });
            }
        }
    }

    // ── SystemServicePolicy surface ───────────────────────────────────

    /// Cache the wire-helper context.  Called by BoardServer::begin()
    /// before any packet arrives.  No I/O — pure stash.
    bool begin(sfx_core::ServiceContext* ctx) {
        _ctx = ctx;
        return ctx != nullptr;
    }

    /// True iff this policy handles `type` — the component range 0x01-0x7F.
    bool ownsType(uint8_t type) const {
        return type >= 0x01 && type <= 0x7F;
    }

    /// Dispatch a component packet.  Type already checked via ownsType().
    CommandHandleResult handle(uint8_t type,
                               const uint8_t* payload, size_t len);

    /// Policy-specific error-code message lookup (used by BoardServer
    /// when assembling NACK payloads).  Returns nullptr if the code
    /// isn't owned by this policy — BoardServer falls back to the
    /// generic SerialError table or the next policy.
    const char* getErrorMessage(uint8_t code) const;

    /// INIT lifecycle hook.
    void handleInit(uint8_t mode, uint8_t flags) {
        if (_attached) {
            DiagLog::instance().info("expander: re-INIT received while attached — parking first");
            enterSafeState();
        }
        DiagLog::instance().info("expander: INIT mode=%u flags=0x%02x — attaching collections",
                                 (unsigned)mode, (unsigned)flags);
        _initMode          = mode;
        _lastKeepalive_ms  = millis();
        wireAsyncEvents();
        if (_servos)   _servos ->attach();
        if (_pwms)     _pwms   ->attach();
        // LedRuntime doesn't have a separate attach() — begin() at
        // setup() time already attached the channels.  The collection
        // base-class call below is unconditional in case of future
        // changes that add an attach() method.
        _attached = true;
    }

    /// SHUTDOWN lifecycle hook.
    void handleShutdown() {
        DiagLog::instance().info("expander: SHUTDOWN received");
        enterSafeState();
        if (_servos) _servos->detach();
        if (_pwms)   _pwms  ->detach();
    }

    /// Note keepalive packet receipt.
    void handleKeepalive() { noteMasterTraffic(); }

    /// Configurable rate for the unified COMPONENT_STATUS_BROADCAST.
    void setStatusBroadcastRate(uint8_t hz, uint8_t kindsMask = 0) {
        _statusBroadcastRate_hz = hz;
        _statusKindsMask        = kindsMask;
    }

private:
    sfx_core::ServiceContext*           _ctx          = nullptr;   ///< back-ref to BoardServer for ACK/NACK/sendRawPacket

    TServos*                            _servos       = nullptr;
    TPwms*                              _pwms         = nullptr;
    TLedsDed*                           _ledsDed      = nullptr;
    TLedsBor*                           _ledsBor      = nullptr;
    TBattery*                           _battery      = nullptr;
    sfx_core::BoardIdentifier*    _ident        = nullptr;
    IdentStorage                        _identStorage{};

    bool _attached = false;

    uint8_t  _initMode               = 0;
    uint32_t _lastKeepalive_ms       = 0;
    uint32_t _keepaliveTimeout_ms    = 2000;

    uint8_t  _statusBroadcastRate_hz = 0;
    uint8_t  _statusKindsMask        = 0;
    uint32_t _lastStatusBroadcast_ms = 0;

    void checkKeepaliveTimeout();
    void noteMasterTraffic();

    void emitServoTargetReached(uint8_t idx, uint16_t pos_us);
    void emitServoMotionUpdate (uint8_t idx, uint16_t pos, uint16_t target, int16_t vel);
    void emitLedQueueDone      (uint8_t addr);
    void emitPwmStall          (uint8_t idx, uint16_t peak_mA, uint16_t duration_ms);
    void emitBatteryAlert      (uint8_t level, uint16_t voltage_mV, uint8_t cellCount);

    size_t buildStatusPayload(uint8_t* buf, size_t bufSize, uint8_t kindsMask) const;
    void   emitStatusBroadcast();
    void   checkStatusBroadcast();
    bool   handleStatusReq(uint8_t kindsMask);

    bool handleComponentList (const SerialPacket& pkt);
    bool handleIdentGet      (const SerialPacket& pkt);
    bool handleIdentSet      (const SerialPacket& pkt);
    bool handleServo         (const SerialPacket& pkt);
    bool handlePwm           (const SerialPacket& pkt);
    bool handleLed           (const SerialPacket& pkt);
    bool handleBatteryInfo       (const SerialPacket& pkt);
    bool handleBatteryReconfigure(const SerialPacket& pkt);

    size_t appendBatterySection(uint8_t* buf, size_t bufSize) const;

    /// Resolve an LED address byte to (pool, idx).  Returns true on
    /// success.  The borrowed flag selects which LedRuntime the
    /// caller should dispatch into.  Bounds-checks against the
    /// matching pool's `COUNT`.  Returns false for indices outside
    /// the pool or for a borrowed address on a board with
    /// NoBorrowedLeds.
    bool resolveLedAddr(uint8_t addr, bool& outBorrowed, uint8_t& outIdx) const {
        if (ComponentPacket::LedAddr::isPwmBorrowed(addr)) {
            if constexpr (std::is_same_v<TLedsBor, NoBorrowedLeds>) return false;
            outBorrowed = true;
            outIdx = ComponentPacket::LedAddr::indexOf(addr);
            return outIdx < TLedsBor::COUNT;
        }
        outBorrowed = false;
        outIdx = ComponentPacket::LedAddr::indexOf(addr);
        return outIdx < TLedsDed::COUNT;
    }
};

}  // namespace sfx_core

#include "component_service.ipp"

#endif  // SFX_COMPONENT_SERVICE_H
