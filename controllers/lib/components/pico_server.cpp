/**
 * PicoServer — Common Pico Server Controller Boilerplate
 *
 * See pico_server.h for documentation.
 */

#include "pico_server.h"
#include <pico/unique_id.h>

// ============================================================================
// Initialization
// ============================================================================

void PicoServer::begin(const char* prefix, const char* version,
                       uint32_t buildNumber,
                       uint8_t connectionPin, uint8_t errorPin) {
    // Initialize USB serial
    Serial.begin(BAUD_RATE);
    while (!Serial && millis() < 3000) delay(10);

    // Build unique device name from Pico board ID
    buildDeviceName(prefix);

    // Initialize indicator LEDs (standard GP13/GP14)
    _indicators.begin(connectionPin, errorPin);

    // Initialize CoreCommandServer
    _core.begin(&Serial);
    _core.setBoardInfo(_deviceName, version, "RP2040",
                       F_CPU / 1000000, rp2040.getFreeHeap(), buildNumber);

    // Standard system callbacks
    _core.onInit([this]() { doInit(); });
    _core.onShutdown([this]() { doShutdown(); });

    _core.onReboot([this]() {
        doShutdown();
        delay(100);
        rp2040.reboot();
    });

    _core.onBootsel([this]() {
        doShutdown();
        delay(500);
        rp2040.rebootToBootloader();
    });
}

void PicoServer::addModuleHandler(ICommandHandler* handler) {
    // Initialize CommandRouter with NACK callback
    _router.begin(&Serial, [this](uint8_t code, uint8_t /* type */) {
        _core.sendNack(code);
    });

    // Add handlers in priority order
    _router.addHandler(&_core);        // Priority 1: core/system commands
    if (handler) {
        _router.addHandler(handler);   // Priority 2: module commands
    }
}

// ============================================================================
// Loop Processing
// ============================================================================

void PicoServer::loop() {
    // Process incoming serial packets via CommandRouter
    _router.process();

    // Forward activity timestamp to core handler for timeout detection
    if (_router.lastActivityMs() > _core.lastActivityMs()) {
        _core.updateActivity();
    }

    // Keep free RAM current for STATUS response
    _core.updateFreeRam(rp2040.getFreeHeap());

    // Check connection timeout
    checkConnectionTimeout();

    // Update indicator LEDs
    _indicators.update();
}

// ============================================================================
// Internal Helpers
// ============================================================================

void PicoServer::buildDeviceName(const char* prefix) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    snprintf(_deviceName, sizeof(_deviceName), "%s-%02X%02X",
             prefix, id.id[6], id.id[7]);
}

void PicoServer::doInit() {
    if (_initCb) _initCb();
    _indicators.setConnected(true);
    _indicators.setWatchdogTriggered(false);
}

void PicoServer::doShutdown() {
    if (_shutdownCb) _shutdownCb();
    _indicators.setConnected(false);
}

void PicoServer::checkConnectionTimeout() {
    if (_core.checkTimeout(CONNECTION_TIMEOUT_ms)) {
        if (!_indicators.isWatchdogTriggered()) {
            doShutdown();
            _indicators.setWatchdogTriggered(true);
        }
    }
}
