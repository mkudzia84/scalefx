/*
 * SystemServicePolicy + BoardServer — composable policy framework.
 *
 * A `BoardServer<...Policies>` composes typed service policies at
 * compile time:
 *
 *   using HubFxBoard = BoardServer<
 *       BoardServicePolicy,
 *       IndicatorServicePolicy,
 *       AudioServicePolicy<HubFxMixer>,
 *       ConfigServicePolicy<HubSettings, EngineCfg, LightCfg>,
 *       StorageServicePolicy<Esp32UploadPolicy, FlashBackend, SdLfsBackend>,
 *       BatteryServicePolicy<Ina226Battery>,
 *       UsbHostServicePolicy,
 *       EngineServicePolicy>;
 *
 *   HubFxBoard board;
 *   board.begin(&Serial);     // forwards to every policy's begin()
 *
 *   PacketReader<HubFxBoard> reader;
 *   reader.begin(&Serial, &board);
 *   reader.process();         // call once per loop()
 *
 * Each policy:
 *   - declares the capability bits it contributes to IDENTIFY
 *   - declares which wire-packet types it owns (`ownsType(uint8_t)`)
 *   - handles those packets via `handle(type, payload, len)`
 *   - is ticked via `update()` from `BoardServer::update()`
 *
 * `ServiceContext` lets every policy emit ACK / NACK / raw responses
 * without a back-reference to the variadic `BoardServer<...>` type.
 * Policies receive a `ServiceContext*` at `begin()` time.
 *
 * Wave-5 invariant: `BoardServer` is the single packet dispatcher.
 * `BusServer`, `CoreCommandServer`, `ICommandHandler`, `CommandRouter`
 * are all gone — routing is fully compile-time-resolved through the
 * policy tuple, and the wire helpers (`sendAck/Nack/RawPacket`) live
 * directly on `BoardServer`.
 */

#ifndef SFX_SYSTEM_SERVICE_H
#define SFX_SYSTEM_SERVICE_H

#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include <concepts>
#include <tuple>
#include <type_traits>

#include "core.h"                  // CommandHandleResult, SerialError, CorePacket
#include "platform/sfx_wire.h"     // SfxWire::encodePacket / TAG_ASYNC / COBS_BUFFER_SIZE

namespace sfx_core {

// ============================================================================
// ServiceContext — wire helpers BoardServer hands to each policy
// ============================================================================

/**
 * @brief Non-owning interface giving a policy access to BoardServer's
 *        wire helpers.  The policy receives a pointer at begin() time
 *        and calls these methods from inside handle().
 */
class ServiceContext {
public:
    virtual ~ServiceContext() = default;

    /// Send ACK with the current request's correlation tag.
    virtual int sendAck() = 0;

    /// Send NACK with error code.  If `reason` is null, BoardServer
    /// looks up the message via its owned policies' getErrorMessage().
    virtual int sendNack(uint8_t errorCode, const char* reason = nullptr) = 0;

    /// Send a raw packet — caller supplies type, tag, payload.
    virtual int sendRawPacket(uint8_t type, uint8_t tag,
                              const uint8_t* payload, size_t len) = 0;

    /// The correlation tag from the request currently being handled.
    virtual uint8_t currentTag() const = 0;

    /// The Stream this server speaks on (Serial / Serial1 / etc.).
    virtual Stream* serial() const = 0;
};

// ============================================================================
// SystemServicePolicy concept
// ============================================================================

/**
 * @brief Compile-time contract every system-service policy satisfies.
 *
 * Each policy is a plain class (no inheritance required) providing:
 *
 *   static constexpr uint32_t kCapabilityBits;
 *       OR'd into the board's capabilities word for IDENTIFY.
 *
 *   bool begin(sfx_core::ServiceContext* ctx);
 *       Called once before the first packet — caches `ctx`, initialises
 *       state.  Return false on init failure (board sets up but service
 *       advertises false in IDENTIFY).
 *
 *   bool ownsType(uint8_t type) const;
 *       True iff this policy handles the given packet type byte.
 *       BoardServer walks the policy pack in declaration order.
 *
 *   CommandHandleResult handle(uint8_t type,
 *                              const uint8_t* payload, size_t len);
 *       Process a packet this policy owns.  Use the cached
 *       ServiceContext to send ACK/NACK/responses.
 *
 *   void update();
 *       Tick — called from BoardServer::update() once per loop.
 */
template <typename T>
concept SystemServicePolicy = requires(T t, sfx_core::ServiceContext* ctx,
                                       uint8_t type, const uint8_t* p, size_t len) {
    { T::kCapabilityBits } -> std::convertible_to<uint32_t>;
    { t.begin(ctx) }       -> std::convertible_to<bool>;
    { t.ownsType(type) }   -> std::convertible_to<bool>;
    { t.handle(type, p, len) } -> std::convertible_to<CommandHandleResult>;
    { t.update() }         -> std::same_as<void>;
};

// ============================================================================
// BoardServer<...Policies> — variadic composer + wire dispatcher
// ============================================================================

/**
 * @brief Hosts a tuple of system-service policies and dispatches wire
 *        packets to them.  Self-contained: owns the Stream, the
 *        correlation tag, and the COBS-encode helpers.  Dispatched by
 *        `PacketReader<BoardServer<...>>`.
 *
 * Dispatch (called from PacketReader):
 *   dispatch(type, tag, payload, len)
 *     → records `tag` as the current correlation tag
 *     → walks the policy tuple in declaration order
 *     → first policy whose ownsType(type) returns true gets handle()'d
 *     → NACKs INVALID_COMMAND if no policy owned the type
 *
 * Lifecycle:
 *   begin(Stream*)  → caches stream, calls begin(this) on every policy
 *   update()        → calls update() on every policy
 *
 * Capability bits:
 *   `capabilities()` returns the compile-time OR of every policy's
 *   `kCapabilityBits` — surfaced in IDENTIFY via BoardServicePolicy.
 */
template <typename... Policies>
class BoardServer : public ServiceContext {
    static_assert((SystemServicePolicy<Policies> && ...),
                  "Every parameter of BoardServer must satisfy "
                  "sfx_core::SystemServicePolicy");

public:
    /// Compile-time OR of every policy's capability bits.
    static constexpr uint32_t kCapabilityMask = (Policies::kCapabilityBits | ... | 0u);

    /// Number of policies in the pack.
    static constexpr size_t policyCount() { return sizeof...(Policies); }

    /// Access a specific policy by type — for cross-policy interaction
    /// (e.g., StorageServicePolicy mute-on-upload talking to AudioServicePolicy).
    template <typename P>
    P& policy() { return std::get<P>(_policies); }

    template <typename P>
    const P& policy() const { return std::get<P>(_policies); }

    BoardServer() = default;
    BoardServer(const BoardServer&) = delete;
    BoardServer& operator=(const BoardServer&) = delete;

    // ─────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────

    /// Open the serial stream, then call begin() on every policy.
    /// Each policy receives `this` as its ServiceContext.
    bool begin(Stream* serial) {
        if (!serial) return false;
        _serial      = serial;
        _initialized = true;
        bool ok = true;
        std::apply([&](auto&... p) {
            ((ok = ok && p.begin(static_cast<ServiceContext*>(this))), ...);
        }, _policies);
        return ok;
    }

    /// Tick — calls update() on every policy.  Drive from loop().
    void update() {
        std::apply([](auto&... p) { (p.update(), ...); }, _policies);
    }

    /// Compile-time capability bitmask — feed into BoardServicePolicy
    /// via `setCapabilities(Board::capabilities())` during setup().
    static constexpr uint32_t capabilities() { return kCapabilityMask; }

    bool       isInitialized() const { return _initialized; }
    Stream*    stream()        const { return _serial; }

    // ─────────────────────────────────────────────────────────────────
    // PacketReader dispatch entry point
    // ─────────────────────────────────────────────────────────────────

    /// Called by PacketReader for every complete, CRC-verified frame.
    /// Records `tag`, walks the policy tuple, falls back to NACK.
    void dispatch(uint8_t type, uint8_t tag,
                  const uint8_t* payload, size_t len) {
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

    // ─────────────────────────────────────────────────────────────────
    // ServiceContext — wire helpers
    // ─────────────────────────────────────────────────────────────────

    int sendAck() override {
        return sendRawPacket(CorePacket::ACK, _currentTag, nullptr, 0);
    }

    int sendNack(uint8_t errorCode, const char* reason = nullptr) override {
        uint8_t payload[64];
        payload[0] = errorCode;

        const char* msg = (reason && reason[0]) ? reason
                                                : aggregateErrorMessage(errorCode);
        size_t msgLen = std::strlen(msg);
        if (msgLen > sizeof(payload) - 1) msgLen = sizeof(payload) - 1;
        std::memcpy(&payload[1], msg, msgLen);

        return sendRawPacket(CorePacket::NACK, _currentTag, payload, 1 + msgLen);
    }

    int sendRawPacket(uint8_t type, uint8_t tag,
                      const uint8_t* payload, size_t len) override {
        if (!_serial) return -1;
        uint8_t buf[SfxWire::COBS_BUFFER_SIZE];
        size_t  encoded = SfxWire::encodePacket(buf, type, tag, payload, len);
        if (encoded == 0) return -1;
        return static_cast<int>(_serial->write(buf, encoded));
    }

    uint8_t currentTag() const override { return _currentTag; }
    Stream* serial()     const override { return _serial; }

private:
    /// Aggregate error-message lookup — walks the policy tuple asking
    /// each policy if it knows this code, falls back to the generic
    /// SerialError table.  C++20 `requires` keeps the SFINAE inline.
    const char* aggregateErrorMessage(uint8_t code) {
        const char* msg = nullptr;
        std::apply([&](auto&... pol) {
            (void) ((msg == nullptr
                     && (msg = errorMessageOrNull(pol, code))) || ...);
        }, _policies);
        return msg ? msg : SerialError::getMessage(code);
    }

    template <typename P>
    static const char* errorMessageOrNull(P& p, uint8_t code) {
        if constexpr (requires { { p.getErrorMessage(code) } -> std::convertible_to<const char*>; }) {
            return p.getErrorMessage(code);
        } else {
            return nullptr;
        }
    }

    std::tuple<Policies...> _policies;
    Stream*                 _serial      = nullptr;
    uint8_t                 _currentTag  = 0;
    bool                    _initialized = false;
};

}  // namespace sfx_core

#endif  // SFX_SYSTEM_SERVICE_H
