/**
 * PicoServer — Common Pico Server Controller Boilerplate
 *
 * See pico_server.h for documentation.
 */

#include "pico_server.h"
#include <pico/unique_id.h>
#include <Wire.h>
#include <i2c_device.h>

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

// ============================================================================
// I2C Bus Scan Support
// ============================================================================

void PicoServer::enableI2CScan(TwoWire& wire) {
    _i2cWire = &wire;
    _core.onI2CScan([this]() -> I2CScanResult {
        return performI2CScan();
    });
}

void PicoServer::addExpectedI2CDevice(uint8_t address, I2CDevice* device) {
    if (_numExpectedI2C < MAX_EXPECTED_I2C) {
        _expectedI2C[_numExpectedI2C].address = address;
        _expectedI2C[_numExpectedI2C].device = device;
        _numExpectedI2C++;
    }
}

I2CScanResult PicoServer::performI2CScan() {
    I2CScanResult result;

    if (!_i2cWire) return result;

    // Check each expected device
    result.numExpected = _numExpectedI2C;
    for (uint8_t i = 0; i < _numExpectedI2C; i++) {
        result.expected[i].address = _expectedI2C[i].address;
        result.expected[i].found = I2CDevice::probe(*_i2cWire, _expectedI2C[i].address);
        result.expected[i].identified = _expectedI2C[i].device != nullptr
                                     && _expectedI2C[i].device->isAvailable();
    }

    // Scan wider I2C range for unexpected devices (0x08-0x77, standard range)
    uint8_t allAddrs[32];
    uint8_t totalFound = I2CDevice::scan(*_i2cWire, 0x08, 0x77, allAddrs, 32);

    // Filter out expected addresses to find extras
    result.numExtra = 0;
    for (uint8_t j = 0; j < totalFound; j++) {
        bool isExpected = false;
        for (uint8_t i = 0; i < _numExpectedI2C; i++) {
            if (allAddrs[j] == _expectedI2C[i].address) {
                isExpected = true;
                break;
            }
        }
        if (!isExpected && result.numExtra < I2CScanResult::MAX_EXTRA) {
            result.extraAddresses[result.numExtra++] = allAddrs[j];
        }
    }

    return result;
}
