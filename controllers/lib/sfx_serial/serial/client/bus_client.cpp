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

bool BusClient::begin(int deviceIndex) {
    if (!SerialBus::begin(deviceIndex)) {
        return false;
    }

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

int BusClient::sendInit() {
    _serverReady = false;  // Reset — must wait for new INIT_READY
    return SerialBus::sendInit();  // Sends binary INIT packet via COBS
}

int BusClient::sendIdentify() {
    return SerialBus::sendIdentify();  // Non-destructive board info query
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
        case CorePacket::IDENTIFY: {
            // Both INIT_READY and IDENTIFY carry identical board info payload.
            // INIT_READY = server activated (init callbacks fired).
            // IDENTIFY   = non-destructive probe (no state change on server).
            _serverReady = true;
            if (len > 0) {
                CoreBoardInfo coreInfo;
                if (CorePayload::decodeInitReady(payload, len, coreInfo)) {
                    static_cast<CoreBoardInfo&>(_boardInfo) = coreInfo;
                    strncpy(_serverName, coreInfo.deviceName, sizeof(_serverName) - 1);
                    _serverName[sizeof(_serverName) - 1] = '\0';
                    _boardInfo.versionCompatible = checkVersionCompatibility(coreInfo.firmwareVersion);
                }
            }
            onServerReady();
            if (_readyCallback) _readyCallback(_serverName);
            break;
        }

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
            // Capture raw response for routing layers (e.g., STATUS forwarding)
            _lastResponseType = type;
            _lastResponseLen = (len <= RAW_RESPONSE_MAX) ? len : RAW_RESPONSE_MAX;
            if (_lastResponseLen > 0) {
                memcpy(_lastResponsePayload, payload, _lastResponseLen);
            }
            // Delegate to module-specific handler
            onModulePacket(type, tag, payload, len);
            // Fire async hook for unsolicited packets so routing layers can
            // forward slave-initiated packets upstream (HubFX re-emits
            // verbatim with TAG_ASYNC). Solicited responses are correlated
            // by tag in the result queue and would not reach here with
            // TAG_ASYNC.
            if (_asyncCallback && tag == CoreProtocol::TAG_ASYNC) {
                _asyncCallback(type, payload, len);
            }
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
