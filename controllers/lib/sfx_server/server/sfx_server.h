/**
 * SfxServer<...UserPolicies> — templated ScaleFX server boilerplate.
 *
 * Owns a `BoardServer<BoardServicePolicy, IndicatorServicePolicy, UserPolicies...>`
 * + a `CommandRouter` configured with that BoardServer as its sole
 * registered handler.  Setup boilerplate is identical to the legacy
 * (non-templated) shape; the difference is that every protocol-exposed
 * subsystem now sits in the policy pack instead of being a separate
 * `addModuleHandler()` call.
 *
 * Usage (HubFX):
 *
 *   using HubFxServer = SfxServer<
 *       UsbHostServicePolicy,
 *       AudioServicePolicy<Mixer>,
 *       EngineServicePolicy,
 *       ConfigServicePolicy,
 *       BatteryServicePolicy<Ina226Battery>,
 *       StorageServer
 *   >;
 *   HubFxServer server;
 *
 *   void setup() {
 *       server.begin("HubFx", FIRMWARE_VERSION, BUILD_NUMBER);
 *       server.onInit(    [](uint8_t m, uint8_t f) { ... });
 *       server.onShutdown([](){ ... });
 *       // Bind externally-dependent policies via board().policy<P>():
 *       server.board().policy<BatteryServicePolicy<Ina226Battery>>().bindBattery(...);
 *       // ...
 *   }
 *
 *   void loop() {
 *       server.loop();
 *       // ... board-specific work ...
 *   }
 *
 * Accessors:
 *   server.core()        → BoardServicePolicy& (was CoreCommandServer&)
 *   server.indicators()  → IndicatorServicePolicy&
 *   server.board()       → BoardServer<...>& — gives policy<P>() access
 */

#ifndef SFX_SERVER_H
#define SFX_SERVER_H

#include <Arduino.h>
#include <cstring>
#include <functional>

#include "serial/core/core.h"
#include "serial/core/system_service.h"   // BoardServer template + ServiceContext
#include "serial/core/board_service.h"    // BoardServicePolicy
#include "serial/core/packet_reader.h"    // PacketReader<Board>
#include "platform/diag_log.h"
#include "platform/sfx_platform.h"        // SFX_PLATFORM_*, SFX_FREE_HEAP, SFX_REBOOT, sfxGetBoardId
#include "led/led_control.h"
#include "indicators/indicator_leds.h"    // IndicatorServicePolicy

// Forward declarations
class I2CDevice;
class TwoWire;

// ============================================================================
// SfxServerBase — non-template state + helpers (deduplicates template bloat)
// ============================================================================

class SfxServerBase {
public:
    static constexpr uint32_t BAUD_RATE             = 6000000;
    static constexpr unsigned long CONNECTION_TIMEOUT_ms = 15000;
    static constexpr uint8_t MAX_EXPECTED_I2C = 8;

    /// Get the device name (e.g., "HubFx-A1B2").  Populated in begin().
    const char* deviceName() const { return _deviceName; }

    /// Enable/disable the connection-inactivity watchdog.  Masters
    /// (HubFX) disable it; expander boards keep it on.
    void setConnectionTimeoutEnabled(bool enabled) {
        _timeoutEnabled       = enabled;
        _timeoutEnabledByUser = enabled;
    }

    /// Convenience accessor.
    DiagLog& diagLog() { return DiagLog::instance(); }

    // I²C scan support — registered with BoardServicePolicy in begin().
    void addExpectedI2CDevice(uint8_t address, I2CDevice* device = nullptr);

protected:
    struct ExpectedI2CDevice {
        uint8_t address = 0;
        I2CDevice* device = nullptr;
    };

    void buildDeviceName(const char* prefix);
    I2CScanResult performI2CScan();

    char _deviceName[24] = {};

    std::function<void(uint8_t mode, uint8_t flags)> _initCb;
    std::function<void()>                            _shutdownCb;

    bool _timeoutEnabled        = true;
    bool _timeoutEnabledByUser  = true;
    bool _configLoaded          = false;

    TwoWire*           _i2cWire = nullptr;
    ExpectedI2CDevice  _expectedI2C[MAX_EXPECTED_I2C] = {};
    uint8_t            _numExpectedI2C = 0;
};

// ============================================================================
// SfxServer<...UserPolicies>
// ============================================================================

template <typename... UserPolicies>
class SfxServer : public SfxServerBase {
public:
    using Board = sfx_core::BoardServer<
        sfx_core::BoardServicePolicy,
        IndicatorServicePolicy,
        UserPolicies...
    >;

    SfxServer() = default;
    ~SfxServer() = default;

    SfxServer(const SfxServer&) = delete;
    SfxServer& operator=(const SfxServer&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────

    void begin(const char* prefix, const char* version, uint32_t buildNumber,
               int connectionPin = 13, int errorPin = 14) {
        // Serial setup
#ifdef ESP32
        Serial.setRxBufferSize(131072);
#endif
        Serial.begin(BAUD_RATE);
        while (!Serial && millis() < 3000) SFX_DELAY_MS(10);

        buildDeviceName(prefix);

        // DiagLog over serial
        DiagLog::instance().begin(&Serial);

        // Board (composes BoardServicePolicy + IndicatorServicePolicy + UserPolicies)
        _board.begin(&Serial);

        // Configure indicator LEDs
        _board.template policy<IndicatorServicePolicy>().configure(connectionPin, errorPin);

        // Seed lifecycle policy
        auto& core = _board.template policy<sfx_core::BoardServicePolicy>();
        core.setBoardInfo(_deviceName, version, SFX_PLATFORM_NAME,
                          SFX_CPU_MHZ(), SFX_FREE_HEAP(), buildNumber);
        core.setCapabilities(Board::capabilities());

        // Wire lifecycle callbacks
        core.onInit    ([this](uint8_t mode, uint8_t flags) { doInit(mode, flags); });
        core.onShutdown([this]()                            { doShutdown();       });
        core.onReboot  ([this]() {
            doShutdown();
            SFX_DELAY_MS(100);
            SFX_REBOOT();
        });
#if SFX_PLATFORM_PICO
        core.onBootsel([this]() {
            doShutdown();
            SFX_DELAY_MS(500);
            sfxRebootToBootloader();
        });
#endif

        // Frame reader — BoardServer is the sole dispatcher.  PacketReader
        // pumps COBS frames into board.dispatch(type, tag, payload, len),
        // which walks the policy tuple and NACKs if nothing owns the type.
        _reader.begin(&Serial, &_board);
    }

    // ── Lifecycle callbacks (controller-specific hooks) ──────────────

    void onInit(std::function<void(uint8_t mode, uint8_t flags)> cb) { _initCb = cb; }
    void onShutdown(std::function<void()> cb)                        { _shutdownCb = cb; }

    /// Mark that valid config was loaded — flips IDLE → STANDALONE so
    /// the slave reverts to STANDALONE (not IDLE) after disconnect.
    void markConfigLoaded() {
        _configLoaded = true;
        auto& core = _board.template policy<sfx_core::BoardServicePolicy>();
        if (core.boardState() == BoardState::IDLE) {
            core.setBoardState(BoardState::STANDALONE);
        }
    }
    bool isConfigLoaded() const { return _configLoaded; }

    // ── Main loop ─────────────────────────────────────────────────────

    void loop() {
        _reader.process();

        auto& core = _board.template policy<sfx_core::BoardServicePolicy>();
        if (_reader.lastActivityMs() > core.lastActivityMs()) {
            core.updateActivity();
        }
        core.updateFreeRam(SFX_FREE_HEAP());

        // Drive every policy's update() tick (status-broadcast + indicator LEDs + ...)
        _board.update();

        checkConnectionTimeout();
    }

    // ── Accessors ────────────────────────────────────────────────────

    Board&                        board()       { return _board; }
    const Board&                  board() const { return _board; }
    sfx_core::BoardServicePolicy& core()        { return _board.template policy<sfx_core::BoardServicePolicy>(); }
    IndicatorServicePolicy&       indicators()  { return _board.template policy<IndicatorServicePolicy>(); }

    // ── I²C scan (registered with BoardServicePolicy) ────────────────

    void enableI2CScan(TwoWire& wire) {
        _i2cWire = &wire;
        _board.template policy<sfx_core::BoardServicePolicy>().onI2CScan(
            [this]() -> I2CScanResult { return performI2CScan(); });
    }

private:
    Board                          _board;
    sfx_core::PacketReader<Board>  _reader;

    void doInit(uint8_t mode, uint8_t flags) {
        auto& core = _board.template policy<sfx_core::BoardServicePolicy>();
        auto& ind  = _board.template policy<IndicatorServicePolicy>();

        if (mode == InitMode::SLAVE) {
            _timeoutEnabled = _timeoutEnabledByUser;
            core.setBoardState(BoardState::SLAVE);
        } else if (mode == InitMode::DIRECT) {
            _timeoutEnabled = false;
            core.setBoardState(BoardState::DIRECT);
        }

        SFX_LOG_INFO("INIT: mode=%s flags=0x%02X verbose=%s state=%s",
                     InitMode::getName(mode), flags,
                     (flags & InitFlags::VERBOSE) ? "on" : "off",
                     BoardState::getName(core.boardState()));

        if (_initCb) _initCb(mode, flags);
        ind.setConnected(true);
        ind.setWatchdogTriggered(false);
    }

    void doShutdown() {
        if (_shutdownCb) _shutdownCb();
        auto& core = _board.template policy<sfx_core::BoardServicePolicy>();
        auto& ind  = _board.template policy<IndicatorServicePolicy>();
        ind.setConnected(false);
        _timeoutEnabled = _timeoutEnabledByUser;
        core.setBoardState(_configLoaded ? BoardState::STANDALONE : BoardState::IDLE);
    }

    void checkConnectionTimeout() {
        if (!_timeoutEnabled) return;
        auto& core = _board.template policy<sfx_core::BoardServicePolicy>();
        auto& ind  = _board.template policy<IndicatorServicePolicy>();
        if (core.checkTimeout(CONNECTION_TIMEOUT_ms)) {
            if (!ind.isWatchdogTriggered()) {
                SFX_LOG_WARN("Connection timeout (%lums inactivity)", CONNECTION_TIMEOUT_ms);
                doShutdown();
                ind.setWatchdogTriggered(true);
            }
        }
    }
};

// Backward-compatibility alias for callers that don't need the policy
// pack (yet).  No user policies = empty pack — just BoardServicePolicy +
// IndicatorServicePolicy.
using SfxServerLegacy = SfxServer<>;
using PicoServer      = SfxServer<>;

#endif // SFX_SERVER_H
