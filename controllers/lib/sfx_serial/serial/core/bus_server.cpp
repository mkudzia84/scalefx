/*
 * Bus Server — Base Class Implementation + CoreCommandServer
 *
 * Common server boilerplate: serial I/O, COBS encoding, ACK/NACK/ERROR.
 * CoreCommandServer handles core system commands (INIT, SHUTDOWN, etc.).
 * Module-specific servers extend BusServer with their own handleModulePacket().
 */

#include "bus_server.h"
#include "platform/diag_log.h"  // For DIAG_HISTORY handler

using namespace CoreProtocol;

// ============================================================================
// Lifecycle
// ============================================================================

bool BusServer::begin(Stream* serial) {
    if (!serial) return false;
    _serial = serial;
    _initialized = true;
    return true;
}

void BusServer::end() {
    _serial = nullptr;
    _initialized = false;
}

// ============================================================================
// ICommandHandler Interface
// ============================================================================

CommandHandleResult BusServer::tryProcess(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_initialized || !_serial) return CommandHandleResult::NotMyCommand;

    if (type >= moduleRangeLow() && type <= moduleRangeHigh()) {
        return handleModulePacket(type, payload, len);
    }

    return CommandHandleResult::NotMyCommand;
}

// ============================================================================
// Response Methods
// ============================================================================

int BusServer::sendRawPacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) {
    if (!_serial) return -1;

    uint8_t buffer[COBS_BUFFER_SIZE];
    size_t encodedLen = encodePacket(buffer, type, tag, payload, len);
    // encodePacket() already appends FRAME_DELIMITER (0x00) at the end

    if (encodedLen == 0) return -1;

    size_t written = _serial->write(buffer, encodedLen);
    return (int)written;
}

int BusServer::sendAck() {
    return sendRawPacket(CorePacket::ACK, _currentTag, nullptr, 0);
}

int BusServer::sendNack(uint8_t errorCode, const char* reason) {
    uint8_t payload[64];
    payload[0] = errorCode;

    const char* msg = (reason && reason[0]) ? reason : getModuleErrorMessage(errorCode);
    size_t msgLen = strlen(msg);
    if (msgLen > sizeof(payload) - 1) {
        msgLen = sizeof(payload) - 1;
    }
    memcpy(&payload[1], msg, msgLen);

    return sendRawPacket(CorePacket::NACK, _currentTag, payload, 1 + msgLen);
}

int BusServer::sendError(uint8_t errorCode, const char* message) {
    uint8_t payload[64];
    payload[0] = errorCode;

    size_t len = 1;
    if (message) {
        size_t msgLen = strlen(message);
        if (msgLen > sizeof(payload) - 1) {
            msgLen = sizeof(payload) - 1;
        }
        memcpy(&payload[1], message, msgLen);
        len += msgLen;
    }

    return sendRawPacket(CorePacket::ERROR, TAG_ASYNC, payload, len);
}

// ============================================================================
// CoreCommandServer Implementation
// ============================================================================

void CoreCommandServer::begin(Stream* serial) {
    BusServer::begin(serial);
    _initReceived = false;
    _lastActivityMs = 0;
    _prevActivityMs = 0;
}

void CoreCommandServer::setBoardInfo(const char* deviceName, const char* firmwareVersion,
                                     const char* platform, uint32_t cpuMHz, uint32_t freeRam,
                                     uint32_t buildNumber) {
    if (deviceName) {
        strncpy(_boardInfo.deviceName, deviceName, sizeof(_boardInfo.deviceName) - 1);
        _boardInfo.deviceName[sizeof(_boardInfo.deviceName) - 1] = '\0';
    }
    if (firmwareVersion) {
        // Strip "v" or "V" prefix if present
        const char* ver = firmwareVersion;
        if (ver[0] == 'v' || ver[0] == 'V') ver++;
        strncpy(_boardInfo.firmwareVersion, ver, sizeof(_boardInfo.firmwareVersion) - 1);
        _boardInfo.firmwareVersion[sizeof(_boardInfo.firmwareVersion) - 1] = '\0';
    }
    if (platform) {
        strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
        _boardInfo.platform[sizeof(_boardInfo.platform) - 1] = '\0';
    }
    _boardInfo.cpuFrequencyMHz = cpuMHz;
    _boardInfo.freeRamBytes = freeRam;
    _boardInfo.buildNumber = buildNumber;
}

CommandHandleResult CoreCommandServer::tryProcess(uint8_t type, const uint8_t* payload, size_t len) {
    // Override BusServer's tryProcess to skip the _initialized check.
    // Core commands (especially INIT) must be processed before the INIT
    // handshake is complete. We only need the serial port to be set.
    if (!_serial) return CommandHandleResult::NotMyCommand;

    if (type >= moduleRangeLow() && type <= moduleRangeHigh()) {
        return handleModulePacket(type, payload, len);
    }

    return CommandHandleResult::NotMyCommand;
}

CommandHandleResult CoreCommandServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
    _prevActivityMs = _lastActivityMs;
    _lastActivityMs = millis();
    // Increment with overflow protection (wrap to 1, not 0, to distinguish from reset)
    if (_commandCounter == UINT32_MAX) {
        _commandCounter = 1;
    } else {
        _commandCounter++;
    }

    switch (type) {
        case CorePacket::INIT:
            handleInit(payload, len);
            return CommandHandleResult::Handled;

        case CorePacket::SHUTDOWN:
            BusServer::sendAck();
            if (_shutdownCallback) _shutdownCallback();
            return CommandHandleResult::Handled;

        case CorePacket::REBOOT:
            // No ACK - device reboots immediately
            if (_rebootCallback) _rebootCallback();
            return CommandHandleResult::Handled;

        case CorePacket::BOOTSEL:
            if (_bootselCallback) {
                // No ACK — device enters bootloader immediately (Pico BOOTSEL mode)
                _bootselCallback();
            } else {
                // Not supported on this platform (e.g., ESP32 has no UF2 bootloader)
                sendNack(SerialError::NOT_SUPPORTED);
            }
            return CommandHandleResult::Handled;

        case CorePacket::KEEPALIVE:
            // Keepalive updates activity timer (already done above) and sends ACK
            // Increment with overflow protection (wrap to 1, not 0, to distinguish from reset)
            if (_keepaliveCounter == UINT32_MAX) {
                _keepaliveCounter = 1;
            } else {
                _keepaliveCounter++;
            }
            BusServer::sendAck();
            if (_keepaliveCallback) _keepaliveCallback();
            return CommandHandleResult::Handled;

        case CorePacket::STATUS_REQ:
            sendStatus();
            return CommandHandleResult::Handled;

        case CorePacket::IDENTIFY:
            // Return board info without triggering init callbacks or state changes.
            // Same payload format as INIT_READY, but response type is IDENTIFY.
            sendIdentify();
            return CommandHandleResult::Handled;

        case CorePacket::I2C_SCAN:
            if (_i2cScanCallback) {
                I2CScanResult result = _i2cScanCallback();
                sendI2CScanResult(result);
            } else {
                BusServer::sendNack(SerialError::NOT_SUPPORTED);
            }
            return CommandHandleResult::Handled;

        case CorePacket::DIAG_HISTORY: {
            // Send all buffered log messages WITHOUT draining the buffer.
            // Messages are sent as LOG_MESSAGE packets, then ACK with count.
            uint16_t sent = DiagLog::instance().sendHistory();
            uint8_t countPayload[2];
            CoreProtocol::putU16LE(countPayload, sent);
            BusServer::sendRawPacket(CorePacket::ACK, _currentTag, countPayload, 2);
            return CommandHandleResult::Handled;
        }

        default:
            _commandCounter--;  // Don't count unhandled commands
            return CommandHandleResult::NotMyCommand;
    }
}

bool CoreCommandServer::checkTimeout(unsigned long timeoutMs) {
    if (timeoutMs == 0 || _lastActivityMs == 0) return false;

    if (millis() - _lastActivityMs > timeoutMs) {
        if (_initReceived) {
            reset();
        }
        return true;
    }
    return false;
}

void CoreCommandServer::reset() {
    _initReceived = false;
}

void CoreCommandServer::handleInit(const uint8_t* payload, size_t len) {
    (void)payload;
    (void)len;

    // If already initialized, this is a reconnection
    if (_initReceived) {
        reset();
    }

    // Send INIT_READY response
    sendInitReady();

    _initReceived = true;

    if (_initCallback) {
        _initCallback();
    }
}

void CoreCommandServer::sendInitReady() {
    uint8_t payload[64];
    size_t len = CorePayload::encodeInitReady(_boardInfo, payload);
    sendRawPacket(CorePacket::INIT_READY, _currentTag, payload, len);
}

void CoreCommandServer::sendIdentify() {
    // Same payload as INIT_READY, but uses IDENTIFY packet type.
    // Does NOT set _initReceived or fire _initCallback.
    uint8_t payload[64];
    size_t len = CorePayload::encodeInitReady(_boardInfo, payload);
    sendRawPacket(CorePacket::IDENTIFY, _currentTag, payload, len);
}

void CoreCommandServer::sendStatus() {
    // STATUS payload: [counter:u32LE][uptime:u32LE][freeRam:u32LE]
    //                 [lastActivity_ms:u32LE][keepaliveCount:u32LE][moduleData:0-N]
    uint8_t payload[CoreProtocol::MAX_PAYLOAD_SIZE];
    size_t idx = 0;

    // Core header (20 bytes)
    CoreProtocol::putU32LE(&payload[idx], _commandCounter);
    idx += 4;
    CoreProtocol::putU32LE(&payload[idx], millis());
    idx += 4;
    CoreProtocol::putU32LE(&payload[idx], _boardInfo.freeRamBytes);
    idx += 4;
    // Time since previous activity (ms) — excludes this STATUS_REQ itself
    // Uses _prevActivityMs so the report reflects the gap *before* this command
    uint32_t sinceActivity = (_prevActivityMs > 0) ? (millis() - _prevActivityMs) : 0;
    CoreProtocol::putU32LE(&payload[idx], sinceActivity);
    idx += 4;
    CoreProtocol::putU32LE(&payload[idx], _keepaliveCounter);
    idx += 4;

    // Module-specific data (appended by callback)
    if (_statusDataCallback) {
        size_t moduleLen = _statusDataCallback(&payload[idx], sizeof(payload) - idx);
        idx += moduleLen;
    }

    sendRawPacket(CorePacket::STATUS, _currentTag, payload, idx);
}

void CoreCommandServer::sendI2CScanResult(const I2CScanResult& result) {
    // Wire format: [numExpected:u8][N×(addr:u8,found:u8,id:u8)][numExtra:u8][M×addr:u8]
    uint8_t payload[CoreProtocol::MAX_PAYLOAD_SIZE];
    size_t idx = 0;

    payload[idx++] = result.numExpected;
    for (uint8_t i = 0; i < result.numExpected && i < I2CScanResult::MAX_EXPECTED; i++) {
        payload[idx++] = result.expected[i].address;
        payload[idx++] = result.expected[i].found ? 1 : 0;
        payload[idx++] = result.expected[i].identified ? 1 : 0;
    }
    payload[idx++] = result.numExtra;
    for (uint8_t i = 0; i < result.numExtra && i < I2CScanResult::MAX_EXTRA; i++) {
        if (idx >= sizeof(payload)) break;
        payload[idx++] = result.extraAddresses[i];
    }

    sendRawPacket(CorePacket::I2C_SCAN_RESULT, _currentTag, payload, idx);
}
