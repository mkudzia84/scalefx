/*
 * Bus Client — Base Class for USB Host Client Controllers (Implementation)
 *
 * Extracts the common boilerplate shared by all ScaleFX client controllers
 * (GunFxClient, LightFxClient, GearControlClient).
 */

#include "bus_client.h"
#include "bus.h"

using namespace CoreProtocol;

// ============================================================================
// Lifecycle
// ============================================================================

bool BusClient::begin(UsbHost* usbHost, int deviceIndex) {
    if (!SerialBus::begin(usbHost, deviceIndex)) {
        return false;
    }

    _usbHostRef = usbHost;
    _serverReady = false;
    _serverName[0] = '\0';
    memset(&_boardInfo, 0, sizeof(_boardInfo));

    // Route all incoming packets through handlePacket
    SerialBus::onPacketReceived([this](uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) {
        handlePacket(type, tag, payload, len);
    });

    return true;
}

int BusClient::process() {
    return SerialBus::process();
}

// ============================================================================
// Connection Management
// ============================================================================

int BusClient::sendInit(unsigned long keepaliveMs) {
    if (!_usbHostRef) return -1;

    char buf[64];
    if (keepaliveMs > 0) {
        snprintf(buf, sizeof(buf), "INIT protocol=binary keepalive=%lu", keepaliveMs);
    } else {
        snprintf(buf, sizeof(buf), "INIT protocol=binary keepalive=off");
    }

    int written = _usbHostRef->cdcPrintln(SerialBus::deviceIndex(), buf);
    if (written > 0) {
        _lastSendMs = millis();
    }
    return written;
}

// ============================================================================
// Command Execution
// ============================================================================

CommandResult BusClient::sendCommand(uint8_t type, const uint8_t* payload, size_t len) {
    if (!isConnected()) {
        _lastCommandResult = CommandResult::NotConnected();
        return _lastCommandResult;
    }

    uint8_t tag = _resultQueue.nextTag();
    int sent = sendPacket(type, payload, len, tag);
    if (sent < 0) {
        _lastCommandResult = CommandResult::SendFailed();
        return _lastCommandResult;
    }

    if (!_blockingMode) {
        _lastCommandResult = CommandResult::Ack();
        return _lastCommandResult;
    }

    _lastCommandResult = _resultQueue.waitForTag(tag, [this]() { SerialBus::process(); });
    return _lastCommandResult;
}

// ============================================================================
// Configuration
// ============================================================================

void BusClient::setCompatibleVersions(const char** versions, size_t count) {
    _compatibleVersions = versions;
    _compatibleVersionCount = count;
}

// ============================================================================
// Packet Handling
// ============================================================================

void BusClient::handlePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) {
    switch (type) {
        case CorePacket::INIT_READY:
            _serverReady = true;
            if (len > 0) {
                char buffer[128];
                size_t bufLen = (len < sizeof(buffer) - 1) ? len : sizeof(buffer) - 1;
                memcpy(buffer, payload, bufLen);
                buffer[bufLen] = '\0';

                // Parse pipe-delimited: name|version|platform|cpuMHz|ramBytes
                char* name = strtok(buffer, "|");
                char* version = strtok(nullptr, "|");
                char* platform = strtok(nullptr, "|");
                char* cpuStr = strtok(nullptr, "|");
                char* ramStr = strtok(nullptr, "|");

                if (name) {
                    strncpy(_serverName, name, sizeof(_serverName) - 1);
                    _serverName[sizeof(_serverName) - 1] = '\0';
                    strncpy(_boardInfo.deviceName, name, sizeof(_boardInfo.deviceName) - 1);
                }
                if (version) {
                    strncpy(_boardInfo.firmwareVersion, version, sizeof(_boardInfo.firmwareVersion) - 1);
                    _boardInfo.versionCompatible = checkVersionCompatibility(version);
                }
                if (platform) strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
                if (cpuStr) _boardInfo.cpuFrequencyMHz = atoi(cpuStr);
                if (ramStr) _boardInfo.freeRamBytes = atoi(ramStr);
            }
            onServerReady();
            if (_readyCallback) _readyCallback(_serverName);
            break;

        case CorePacket::ACK:
            _lastCommandResult = CommandResult::Ack();
            _resultQueue.resolve(tag, _lastCommandResult);
            break;

        case CorePacket::NACK: {
            uint8_t errorCode = (len >= 1) ? payload[0] : SerialError::UNKNOWN;
            char reason[64] = "";
            if (len > 1) {
                size_t msgLen = (len - 1 < sizeof(reason) - 1) ? len - 1 : sizeof(reason) - 1;
                memcpy(reason, &payload[1], msgLen);
                reason[msgLen] = '\0';
            }
            _lastCommandResult = CommandResult::Nack(errorCode,
                reason[0] ? reason : getModuleErrorMessage(errorCode));
            _resultQueue.resolve(tag, _lastCommandResult);
            break;
        }

        case CorePacket::ERROR:
            if (_errorCallback && len >= 1) {
                uint8_t errorCode = payload[0];
                char message[64] = "";
                if (len > 1) {
                    size_t msgLen = (len - 1 < sizeof(message) - 1) ? len - 1 : sizeof(message) - 1;
                    memcpy(message, &payload[1], msgLen);
                    message[msgLen] = '\0';
                }
                _errorCallback(errorCode, message);
            }
            break;

        case CorePacket::LOG_MESSAGE:
            // Relay slave log messages — wire format: [level:u8][millis:u32LE][message:str]
            if (_logCallback && len >= 6) {
                uint8_t level = payload[0];
                uint32_t timestamp_ms = getU32LE(&payload[1]);
                char message[128] = "";
                size_t msgLen = len - 5;
                if (msgLen > sizeof(message) - 1) msgLen = sizeof(message) - 1;
                memcpy(message, &payload[5], msgLen);
                message[msgLen] = '\0';
                _logCallback(level, timestamp_ms, message);
            }
            break;

        default:
            // Delegate to module-specific handler
            onModulePacket(type, tag, payload, len);
            break;
    }
}

bool BusClient::checkVersionCompatibility(const char* version) {
    if (_compatibleVersions == nullptr || _compatibleVersionCount == 0) {
        return true;
    }
    for (size_t i = 0; i < _compatibleVersionCount; i++) {
        if (strcmp(version, _compatibleVersions[i]) == 0) {
            return true;
        }
    }
    return false;
}
