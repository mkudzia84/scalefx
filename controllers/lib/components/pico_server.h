/**
 * PicoServer — Common Pico Server Controller Boilerplate
 *
 * Encapsulates the lifecycle boilerplate shared across all ScaleFX Pico
 * server controllers (GunFX, LightFX, GearControl, etc.):
 *
 *   - USB serial initialization (115200 baud)
 *   - Unique device name from Pico board ID
 *   - Indicator LEDs (GP13=connection, GP14=error, standard across all boards)
 *   - CoreCommandServer with board info, INIT/SHUTDOWN/REBOOT/BOOTSEL callbacks
 *   - CommandRouter with automatic handler priority (core first, then module)
 *   - Connection timeout / watchdog detection
 *   - Common loop() tasks (router process, activity forwarding, free RAM, indicators)
 *
 * Usage:
 *   PicoServer server;
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
 *       delay(1);
 *   }
 */

#ifndef PICO_SERVER_H
#define PICO_SERVER_H

#include <Arduino.h>
#include <functional>
#include <serial.h>
#include "indicator_leds.h"

class PicoServer {
public:
    PicoServer() = default;
    ~PicoServer() = default;

    /**
     * @brief Initialize common server infrastructure
     *
     * Sets up USB serial (115200 baud), builds device name from Pico
     * unique board ID, initializes indicator LEDs, and configures
     * CoreCommandServer with board info and standard system callbacks
     * (INIT, SHUTDOWN, REBOOT, BOOTSEL).
     *
     * @param prefix   Device name prefix (e.g. "GunFX" → "GunFX-A1B2")
     * @param version  Firmware version string (e.g. "0.3.0")
     * @param buildNumber  Build number for INIT_READY response
     * @param connectionPin  GPIO for connection indicator LED (default GP13)
     * @param errorPin       GPIO for error indicator LED (default GP14)
     */
    void begin(const char* prefix, const char* version, uint32_t buildNumber,
               uint8_t connectionPin = 13, uint8_t errorPin = 14);

    /**
     * @brief Register controller-specific init callback
     *
     * Called when INIT command is received. The callback should reset
     * hardware to a safe initial state. PicoServer automatically handles
     * indicator LED state (connected=true, watchdog=false) after the callback.
     */
    void onInit(std::function<void()> cb) { _initCb = cb; }

    /**
     * @brief Register controller-specific shutdown callback
     *
     * Called on SHUTDOWN command, connection timeout, REBOOT, and BOOTSEL.
     * The callback should put hardware in a safe state. PicoServer automatically
     * handles indicator LED state (connected=false) after the callback.
     */
    void onShutdown(std::function<void()> cb) { _shutdownCb = cb; }

    /**
     * @brief Add module-specific command handler and finalize router
     *
     * Initializes the CommandRouter with the standard handler chain:
     *   1. CoreCommandServer (INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE, STATUS)
     *   2. Module handler (e.g. GunFxServer, LightFxServer)
     *
     * Must be called after all callbacks are registered.
     *
     * @param handler  Module-specific ICommandHandler (nullptr for core-only)
     */
    void addModuleHandler(ICommandHandler* handler);

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

    /** @brief Get device name (e.g. "GunFX-A1B2") */
    const char* deviceName() const { return _deviceName; }

private:
    CommandRouter _router;
    CoreCommandServer _core;
    IndicatorLedManager _indicators;
    char _deviceName[24];

    std::function<void()> _initCb;
    std::function<void()> _shutdownCb;

    static constexpr uint32_t BAUD_RATE = 115200;
    static constexpr unsigned long CONNECTION_TIMEOUT_ms = 15000;

    void buildDeviceName(const char* prefix);
    void doInit();
    void doShutdown();
    void checkConnectionTimeout();
};

#endif // PICO_SERVER_H
