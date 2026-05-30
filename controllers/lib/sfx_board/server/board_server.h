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
#include <serial/packet_reader.h> // sfx_serial::PacketReader (byte-stream → frame)
#include <serial/diag_log.h>      // DiagLog (LOG_MESSAGE / DIAG_HISTORY)
#include <platform/sfx_platform.h>  // SFX_PLATFORM_*, SFX_CPU_MHZ, SFX_FREE_HEAP, SFX_REBOOT, sfxGetBoardId
#if SFX_PLATFORM_ESP32
#include <platform/native_uart_stream.h>  // sfx::NativeUartStream — replaces Arduino's Serial under IDF-component
#endif

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
    static constexpr size_t        RX_BUFFER_SIZE         = SfxWire::COBS_BUFFER_SIZE;

    // ── Expected-I²C-device table capacity ───────────────────────────
    //
    // The board sketch calls `addExpectedI2CDevice(addr [, driver])`
    // for every chip it wants to surface in `I2C_SCAN_RESULT` with a
    // found/identified flag (anything that doesn't get registered here
    // still shows up under `extraAddresses` if it ACKs the bus, but
    // without the friendly status).
    //
    // Default is 32 — comfortable headroom for the busiest current
    // firmware (HubFX: TAS5825P + PCA9685 + 8 INA226 = 10 devices, with
    // room for a battery sensor + EEPROM + future expansion).  Override
    // per-firmware via `-DSFX_MAX_EXPECTED_I2C=N` in platformio.ini if
    // a board genuinely needs more.
    //
    // Wire-format note: `I2CScanResult::MAX_EXPECTED` (in serial/core/core.h)
    // must be at least this large — bump them together if increasing
    // beyond 32.
#ifndef SFX_MAX_EXPECTED_I2C
#define SFX_MAX_EXPECTED_I2C 32
#endif
    static constexpr uint8_t MAX_EXPECTED_I2C = SFX_MAX_EXPECTED_I2C;
    static_assert(MAX_EXPECTED_I2C <= I2CScanResult::MAX_EXPECTED,
                  "SFX_MAX_EXPECTED_I2C exceeds the wire-format cap "
                  "(I2CScanResult::MAX_EXPECTED in serial/core/core.h)");

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

    /// Register a chip the board sketch wants surfaced in the scan
    /// result with a found/identified flag.  Returns false (and logs a
    /// WARN) when the expected-devices table is full — bump
    /// `SFX_MAX_EXPECTED_I2C` in platformio.ini if you hit that.
    bool addExpectedI2CDevice(uint8_t address, I2CDevice* device = nullptr);

    // ── Wire helpers (concrete, no virtual dispatch) ─────────────────

    int sendAck() {
        if (_captureNext) {
            _capturedAck  = true;
            _captureNext  = false;
            return 0;
        }
        return sendRawPacket(CorePacket::ACK, _currentTag, nullptr, 0);
    }

    int sendNack(uint8_t errorCode, const char* reason = nullptr);

    // ── Master-internal response capture ─────────────────────────────
    //
    // Effects code that wants to invoke a local policy's handler
    // *without* emitting an ACK/NACK on the wire (because the request
    // didn't come from a wire packet) brackets the call with
    // `beginCapture()` / `endCapture()`.  The next sendAck()/sendNack()
    // is intercepted into local state instead of going to the serial
    // port.  Single-slot — nesting is unsupported.
    void    beginCapture() { _captureNext = true;  _capturedAck = false; _capturedErr = 0; }
    void    endCapture()   { _captureNext = false; _capturedAck = false; _capturedErr = 0; }
    bool    capturedAck()  const { return _capturedAck; }
    uint8_t capturedErr()  const { return _capturedErr; }

    /// Wire the encoded packet out — virtual so the templated
    /// `BoardServerBaseT<TStream>` can override with a direct-stream
    /// implementation that bypasses Stream's vtable.  Base
    /// implementation in board_server.cpp uses the legacy `_serial`
    /// Stream* for non-template callers.
    virtual int sendRawPacket(uint8_t type, uint8_t tag,
                              const uint8_t* payload, size_t len);

    uint8_t currentTag() const { return _currentTag; }
    Stream* serial()     const { return _serial; }

    // Verbose async streams (input-frame broadcasts, gun verbose status, …)
    // only transmit while a host is actively listening — i.e. we've heard
    // from it (ANY packet, including the KEEPALIVE heartbeat the client sends
    // every few seconds) within this window.  When the host disconnects the
    // keepalives stop, so the board quiesces instead of streaming 10 Hz into a
    // dead port — that backlog is what otherwise buries the next IDENTIFY and
    // makes reconnects slow.  Window > the client keepalive interval (~3 s)
    // with margin, and < the 15 s connection-reset timeout.  Public so service
    // policies can gate their broadcast emits via `_ctx`.
    static constexpr unsigned long kVerboseIdleMs = 8000;
    bool hostVerboseActive() const {
        return _lastActivityMs != 0 && (millis() - _lastActivityMs) < kVerboseIdleMs;
    }

protected:
    /// Pump available bytes through the COBS framer.  Each complete
    /// frame is decoded, CRC-checked, and routed to dispatchPacket().
    /// Returns number of frames processed.  Virtual for the same
    /// reason as sendRawPacket() above.
    virtual int readFrames();

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

    // COBS frame accumulator — owned by the base so both this class's
    // readFrames() and the templated subclass's readFrames() override
    // share one buffer.  See packet_reader.h for the state machine
    // (pulled out 2026-05-29 so the same logic could be unit-tested
    // on the host via tests/native/test_packet_reader.cpp).
    sfx_serial::PacketReader _reader;
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

    // ── Response-capture state (see beginCapture above) ──────────────
    bool    _captureNext  = false;
    bool    _capturedAck  = false;
    uint8_t _capturedErr  = 0;

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
// BoardServerBaseT<TStream> — templated mid-tier base (Phase 4 of
// feature/idf-component-build, 2026-05-28).
// ============================================================================
//
// Inherits the type-erased `BoardServerBase` (so policies can keep their
// `BoardServerBase*` ctx — no cascade through the policy framework) but
// holds the concrete `TStream&` and OVERRIDES the wire-rate hot path
// (`readFrames`, `sendRawPacket`) to call directly on the stream — no
// vtable per byte.  Compiler devirtualizes these calls within
// `BoardServer<TStream, ...UserPolicies>::process()` because the
// concrete derived type is statically known at that call site.
//
// `TStream` is required to expose:
//   - bool/int available()
//   - int read()
//   - size_t write(const uint8_t*, size_t)
// i.e. anything that satisfies Arduino's `Stream` interface.  In
// practice: `sfx::NativeUartStream` (ESP32 IDF-component path), Arduino's
// `HardwareSerial`/`USBCDC` (Pico + ESP32 regular-Arduino path), or a
// test stub that captures bytes.
//
template <typename TStream>
class BoardServerBaseT : public BoardServerBase {
public:
    /// Re-exported so consumers parameterised on a `BoardServerBaseT<...>`
    /// (or a `BoardServer<...>` further down) can recover the stream
    /// type via `TBoard::StreamType` — Rule 33 carrier-typedef pattern.
    using StreamType = TStream;

    /// Set by `BoardServer<TStream, ...>::begin(TStream&, …)`.  Pointer
    /// (not reference) so the default-constructed BoardServerBase
    /// chain stays valid before begin() runs.
    void setStream(TStream& stream) {
        _stream  = &stream;
        // Mirror into BoardServerBase::_serial so legacy Stream*
        // consumers (DiagLog, StorageService::serial(), …) keep working
        // without having to spell out TStream.  Requires TStream to
        // inherit Arduino's Stream — every current platform's stream
        // type already does.
        _serial  = &stream;
    }

    TStream* nativeStream() const { return _stream; }

    // ── Wire hot-path overrides ──────────────────────────────────────
    //
    // BoardServerBase's defaults use `_serial` via the Stream vtable
    // (one indirect call per byte).  These overrides go straight through
    // `_stream` — at 6 Mbps the per-byte cost difference is significant.

    int sendRawPacket(uint8_t type, uint8_t tag,
                      const uint8_t* payload, size_t len) override {
        if (!_stream) return -1;
        uint8_t buf[SfxWire::COBS_BUFFER_SIZE];
        const size_t encoded = SfxWire::encodePacket(buf, type, tag, payload, len);
        if (encoded == 0) return -1;
        return static_cast<int>(_stream->write(buf, encoded));
    }

    int readFrames() override {
        if (!_stream) return 0;
        int frames = 0;
        while (_stream->available()) {
            const uint8_t b = static_cast<uint8_t>(_stream->read());
            _lastActivityMs = millis();
            // See packet_reader.h.  The lambda runs once per complete
            // frame and does the cobs-decode + parsePacket + dispatch
            // dance the inline loop used to do.  PacketReader handles
            // FRAME_DELIMITER detection, partial-frame buffering, and
            // overflow recovery.
            _reader.feedByte(b, [this, &frames](const uint8_t* frame, size_t frameLen) {
                uint8_t        decoded[SfxWire::MAX_PACKET_SIZE];
                const size_t   decodedLen = SfxWire::cobsDecode(
                    frame, frameLen, decoded, sizeof(decoded));
                if (decodedLen < 5) return;

                uint8_t        type, tag;
                const uint8_t* payload;
                size_t         payloadLen;
                if (SfxWire::parsePacket(decoded, decodedLen,
                                         &type, &tag, &payload, &payloadLen)) {
                    dispatchPacket(type, tag, payload, payloadLen);
                    ++frames;
                }
            });
        }
        return frames;
    }

protected:
    TStream* _stream = nullptr;
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
template <typename TStream, typename... UserPolicies>
class BoardServer : public BoardServerBaseT<TStream> {
public:
    /// Stream type carrier-typedef — Rule 33.  Lets helpers parameterised
    /// on `TBoard` alone (BoardOf<TBoard, …>, future Stream-aware
    /// helpers) recover the underlying type via `TBoard::StreamType`
    /// without an extra template argument.
    using StreamType = TStream;

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

    // ── Runtime-enabled capability walker ─────────────────────────────
    //
    // A policy optionally exposes `bool enabled() const`.  When present,
    // its `kCapabilityBits` only contribute to the runtime-enabled mask
    // when `enabled()` returns true.  Policies without the accessor are
    // treated as "always enabled" (e.g. PortService, RoleService — they
    // can't be disabled at runtime).  The HubFxConfigServicePolicy
    // re-runs `recomputeEnabledCapabilities()` after every
    // `applyConfig()` so the master's advertised enabled set tracks the
    // YAML in real time.

    template <typename T>
    static constexpr bool kHasEnabledAccessor =
        requires(const T& t) { { t.enabled() } -> std::convertible_to<bool>; };

    template <typename T>
    static uint32_t policyEnabledBits(const T& p) {
        if constexpr (kHasEnabledAccessor<T>) {
            return p.enabled() ? T::kCapabilityBits : 0u;
        } else {
            return T::kCapabilityBits;
        }
    }

    uint32_t computeEnabledCapabilities() const {
        uint32_t mask = 0;
        std::apply([&](const auto&... p) {
            ((mask |= policyEnabledBits(p)), ...);
        }, _policies);
        return mask;
    }

    /// Walk every policy, recompute the enabled-capability mask, write
    /// it back into `BoardServicePolicy._boardInfo.enabledCapabilities`.
    /// Cheap; safe to call after any config apply.
    void recomputeEnabledCapabilities() {
        core().setEnabledCapabilities(computeEnabledCapabilities());
    }

    BoardServer() = default;

    // ── Accessors ────────────────────────────────────────────────────

    template <typename P>       P& policy()       { return std::get<P>(_policies); }
    template <typename P> const P& policy() const { return std::get<P>(_policies); }

    BoardServicePolicy&        core()       { return std::get<BoardServicePolicy>(_policies); }
    const BoardServicePolicy&  core() const { return std::get<BoardServicePolicy>(_policies); }

    IndicatorServicePolicy&        indicators()       { return std::get<IndicatorServicePolicy>(_policies); }
    const IndicatorServicePolicy&  indicators() const { return std::get<IndicatorServicePolicy>(_policies); }

    // ── Lifecycle callbacks (delegate to BoardServicePolicy) ─────────

    void onInit    (std::function<void(uint8_t mode, uint8_t flags)> cb) { this->_initCb     = std::move(cb); }
    void onShutdown(std::function<void()>                            cb) { this->_shutdownCb = std::move(cb); }

    /// Mark that valid config has been loaded from flash so that
    /// disconnect-from-master transitions to STANDALONE rather than IDLE.
    void markConfigLoaded() {
        this->_configLoaded = true;
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
    /// Caller-supplied stream binding (Phase 4 of feature/idf-component-build,
    /// 2026-05-28).  The TStream instance lives in the sketch (typically
    /// a global) and is brought up there with the right pins/baud BEFORE
    /// `board.begin(stream, …)` is called.  Sketch pattern:
    ///
    ///   static sfx::NativeUartStream wireUart;
    ///   void setup() {
    ///       wireUart.begin(UART_NUM_0, 44, 43, 6000000, 16384, 16384);
    ///       board.begin(wireUart, "HubFx", FIRMWARE_VERSION, BUILD_NUMBER);
    ///   }
    void begin(TStream& stream,
               const char* prefix, const char* version, uint32_t buildNumber,
               int connectionPin = 13, int errorPin = 14) {
        // Wire the stream into both the templated hot-path slot AND the
        // legacy Stream* slot in BoardServerBase (DiagLog, StorageService::
        // serial(), … keep using Stream* until they too templatize).
        this->setStream(stream);

        // Block briefly until the stream reports ready (mainly relevant
        // when TStream is Arduino's Serial / USBCDC where !stream means
        // "USB host hasn't enumerated yet"; NativeUartStream is ready
        // immediately after install).
        while (!stream && millis() < 3000) SFX_DELAY_MS(10);

        this->_initialized = true;

        this->buildDeviceName(prefix);
        DiagLog::instance().begin(this->_serial);

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
        c.setBoardInfo(this->_deviceName, version, SFX_PLATFORM_NAME,
                       SFX_CPU_MHZ(), SFX_FREE_HEAP(), buildNumber);
        c.setCapabilities(kCapabilityMask);
        // Initial runtime-enabled mask — walks every policy's `enabled()`
        // accessor.  Re-run by `HubFxConfigServicePolicy::applyConfig()`
        // after each config flip.
        recomputeEnabledCapabilities();

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
        this->_i2cWire = &wire;
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
        int frames = this->readFrames();
        auto& c = core();
        if (frames > 0 || this->lastActivityMs() > c.lastActivityMs()) {
            c.updateActivity();
        }
        c.updateFreeRam(SFX_FREE_HEAP());

        std::apply([](auto&... p) { (p.update(), ...); }, _policies);

        checkConnectionTimeout();
    }

    /// Pump ONLY the bus — read + dispatch incoming COBS frames, update
    /// activity — WITHOUT ticking the policies.  Used by the upload-exclusive
    /// loop branch so a sync upload drains "only the storage server" (as the
    /// loop intends): roles/effects don't tick, so no input/verbose broadcast
    /// is emitted while a transfer owns the bus.  Stream uploads already bypass
    /// this via processStream(); this closes the sync-upload path.
    int pumpBus() {
        int frames = this->readFrames();
        auto& c = core();
        if (frames > 0 || this->lastActivityMs() > c.lastActivityMs()) {
            c.updateActivity();
        }
        return frames;
    }

protected:
    // ── BoardServerBase virtual hooks ────────────────────────────────

    void dispatchPacket(uint8_t type, uint8_t tag,
                        const uint8_t* payload, size_t len) override {
        if (!this->_initialized || !this->_serial) return;
        this->_currentTag = tag;

        CommandHandleResult result = CommandHandleResult::NotMyCommand;
        std::apply([&](auto&... pol) {
            (void) ((result == CommandHandleResult::NotMyCommand
                     && pol.ownsType(type)
                     && (result = pol.handle(type, payload, len),
                         true /* short-circuit OK */)) || ...);
        }, _policies);

        if (result != CommandHandleResult::Handled) {
            this->sendNack(SerialError::INVALID_COMMAND);
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
            this->_timeoutEnabled = this->_timeoutEnabledByUser;
            c.setBoardState(BoardState::SLAVE);
        } else if (mode == InitMode::DIRECT) {
            this->_timeoutEnabled = false;
            c.setBoardState(BoardState::DIRECT);
        }

        SFX_LOG_INFO("INIT: mode=%s flags=0x%02X verbose=%s state=%s",
                     InitMode::getName(mode), flags,
                     (flags & InitFlags::VERBOSE) ? "on" : "off",
                     BoardState::getName(c.boardState()));

        if (this->_initCb) this->_initCb(mode, flags);
        ind.setConnected(true);
        ind.setWatchdogTriggered(false);
    }

    void doShutdown() {
        if (this->_shutdownCb) this->_shutdownCb();
        indicators().setConnected(false);
        this->_timeoutEnabled = this->_timeoutEnabledByUser;
        core().setBoardState(this->_configLoaded ? BoardState::STANDALONE : BoardState::IDLE);
    }

    void checkConnectionTimeout() {
        if (!this->_timeoutEnabled) return;
        auto& c   = core();
        auto& ind = indicators();
        if (c.checkTimeout(BoardServerBase::CONNECTION_TIMEOUT_ms)) {
            if (!ind.isWatchdogTriggered()) {
                SFX_LOG_WARN("Connection timeout (%lums inactivity)", BoardServerBase::CONNECTION_TIMEOUT_ms);
                doShutdown();
                ind.setWatchdogTriggered(true);
            }
        }
    }

    Policies _policies;
};

}  // namespace sfx_core

#endif  // SFX_BOARD_SERVER_H
