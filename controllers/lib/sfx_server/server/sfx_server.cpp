/**
 * SfxServer — Common ScaleFX Server Controller Boilerplate
 *
 * Cross-platform server infrastructure for RP2040, RP2350, and ESP32-S3.
 * See sfx_server.h for documentation.
 */

#include "sfx_server.h"
#include "platform/sfx_platform.h"
#include <Wire.h>
#include "power/i2c_device.h"

// ============================================================================
// IndicatorLedManager (SfxServer nested class)
// ============================================================================

void SfxServer::IndicatorLedManager::begin(int connectionPin, int errorPin) {
    if (connectionPin >= 0) _leds[0].begin(connectionPin);
    if (errorPin >= 0)      _leds[1].begin(errorPin);
}

void SfxServer::IndicatorLedManager::update() {
    // LED 0: Connection status
    if (_watchdogTriggered) {
        _leds[0].off();
    } else if (!_connected) {
        _leds[0].set((millis() / BLINK_WAITING_ms) % 2);  // Slow blink: waiting for INIT
    } else {
        _leds[0].on();  // Solid: connected
    }

    // LED 1: Error/warning status (three-tier priority)
    if (_errorCondition) {
        _leds[1].set((millis() / BLINK_ERROR_ms) % 2);    // Fast blink: error
    } else if (_warningCondition) {
        _leds[1].set((millis() / BLINK_WARNING_ms) % 2);  // Slow blink: warning
    } else {
        _leds[1].off();                                     // Off: normal
    }
}

// ============================================================================
// Initialization
// ============================================================================

void SfxServer::begin(const char* prefix, const char* version,
                      uint32_t buildNumber,
                      int connectionPin, int errorPin) {
    // Initialize USB serial
    // ESP32 UART default RX buffer is 256 bytes — too small for MAX_PAYLOAD_SIZE
    // (2048) packets which COBS-encode to ~2060+ bytes. Must set before begin().
    // 128 KB holds ~64 full packets (~220ms at 6 Mbps line rate), matching the
    // default window size for sliding-window uploads. This provides headroom for
    // burst pipelining while Core 1 performs async SD card writes.
#ifdef ESP32
    Serial.setRxBufferSize(131072);
#endif
    Serial.begin(BAUD_RATE);
    while (!Serial && millis() < 3000) SFX_DELAY_MS(10);

    // Build unique device name from board ID
    buildDeviceName(prefix);

    // Initialize indicator LEDs (standard GP13/GP14)
    _indicators.begin(connectionPin, errorPin);

    // Initialize diagnostic log singleton (buffered COBS log packets over serial)
    DiagLog::instance().begin(&Serial);

    // Initialize CoreCommandServer
    _core.begin(&Serial);
    _core.setBoardInfo(_deviceName, version, SFX_PLATFORM_NAME,
                       SFX_CPU_MHZ(), SFX_FREE_HEAP(), buildNumber);

    // Standard system callbacks
    _core.onInit([this]() { doInit(); });
    _core.onShutdown([this]() { doShutdown(); });

    _core.onReboot([this]() {
        doShutdown();
        SFX_DELAY_MS(100);
        SFX_REBOOT();
    });

#if SFX_PLATFORM_PICO
    _core.onBootsel([this]() {
        doShutdown();
        SFX_DELAY_MS(500);
        sfxRebootToBootloader();
    });
#endif
    // ESP32: no BOOTSEL callback — CoreCommandServer sends NACK NOT_SUPPORTED

    // Initialize command router with core handler — MUST happen in begin()
    // so core commands (INIT, IDENTIFY, STATUS, etc.) work even if no module
    // handlers are registered (e.g., during feature-flagged board bring-up).
    _router.begin(&Serial, [this](uint8_t code, uint8_t /* type */) {
        _core.sendNack(code);
    });
    _router.addHandler(&_core);  // Priority 1: core/system commands
    _routerInitialized = true;
}

void SfxServer::addModuleHandler(ICommandHandler* handler) {
    // Add module handler (supports multiple calls for multi-domain servers)
    if (handler) {
        _router.addHandler(handler);
    }
}

// ============================================================================
// Loop Processing
// ============================================================================

void SfxServer::loop() {
    // Process incoming serial packets via CommandRouter
    _router.process();

    // Forward activity timestamp to core handler for timeout detection
    if (_router.lastActivityMs() > _core.lastActivityMs()) {
        _core.updateActivity();
    }

    // Keep free RAM current for STATUS response
    _core.updateFreeRam(SFX_FREE_HEAP());

    // DiagLog uses a rolling ring buffer — log messages are retrieved
    // on-demand via DIAG_HISTORY command (non-draining read).

    // Check connection timeout (inactivity)
    checkConnectionTimeout();

    // Update indicator LEDs
    _indicators.update();
}

// ============================================================================
// Internal Helpers
// ============================================================================

void SfxServer::buildDeviceName(const char* prefix) {
    char boardId[16];
    sfxGetBoardId(boardId, sizeof(boardId));
    // sfxGetBoardId returns 8 hex chars — use last 4 for compact suffix
    size_t len = strlen(boardId);
    const char* suffix = (len >= 4) ? &boardId[len - 4] : boardId;
    snprintf(_deviceName, sizeof(_deviceName), "%s-%s", prefix, suffix);
}

void SfxServer::doInit() {
    if (_initCb) _initCb();
    _indicators.setConnected(true);
    _indicators.setWatchdogTriggered(false);
}

void SfxServer::doShutdown() {
    if (_shutdownCb) _shutdownCb();
    _indicators.setConnected(false);
}

void SfxServer::checkConnectionTimeout() {
    if (!_timeoutEnabled) return;

    if (_core.checkTimeout(CONNECTION_TIMEOUT_ms)) {
        if (!_indicators.isWatchdogTriggered()) {
            SFX_LOG_WARN("Connection timeout (%lums inactivity)", CONNECTION_TIMEOUT_ms);
            doShutdown();
            _indicators.setWatchdogTriggered(true);
        }
    }
}


// ============================================================================
// I2C Bus Scan Support
// ============================================================================

void SfxServer::enableI2CScan(TwoWire& wire) {
    _i2cWire = &wire;
    _core.onI2CScan([this]() -> I2CScanResult {
        return performI2CScan();
    });
}

void SfxServer::addExpectedI2CDevice(uint8_t address, I2CDevice* device) {
    if (_numExpectedI2C < MAX_EXPECTED_I2C) {
        _expectedI2C[_numExpectedI2C].address = address;
        _expectedI2C[_numExpectedI2C].device = device;
        _numExpectedI2C++;
    }
}

I2CScanResult SfxServer::performI2CScan() {
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
