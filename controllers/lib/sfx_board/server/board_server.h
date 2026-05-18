/*
 * BoardServer<...UserPolicies> — the single board-side firmware composer.
 *
 * One class owns:
 *   - the Stream connection (UART0 / USB CDC),
 *   - the COBS frame reader,
 *   - the policy tuple (BoardServicePolicy + IndicatorServicePolicy +
 *     UserPolicies...),
 *   - the wire helpers (sendAck / sendNack / sendRawPacket),
 *   - the device-name building + I²C-scan helpers,
 *   - the connection-timeout watchdog + lifecycle callbacks.
 *
 * No separate "SfxServer wrapper" / "PacketReader" / "ServiceContext"
 * interfaces — those were three layers around the same thing and have
 * been collapsed.
 *
 * Usage (HubFX-style master):
 *
 *   using HubFxBoard = sfx_core::BoardServer<
 *       AudioServicePolicy<Mixer>,
 *       StorageServicePolicy<Esp32StoragePolicy>,
 *       BatteryServicePolicy<Ina226Battery>,
 *       UsbHostServicePolicy,
 *       EngineServicePolicy,
 *       ConfigServicePolicy>;
 *
 *   HubFxBoard board;
 *
 *   void setup() {
 *       board.begin("HubFx", FIRMWARE_VERSION, BUILD_NUMBER);
 *       board.setConnectionTimeoutEnabled(false);          // master: no watchdog
 *       board.policy<BatteryServicePolicy<Ina226Battery>>().bindBattery(...);
 *   }
 *
 *   void loop() {
 *       board.process();
 *   }
 *
 * Accessors:
 *   board.core()        → BoardServicePolicy&     (lifecycle / status / INIT)
 *   board.indicators()  → IndicatorServicePolicy& (connection / error LEDs)
 *   board.policy<P>()   → P&                       (any policy in the pack)
 *
 * Per-policy contract (sfx_core::SystemServicePolicy concept):
 *   - `static constexpr uint32_t kCapabilityBits`  (OR'd into IDENTIFY caps)
 *   - `bool begin(BoardServerBase*)`               (cache context, init state)
 *   - `bool ownsType(uint8_t) const`               (claim a packet type byte)
 *   - `CommandHandleResult handle(uint8_t type,
 *                                 const uint8_t* p, size_t len)`
 *   - `void update()`                              (tick from loop())
 */

#ifndef SFX_BOARD_SERVER_H
#define SFX_BOARD_SERVER_H

#include <Arduino.h>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <tuple>
#include <type_traits>

#include <serial/core/core.h>     // CommandHandleResult, SerialError, CorePacket, CoreBoardInfo, I2CScanResult
#include <serial/wire.h>          // SfxWire (CRC-8 / COBS / encode / parse)
#include <serial/diag_log.h>      // DiagLog (LOG_MESSAGE / DIAG_HISTORY)
#include <platform/sfx_platform.h>  // SFX_PLATFORM_*, SFX_CPU_MHZ, SFX_FREE_HEAP, SFX_REBOOT, sfxGetBoardId

#include "board_service.h"
#include <indicators/indicator_leds.h>

// Forward declarations — avoid pulling sfx_peripherals headers from the API surface.
class I2CDevice;
class TwoWire;

namespace sfx_core {

class PortRegistryBase;   // defined in port_registry.h

// ============================================================================
// BoardServerBase — non-template state + concrete wire helpers
// ============================================================================
//
// Holds everything that doesn't depend on the policy pack: the Stream,
// the rx framer state, the device name, the I²C scan table, the
// timeout / config-loaded flags, the lifecycle callbacks, the per-frame
// correlation tag, and the concrete sendAck / sendNack / sendRawPacket
// wire helpers (no virtual dispatch — single firmware instance per board).
//
// Two virtual hooks reach into the templated subclass:
//   - dispatchPacket(type, tag, payload, len) — walk the policy tuple,
//     NACK fallback.  Called once per CRC-valid frame.
//   - aggregateErrorMessage(code)             — ask each policy for its
//     error-code string.  Called from sendNack when the caller did not
//     pass an inline reason.
//
class BoardServerBase {
public:
    static constexpr uint32_t      BAUD_RATE              = 6000000;
    static constexpr unsigned long CONNECTION_TIMEOUT_ms  = 15000;
    static constexpr uint8_t       MAX_EXPECTED_I2C       = 8;
    static constexpr size_t        RX_BUFFER_SIZE         = SfxWire::COBS_BUFFER_SIZE;

    BoardServerBase() = default;
    virtual ~BoardServerBase() = default;
    BoardServerBase(const BoardServerBase&) = delete;
    BoardServerBase& operator=(const BoardServerBase&) = delete;

    // ── Identity / introspection ─────────────────────────────────────

    const char* deviceName() const { return _deviceName; }
    Stream*     stream()     const { return _serial; }
    bool        isInitialized() const { return _initialized; }

    // ── Watchdog / lifecycle flags ───────────────────────────────────

    /// Master controllers (HubFX) call this with `false` to disable the
    /// keepalive-inactivity watchdog.  Expanders leave it enabled.
    void setConnectionTimeoutEnabled(bool enabled) {
        _timeoutEnabled       = enabled;
        _timeoutEnabledByUser = enabled;
    }

    /// True iff a config file has been loaded — flips post-disconnect
    /// state from IDLE to STANDALONE.
    bool isConfigLoaded() const { return _configLoaded; }

    DiagLog& diagLog() { return DiagLog::instance(); }

    /// Read-only access to the hub's locally-stored board info.  Resolves
    /// `BoardServicePolicy` through `findPolicy<>()`; nullptr until the
    /// lookup machinery is installed by `BoardServer<...>::begin()`.
    /// Used by policies that need to fold the hub's own identify into
    /// a unified system-info response (e.g. `ExpanderServicePolicy`).
    const CoreBoardInfo* hubBoardInfo() const {
        auto* core = findPolicy<BoardServicePolicy>();
        return core ? &core->boardInfo() : nullptr;
    }

    /// Access to the per-board port registry, installed by `BoardOf<>`
    /// before `begin()` walks the policy pack.  Lets non-templated
    /// policies (e.g. `TopologyServicePolicy`) enumerate local ports
    /// without the templated subclass at hand.  nullptr on bare
    /// `BoardServer<>` builds that don't subclass `BoardOf<>`.  Not a
    /// policy, so it stays a direct member rather than going through
    /// `findPolicy<>()`.
    PortRegistryBase* portRegistry() const { return _portRegistry; }

    /// Flip the board-wide "transfer in progress" flag — currently a
    /// signal to `BoardServicePolicy` that STATUS_UPDATE broadcasts
    /// should be suppressed so the serial channel is exclusive to the
    /// file transfer (Rule 28).  Storage policies call this from their
    /// transfer-start / transfer-end hooks.
    void setTransferActive(bool active) {
        if (auto* core = findPolicy<BoardServicePolicy>()) {
            core->setTransferActive(active);
        }
    }

    /// Type-erased policy lookup.  Returns a pointer to the policy of
    /// type `P` if it exists in the templated `BoardServer<...>` pack,
    /// nullptr otherwise.  Same instance the user would get from
    /// `board.policy<P>()` — so a policy can resolve its dependencies
    /// from inside its own `begin()` without the user manually binding
    /// them before `board.begin()`.
    ///
    /// RTTI is disabled in the firmware build (`-fno-rtti`), so type
    /// identity uses the address of a per-type static char as a tag —
    /// each `policyTypeTag<T>()` returns a unique pointer that's
    /// stable across the whole binary.  The dispatcher (installed by
    /// `BoardServer<...>::begin()`) walks the policy tuple and
    /// compares these tags via `std::apply`.  O(N) walk runs only at
    /// `begin()` time; the runtime hot path never touches it.
    template <typename T>
    static const void* policyTypeTag() {
        static const char unique = 0;
        return &unique;
    }

    template <typename P>
    P* findPolicy() const {
        if (!_policyLookupFn || !_policyOwner) return nullptr;
        return static_cast<P*>(_policyLookupFn(_policyOwner, policyTypeTag<P>()));
    }

    // ── I²C scan registration (forwarded to BoardServicePolicy) ──────

    void addExpectedI2CDevice(uint8_t address, I2CDevice* device = nullptr);

    // ── Wire helpers (concrete, no virtual dispatch) ─────────────────

    int sendAck() {
        return sendRawPacket(CorePacket::ACK, _currentTag, nullptr, 0);
    }

    int sendNack(uint8_t errorCode, const char* reason = nullptr);

    int sendRawPacket(uint8_t type, uint8_t tag,
                      const uint8_t* payload, size_t len);

    uint8_t currentTag() const { return _currentTag; }
    Stream* serial()     const { return _serial; }

protected:
    /// Pump available bytes through the COBS framer.  Each complete
    /// frame is decoded, CRC-checked, and routed to dispatchPacket().
    /// Returns number of frames processed.
    int readFrames();

    /// Implemented by the template subclass — walk the policy tuple,
    /// first owner handles, NACK INVALID_COMMAND if nothing matched.
    virtual void dispatchPacket(uint8_t type, uint8_t tag,
                                const uint8_t* payload, size_t len) = 0;

    /// Implemented by the template subclass — ask every policy if it
    /// knows this error code; returns nullptr if no policy claimed it.
    virtual const char* aggregateErrorMessage(uint8_t code) = 0;

    void buildDeviceName(const char* prefix);
    I2CScanResult performI2CScan();

    // Connection-state plumbing called from the template subclass.
    void   resetActivity()             { _lastActivityMs = 0; }
    void   noteActivity()              { _lastActivityMs = millis(); }
    unsigned long lastActivityMs() const { return _lastActivityMs; }

protected:
    // ── Stream / framing / tag ───────────────────────────────────────
    Stream* _serial      = nullptr;
    bool    _initialized = false;
    uint8_t _currentTag  = 0;

    uint8_t       _rxBuffer[RX_BUFFER_SIZE] = {};
    size_t        _rxIndex        = 0;
    unsigned long _lastActivityMs = 0;

    // ── Device identity ──────────────────────────────────────────────
    char _deviceName[24] = {};

    // ── Lifecycle hooks (forwarded by the template subclass) ─────────
    std::function<void(uint8_t mode, uint8_t flags)> _initCb;
    std::function<void()>                            _shutdownCb;

    bool _timeoutEnabled        = true;
    bool _timeoutEnabledByUser  = true;
    bool _configLoaded          = false;

    // Type-erased policy lookup installed at begin() time — see
    // findPolicy<P>() above.  Dispatcher casts `owner` back to
    // `BoardServer<...>` and walks the policy tuple comparing each
    // element's per-type tag against `requestedTag`.  No heap state.
    // Wired *before* the policy-begin() loop so each policy's begin()
    // can resolve its siblings via `ctx->findPolicy<Sibling>()`.
    using PolicyLookupFn = void* (*)(void* owner, const void* requestedTag);
    PolicyLookupFn _policyLookupFn = nullptr;
    void*          _policyOwner    = nullptr;

    /// Per-board `PortRegistry`, installed by `BoardOf<>::begin()`
    /// (the registry isn't a policy, so it can't ride `findPolicy<>()`).
    PortRegistryBase* _portRegistry = nullptr;

    // ── I²C scan registry ───────────────────────────────────────────
    struct ExpectedI2CDevice {
        uint8_t    address = 0;
        I2CDevice* device  = nullptr;
    };
    TwoWire*           _i2cWire = nullptr;
    ExpectedI2CDevice  _expectedI2C[MAX_EXPECTED_I2C] = {};
    uint8_t            _numExpectedI2C = 0;
};

// ============================================================================
// SystemServicePolicy concept
// ============================================================================

/**
 * @brief Compile-time contract every system-service policy satisfies.
 *
 * Policies are plain classes (no inheritance required) providing:
 *
 *   static constexpr uint32_t kCapabilityBits;
 *       OR'd into the board's IDENTIFY capabilities word.
 *
 *   bool begin(BoardServerBase* ctx);
 *       Called once before the first packet — caches `ctx`, initialises
 *       state.  Return false on init failure.
 *
 *   bool ownsType(uint8_t type) const;
 *       True iff this policy handles the given packet type byte.
 *
 *   CommandHandleResult handle(uint8_t type,
 *                              const uint8_t* payload, size_t len);
 *       Process a packet this policy owns.  Use the cached ctx to
 *       send ACK / NACK / responses.
 *
 *   void update();
 *       Tick — called from BoardServer::process() once per loop.
 *
 * Optional:
 *   const char* getErrorMessage(uint8_t code) const;
 *       If present, BoardServer asks each policy for module-specific
 *       error strings before falling back to SerialError::getMessage().
 */
template <typename T>
concept SystemServicePolicy = requires(T t, BoardServerBase* ctx,
                                       uint8_t type, const uint8_t* p, size_t len) {
    { T::kCapabilityBits } -> std::convertible_to<uint32_t>;
    { t.begin(ctx) }       -> std::convertible_to<bool>;
    { t.ownsType(type) }   -> std::convertible_to<bool>;
    { t.handle(type, p, len) } -> std::convertible_to<CommandHandleResult>;
    { t.update() }         -> std::same_as<void>;
};

// ============================================================================
// BoardServer<...UserPolicies>
// ============================================================================

/**
 * @brief The board's main runtime object.
 *
 * Composes BoardServicePolicy + IndicatorServicePolicy + the user's
 * application policies into a single policy tuple, with the full
 * lifecycle wiring (Serial setup, device name, indicator pins, capability
 * advertising, INIT/SHUTDOWN/REBOOT callbacks, COBS frame loop, status
 * broadcast tick, keepalive watchdog).
 *
 * The user instantiates one of these per firmware:
 *
 *   using MyBoard = BoardServer<MyAudioPolicy, MyStoragePolicy, ...>;
 *   MyBoard board;
 *   void setup() { board.begin("MyBoard", FIRMWARE_VERSION, BUILD_NUMBER); }
 *   void loop()  { board.process(); }
 */
template <typename... UserPolicies>
class BoardServer : public BoardServerBase {
public:
    /// Full policy pack — BoardServicePolicy + IndicatorServicePolicy
    /// are prepended automatically; user policies follow.
    using Policies = std::tuple<BoardServicePolicy,
                                IndicatorServicePolicy,
                                UserPolicies...>;

    static_assert((SystemServicePolicy<UserPolicies> && ...),
                  "Every UserPolicy must satisfy sfx_core::SystemServicePolicy");

    /// Capability bitmask — compile-time OR of every policy's
    /// kCapabilityBits.  BoardServicePolicy + IndicatorServicePolicy
    /// contribute 0.
    static constexpr uint32_t kCapabilityMask =
        (BoardServicePolicy::kCapabilityBits
       | IndicatorServicePolicy::kCapabilityBits
       | (UserPolicies::kCapabilityBits | ... | 0u));

    static constexpr uint32_t capabilities() { return kCapabilityMask; }
    static constexpr size_t   policyCount()  { return 2 + sizeof...(UserPolicies); }

    BoardServer() = default;

    // ── Accessors ────────────────────────────────────────────────────

    template <typename P>       P& policy()       { return std::get<P>(_policies); }
    template <typename P> const P& policy() const { return std::get<P>(_policies); }

    BoardServicePolicy&        core()       { return std::get<BoardServicePolicy>(_policies); }
    const BoardServicePolicy&  core() const { return std::get<BoardServicePolicy>(_policies); }

    IndicatorServicePolicy&        indicators()       { return std::get<IndicatorServicePolicy>(_policies); }
    const IndicatorServicePolicy&  indicators() const { return std::get<IndicatorServicePolicy>(_policies); }

    // ── Lifecycle callbacks (delegate to BoardServicePolicy) ─────────

    void onInit    (std::function<void(uint8_t mode, uint8_t flags)> cb) { _initCb     = std::move(cb); }
    void onShutdown(std::function<void()>                            cb) { _shutdownCb = std::move(cb); }

    /// Mark that valid config has been loaded from flash so that
    /// disconnect-from-master transitions to STANDALONE rather than IDLE.
    void markConfigLoaded() {
        _configLoaded = true;
        if (core().boardState() == BoardState::IDLE) {
            core().setBoardState(BoardState::STANDALONE);
        }
    }

    // ── begin / process — the entire public lifecycle ────────────────

    /**
     * @brief Wire up Serial, indicator pins, lifecycle callbacks, and
     *        every policy.  Call once from setup().
     *
     * @param prefix       Device-name prefix (e.g., "HubFx", "LightFx").
     *                     Suffix is the last 4 chars of the silicon ID.
     * @param version      FIRMWARE_VERSION string (without leading 'v').
     * @param buildNumber  BUILD_NUMBER define (auto-incremented on flash).
     * @param connectionPin GPIO for the connection-status indicator LED.
     * @param errorPin     GPIO for the error/warning indicator LED
     *                     (pass -1 to disable on boards without one).
     */
    void begin(const char* prefix, const char* version, uint32_t buildNumber,
               int connectionPin = 13, int errorPin = 14) {
        // Serial setup
#ifdef ESP32
        Serial.setRxBufferSize(131072);
#endif
        Serial.begin(BAUD_RATE);
        while (!Serial && millis() < 3000) SFX_DELAY_MS(10);

        _serial      = &Serial;
        _initialized = true;

        buildDeviceName(prefix);
        DiagLog::instance().begin(&Serial);

        // Install the type-erased policy lookup BEFORE walking the pack
        // so each policy's `begin(ctx)` can resolve siblings through
        // `ctx->findPolicy<Sibling>()` without depending on user-side
        // setter wiring.
        this->_policyOwner    = this;
        this->_policyLookupFn = &BoardServer::policyLookupDispatcher;

        // Init every policy.  Each receives `this` as its BoardServerBase.
        std::apply([this](auto&... p) {
            (p.begin(static_cast<BoardServerBase*>(this)), ...);
        }, _policies);

        // Configure indicator-LED pins.
        indicators().configure(connectionPin, errorPin);

        // Seed board info + advertised capabilities into BoardServicePolicy.
        auto& c = core();
        c.setBoardInfo(_deviceName, version, SFX_PLATFORM_NAME,
                       SFX_CPU_MHZ(), SFX_FREE_HEAP(), buildNumber);
        c.setCapabilities(kCapabilityMask);

        // Wire BoardServicePolicy lifecycle hooks → our overrides.
        c.onInit    ([this](uint8_t mode, uint8_t flags) { doInit(mode, flags); });
        c.onShutdown([this]()                            { doShutdown();       });
        c.onReboot  ([this]() {
            doShutdown();
            SFX_DELAY_MS(100);
            SFX_REBOOT();
        });
#if SFX_PLATFORM_PICO
        c.onBootsel([this]() {
            doShutdown();
            SFX_DELAY_MS(500);
            sfxRebootToBootloader();
        });
#endif
    }

    /// Bind a TwoWire bus and register the I²C-scan callback on
    /// BoardServicePolicy.  Call after begin().  Tracked devices are
    /// added via `addExpectedI2CDevice(addr, device?)`.
    void enableI2CScan(TwoWire& wire) {
        _i2cWire = &wire;
        core().onI2CScan([this]() -> I2CScanResult {
            return this->performI2CScan();
        });
    }

    /// Pump the bus.  Call once per loop():
    ///   1. read incoming COBS frames (and dispatch them);
    ///   2. tick every policy;
    ///   3. update activity/freeRam on BoardServicePolicy;
    ///   4. enforce the connection timeout watchdog.
    void process() {
        int frames = readFrames();
        auto& c = core();
        if (frames > 0 || lastActivityMs() > c.lastActivityMs()) {
            c.updateActivity();
        }
        c.updateFreeRam(SFX_FREE_HEAP());

        std::apply([](auto&... p) { (p.update(), ...); }, _policies);

        checkConnectionTimeout();
    }

protected:
    // ── BoardServerBase virtual hooks ────────────────────────────────

    void dispatchPacket(uint8_t type, uint8_t tag,
                        const uint8_t* payload, size_t len) override {
        if (!_initialized || !_serial) return;
        _currentTag = tag;

        CommandHandleResult result = CommandHandleResult::NotMyCommand;
        std::apply([&](auto&... pol) {
            (void) ((result == CommandHandleResult::NotMyCommand
                     && pol.ownsType(type)
                     && (result = pol.handle(type, payload, len),
                         true /* short-circuit OK */)) || ...);
        }, _policies);

        if (result != CommandHandleResult::Handled) {
            sendNack(SerialError::INVALID_COMMAND);
        }
    }

    const char* aggregateErrorMessage(uint8_t code) override {
        const char* msg = nullptr;
        std::apply([&](auto&... pol) {
            (void) ((msg == nullptr
                     && (msg = errorMessageOrNull(pol, code))) || ...);
        }, _policies);
        return msg;
    }

    /// Type-erased policy lookup — installed into `BoardServerBase`
    /// at begin() time so that any policy holding only a
    /// `BoardServerBase*` can resolve siblings by type:
    ///
    ///     auto* port = ctx->findPolicy<PortServicePolicy>();
    ///
    /// Walks `_policies` via `std::apply` and returns the first element
    /// whose `policyTypeTag<>()` matches `requestedTag`.  Strict type
    /// match — no inheritance traversal.
    static void* policyLookupDispatcher(void* owner, const void* requestedTag) {
        auto* self = static_cast<BoardServer*>(owner);
        void* found = nullptr;
        std::apply([&](auto&... policy) {
            (void)(((found == nullptr &&
                     BoardServerBase::policyTypeTag<std::decay_t<decltype(policy)>>()
                         == requestedTag)
                    ? (found = static_cast<void*>(&policy), true)
                    : false) || ...);
        }, self->_policies);
        return found;
    }

private:
    template <typename P>
    static const char* errorMessageOrNull(P& p, uint8_t code) {
        if constexpr (requires { { p.getErrorMessage(code) } -> std::convertible_to<const char*>; }) {
            return p.getErrorMessage(code);
        } else {
            return nullptr;
        }
    }

    // ── Internal lifecycle / watchdog ────────────────────────────────

    void doInit(uint8_t mode, uint8_t flags) {
        auto& c   = core();
        auto& ind = indicators();

        if (mode == InitMode::SLAVE) {
            _timeoutEnabled = _timeoutEnabledByUser;
            c.setBoardState(BoardState::SLAVE);
        } else if (mode == InitMode::DIRECT) {
            _timeoutEnabled = false;
            c.setBoardState(BoardState::DIRECT);
        }

        SFX_LOG_INFO("INIT: mode=%s flags=0x%02X verbose=%s state=%s",
                     InitMode::getName(mode), flags,
                     (flags & InitFlags::VERBOSE) ? "on" : "off",
                     BoardState::getName(c.boardState()));

        if (_initCb) _initCb(mode, flags);
        ind.setConnected(true);
        ind.setWatchdogTriggered(false);
    }

    void doShutdown() {
        if (_shutdownCb) _shutdownCb();
        indicators().setConnected(false);
        _timeoutEnabled = _timeoutEnabledByUser;
        core().setBoardState(_configLoaded ? BoardState::STANDALONE : BoardState::IDLE);
    }

    void checkConnectionTimeout() {
        if (!_timeoutEnabled) return;
        auto& c   = core();
        auto& ind = indicators();
        if (c.checkTimeout(CONNECTION_TIMEOUT_ms)) {
            if (!ind.isWatchdogTriggered()) {
                SFX_LOG_WARN("Connection timeout (%lums inactivity)", CONNECTION_TIMEOUT_ms);
                doShutdown();
                ind.setWatchdogTriggered(true);
            }
        }
    }

    Policies _policies;
};

}  // namespace sfx_core

#endif  // SFX_BOARD_SERVER_H
