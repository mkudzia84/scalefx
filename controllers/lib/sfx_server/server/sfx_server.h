/**
 * SfxServer — Common ScaleFX Server Controller Boilerplate
 *
 * Encapsulates the lifecycle boilerplate shared across all ScaleFX
 * server controllers (GunFX, LightFX, GearControl, etc.):
 *
 *   - USB serial initialization (1Mbps baud)
 *   - Unique device name from board ID
 *   - Indicator LEDs (GP13=connection, GP14=error, standard across all boards)
 *   - CoreCommandServer with board info, INIT/SHUTDOWN/REBOOT/BOOTSEL callbacks
 *   - CommandRouter with automatic handler priority (core first, then module)
 *   - Connection timeout / watchdog detection
 *   - Common loop() tasks (router process, activity forwarding, free RAM, indicators)
 *
 * Cross-platform: supports RP2040, RP2350, and ESP32-S3 via sfx_platform.h.
 *
 * Usage:
 *   SfxServer server;
 *   GunFxServer gunfxServer;
 *
 *   void setup() {
 *       server.begin("GunFX", FIRMWARE_VERSION, BUILD_NUMBER);
 *       server.onInit([]()     { resetHardware(); });
 *       server.onShutdown([]() { safeHardware();  });
 *
 *       gunfxServer.begin(&Serial, server.deviceName());
 *       // ... register module callbacks ...
 *       server.core().onStatusData([](uint8_t* buf, size_t max) -> size_t { ... });
 *
 *       server.addModuleHandler(&gunfxServer);
 *   }
 *
 *   void loop() {
 *       server.loop();       // protocol, timeout, indicators
 *       updateHardware();    // module-specific work
 *       SFX_DELAY_MS(1);
 *   }
 */

#ifndef SFX_SERVER_H
#define SFX_SERVER_H

#include <Arduino.h>
#include <functional>
#include "serial/core/core.h"
#include "serial/core/bus_server.h"
#include "platform/diag_log.h"
#include "led/led_control.h"

// Forward declaration
class I2CDevice;
class TwoWire;

class SfxServer {
public:
    // ========================================================================
    // IndicatorLedManager — nested class (only used by SfxServer)
    // ========================================================================

    /**
     * @brief Manages standardized indicator LEDs for ScaleFX server controllers
     *
     * Encapsulates the connection/error LED pattern used identically across
     * all Pico server controllers (GunFX, LightFX, GearControl).
     *
     * LED 0 (Connection, GP13):
     *   - Blink 500ms: Waiting for INIT (power-on, not yet connected)
     *   - Solid ON:    Connected and initialized
     *   - OFF:         Connection lost (watchdog timeout triggered)
     *
     * LED 1 (Error/Warning, GP14, three-tier priority):
     *   - Fast blink 200ms: Error condition (highest priority)
     *   - Slow blink 500ms: Warning condition (e.g. low voltage)
     *   - OFF:              Normal operation
     *
     * Also tracks the `connected` and `watchdogTriggered` state that was
     * previously duplicated as bare `bool` variables in every controller.
     */
    class IndicatorLedManager {
    public:
        IndicatorLedManager() = default;
        ~IndicatorLedManager() = default;

        /** @brief Initialize indicator LEDs on specified pins (-1 = disabled) */
        void begin(int connectionPin, int errorPin);

        /** @brief Update indicator LED outputs based on current state (call every loop) */
        void update();

        // State setters
        void setConnected(bool connected) { _connected = connected; }
        void setWatchdogTriggered(bool triggered) { _watchdogTriggered = triggered; }
        void setErrorCondition(bool error) { _errorCondition = error; }
        void setWarningCondition(bool warning) { _warningCondition = warning; }

        // State queries
        bool isConnected() const { return _connected; }
        bool isWatchdogTriggered() const { return _watchdogTriggered; }
        const LedControl& connectionLed() const { return _leds[0]; }
        const LedControl& errorLed() const { return _leds[1]; }

    private:
        LedControl _leds[2];
        bool _connected = false;
        bool _watchdogTriggered = false;
        bool _errorCondition = false;
        bool _warningCondition = false;

        static constexpr uint16_t BLINK_WAITING_ms = 500;
        static constexpr uint16_t BLINK_ERROR_ms   = 200;
        static constexpr uint16_t BLINK_WARNING_ms = 500;
    };

    SfxServer() = default;
    ~SfxServer() = default;

    /**
     * @brief Initialize common server infrastructure
     *
     * Sets up USB serial (1Mbps baud), builds device name from board
     * unique ID, initializes indicator LEDs, and configures
     * CoreCommandServer with board info and standard system callbacks
     * (INIT, SHUTDOWN, REBOOT, BOOTSEL).
     *
     * @param prefix   Device name prefix (e.g. "GunFX" → "GunFX-A1B2")
     * @param version  Firmware version string (e.g. "0.3.0")
     * @param buildNumber  Build number for INIT_READY response
     * @param connectionPin  GPIO for connection indicator LED (default 13, -1 = disabled)
     * @param errorPin       GPIO for error indicator LED (default 14, -1 = disabled)
     */
    void begin(const char* prefix, const char* version, uint32_t buildNumber,
               int connectionPin = 13, int errorPin = 14);

    /**
     * @brief Register controller-specific init callback
     *
     * Called when INIT command is received. The callback should reset
     * hardware to a safe initial state. SfxServer automatically handles
     * indicator LED state (connected=true, watchdog=false) after the callback.
     */
    void onInit(std::function<void()> cb) { _initCb = cb; }

    /**
     * @brief Register controller-specific shutdown callback
     *
     * Called on SHUTDOWN command, connection timeout, REBOOT, and BOOTSEL.
     * The callback should put hardware in a safe state. SfxServer automatically
     * handles indicator LED state (connected=false) after the callback.
     */
    void onShutdown(std::function<void()> cb) { _shutdownCb = cb; }

    /**
     * @brief Add a module-specific command handler to the router
     *
     * On first call, initializes the CommandRouter and registers the
     * CoreCommandServer at priority 1. Each subsequent call adds another
     * module handler in the order called. Supports multiple handlers for
     * multi-domain controllers (e.g., HubFX with audio, engine, storage).
     *
     * Handler chain (example with 3 module handlers):
     *   1. CoreCommandServer (auto-registered)
     *   2. First addModuleHandler() call
     *   3. Second addModuleHandler() call
     *   4. Third addModuleHandler() call
     *
     * Must be called after all callbacks are registered.
     *
     * @param handler  Module-specific ICommandHandler (nullptr for core-only init)
     */
    void addModuleHandler(ICommandHandler* handler);

    /**
     * @brief Enable/disable automatic connection timeout
     *
     * When disabled, SfxServer will not call doShutdown() on serial
     * inactivity. Use for master controllers (like HubFX) that operate
     * independently of any upstream serial connection.
     *
     * Default: enabled (standard slave controller behavior)
     *
     * @param enabled true = timeout active, false = disabled
     */
    void setConnectionTimeoutEnabled(bool enabled) { _timeoutEnabled = enabled; }

    /**
     * @brief Process common loop tasks
     *
     * Handles: serial packet routing, activity timestamp forwarding,
     * free RAM update, connection timeout/watchdog, indicator LED update.
     * Call this once per loop() iteration before module-specific updates.
     */
    void loop();

    // ========================================================================
    // Accessors — for module-specific setup and queries
    // ========================================================================

    /** @brief Access CommandRouter (e.g. for advanced routing) */
    CommandRouter& router() { return _router; }

    /** @brief Access CoreCommandServer (e.g. for onStatusData callback) */
    CoreCommandServer& core() { return _core; }

    /** @brief Access IndicatorLedManager (e.g. for error/warning conditions) */
    IndicatorLedManager& indicators() { return _indicators; }

    /** @brief Access DiagLog singleton (convenience, same as DiagLog::instance()) */
    DiagLog& diagLog() { return DiagLog::instance(); }

    /** @brief Get device name (e.g. "GunFX-A1B2") */
    const char* deviceName() const { return _deviceName; }

    // ========================================================================
    // I2C Bus Scan Support
    // ========================================================================

    /**
     * @brief Enable I2C bus scanning via the I2C_SCAN core command
     *
     * Registers an I2C scan callback with CoreCommandServer so that when
     * an I2C_SCAN packet is received, the bus is probed automatically.
     * Call addExpectedI2CDevice() before this to register expected devices.
     *
     * @param wire TwoWire instance to scan (e.g. Wire, Wire1)
     */
    void enableI2CScan(TwoWire& wire);

    /**
     * @brief Register an expected I2C device for bus scan reporting
     *
     * Expected devices are reported first in I2C_SCAN_RESULT with found/identified
     * flags. Any other devices on the bus are listed as "extra".
     *
     * @param address 7-bit I2C slave address
     * @param device Optional I2CDevice instance for identity verification
     *               (isAvailable() is used for the "identified" flag)
     */
    void addExpectedI2CDevice(uint8_t address, I2CDevice* device = nullptr);

private:
    CommandRouter _router;
    CoreCommandServer _core;
    IndicatorLedManager _indicators;
    char _deviceName[24];

    std::function<void()> _initCb;
    std::function<void()> _shutdownCb;

    bool _routerInitialized = false;
    bool _timeoutEnabled = true;

    static constexpr uint32_t BAUD_RATE = 6000000;
    static constexpr unsigned long CONNECTION_TIMEOUT_ms = 15000;

    void buildDeviceName(const char* prefix);
    void doInit();
    void doShutdown();
    void checkConnectionTimeout();

    // I2C scan support
    static constexpr uint8_t MAX_EXPECTED_I2C = 8;
    struct ExpectedI2CDevice {
        uint8_t address = 0;
        I2CDevice* device = nullptr;
    };
    TwoWire* _i2cWire = nullptr;
    ExpectedI2CDevice _expectedI2C[MAX_EXPECTED_I2C];
    uint8_t _numExpectedI2C = 0;
    I2CScanResult performI2CScan();
};

// Backward compatibility alias (deprecated — use SfxServer directly)
using PicoServer = SfxServer;

#endif // SFX_SERVER_H
