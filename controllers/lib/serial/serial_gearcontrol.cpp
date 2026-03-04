/*
 * Serial GearControl Protocol - Implementation
 *
 * Binary COBS protocol client/server for GearControl landing gear controller.
 *   - GearControlClient: For HubFX (sends commands via USB)
 *   - GearControlServer: For GearControl Pico (receives commands, implements ICommandHandler)
 */

#include "serial_gearcontrol.h"

using namespace CoreProtocol;

// ============================================================================
// GearControlClient Implementation
// ============================================================================

bool GearControlClient::begin(UsbHost* usbHost, int deviceIndex) {
    if (!SerialBus::begin(usbHost, deviceIndex)) {
        return false;
    }

    _usbHostRef = usbHost;
    _serverReady = false;
    _serverName[0] = '\0';
    memset(&_boardInfo, 0, sizeof(_boardInfo));
    memset(&_lastStatus, 0, sizeof(_lastStatus));

    SerialBus::onPacketReceived([this](uint8_t type, const uint8_t* payload, size_t len) {
        handlePacket(type, payload, len);
    });

    return true;
}

int GearControlClient::process() {
    return SerialBus::process();
}

int GearControlClient::sendInit(unsigned long keepaliveMs) {
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

void GearControlClient::setCompatibleVersions(const char** versions, size_t count) {
    _compatibleVersions = versions;
    _compatibleVersionCount = count;
}

bool GearControlClient::checkVersionCompatibility(const char* version) {
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

void GearControlClient::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case CorePacket::INIT_READY:
            _serverReady = true;
            if (len > 0) {
                char buffer[128];
                size_t bufLen = (len < sizeof(buffer) - 1) ? len : sizeof(buffer) - 1;
                memcpy(buffer, payload, bufLen);
                buffer[bufLen] = '\0';

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
            if (_readyCallback) _readyCallback(_serverName);
            break;

        case CorePacket::STATUS:
            // Format: [counter:u32][uptime:u32][freeRam:u32][moduleData:32]
            // Module data per gear (3 × 9 = 27 bytes):
            //   [state:u8][motorCurrent_mA:u16][door0:u16][door1:u16][calibratedStall_mA:u16]
            // Trailing (5 bytes): [yawPos:u16][ledFlags:u8][batteryVoltage_mV:u16]
            if (len >= 44) {  // 12 core + 32 module
                const uint8_t* mod = &payload[12];
                for (int i = 0; i < 3; i++) {
                    size_t off = i * 9;
                    _lastStatus.gear[i].state = static_cast<GearState>(mod[off]);
                    _lastStatus.gear[i].motorCurrent_mA = getU16LE(&mod[off + 1]);
                    _lastStatus.gear[i].door0Pos_us = getU16LE(&mod[off + 3]);
                    _lastStatus.gear[i].door1Pos_us = getU16LE(&mod[off + 5]);
                    _lastStatus.gear[i].calibratedStall_mA = getU16LE(&mod[off + 7]);
                }
                _lastStatus.yawPos_us = getU16LE(&mod[27]);
                _lastStatus.ledFlags = mod[29];
                _lastStatus.batteryVoltage_mV = getU16LE(&mod[30]);

                if (_pendingAckNack) {
                    _receivedAck = true;
                    _pendingAckNack = false;
                    _lastCommandResult = CommandResult::Ack();
                }
                if (_statusCallback) _statusCallback(_lastStatus);
            } else if (len >= 12) {
                if (_pendingAckNack) {
                    _receivedAck = true;
                    _pendingAckNack = false;
                    _lastCommandResult = CommandResult::Ack();
                }
                if (_statusCallback) _statusCallback(_lastStatus);
            }
            break;

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

        case CorePacket::ACK:
            _receivedAck = true;
            _pendingAckNack = false;
            _lastCommandResult = CommandResult::Ack();
            break;

        case CorePacket::NACK:
            _receivedNack = true;
            _pendingAckNack = false;
            {
                uint8_t errorCode = (len >= 1) ? payload[0] : SerialError::UNKNOWN;
                char reason[64] = "";
                if (len > 1) {
                    size_t msgLen = (len - 1 < sizeof(reason) - 1) ? len - 1 : sizeof(reason) - 1;
                    memcpy(reason, &payload[1], msgLen);
                    reason[msgLen] = '\0';
                }
                _lastNackErrorCode = errorCode;
                strncpy(_lastNackReason, reason[0] ? reason : GearControlError::getMessage(errorCode),
                        sizeof(_lastNackReason) - 1);
                _lastCommandResult = CommandResult::Nack(errorCode, _lastNackReason);
            }
            break;

        case GearControlPacket::GEAR_CALIB_STATUS:
            // Unsolicited calibration progress: [gear_id:u8][phase:u8][current:u16LE][peak:u16LE][stall:u16LE]
            if (len >= 8 && _calibStatusCallback) {
                GearControlCalibStatus cs;
                cs.gearId = payload[0];
                cs.phase = static_cast<CalibPhase>(payload[1]);
                cs.current_mA = getU16LE(&payload[2]);
                cs.peak_mA = getU16LE(&payload[4]);
                cs.calibratedStall_mA = getU16LE(&payload[6]);
                _calibStatusCallback(cs);
            }
            break;

        default:
            break;
    }
}

CommandResult GearControlClient::sendPacketBlocking(uint8_t type, const uint8_t* payload, size_t len) {
    if (!isConnected()) {
        _lastCommandResult = CommandResult::NotConnected();
        return _lastCommandResult;
    }

    _pendingAckNack = true;
    _receivedAck = false;
    _receivedNack = false;
    _lastNackErrorCode = 0;
    _lastNackReason[0] = '\0';

    int sent = sendPacket(type, payload, len);
    if (sent < 0) {
        _pendingAckNack = false;
        _lastCommandResult = CommandResult::SendFailed();
        return _lastCommandResult;
    }

    if (!_blockingMode) {
        _lastCommandResult = CommandResult::Ack();
        return _lastCommandResult;
    }

    return waitForAckNack();
}

CommandResult GearControlClient::waitForAckNack() {
    unsigned long startMs = millis();

    while (_pendingAckNack) {
        SerialBus::process();

        if (millis() - startMs > _commandTimeoutMs) {
            _pendingAckNack = false;
            _lastCommandResult = CommandResult::Timeout();
            return _lastCommandResult;
        }

        delay(1);
    }

    return _lastCommandResult;
}

// ============================================================================
// GearControlClient - Gear Control
// ============================================================================

CommandResult GearControlClient::gearDeploy(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendPacketBlocking(GearControlPacket::GEAR_DEPLOY, payload, sizeof(payload));
}

CommandResult GearControlClient::gearRetract(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendPacketBlocking(GearControlPacket::GEAR_RETRACT, payload, sizeof(payload));
}

CommandResult GearControlClient::gearStop(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendPacketBlocking(GearControlPacket::GEAR_STOP, payload, sizeof(payload));
}

CommandResult GearControlClient::gearAll(uint8_t action) {
    uint8_t payload[1] = { action };
    return sendPacketBlocking(GearControlPacket::GEAR_ALL, payload, sizeof(payload));
}

CommandResult GearControlClient::gearCalibrate(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendPacketBlocking(GearControlPacket::GEAR_CALIBRATE, payload, sizeof(payload));
}

CommandResult GearControlClient::gearCalibCancel(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendPacketBlocking(GearControlPacket::GEAR_CALIB_CANCEL, payload, sizeof(payload));
}

// ============================================================================
// GearControlClient - Servo Control
// ============================================================================

CommandResult GearControlClient::setServoPosition(uint8_t servoId, uint16_t pulse_us) {
    uint8_t payload[3];
    payload[0] = servoId;
    putU16LE(&payload[1], pulse_us);
    return sendPacketBlocking(GearControlPacket::SERVO_SET, payload, sizeof(payload));
}

CommandResult GearControlClient::setServoSettings(const GearControlServoConfig& config) {
    uint8_t payload[11];
    payload[0] = config.servoId;
    putU16LE(&payload[1], config.minUs);                // min_us
    putU16LE(&payload[3], config.maxUs);                // max_us
    putU16LE(&payload[5], config.maxSpeedUsPerSec);     // speed  // µs/s
    putU16LE(&payload[7], config.maxAccelUsPerSec2);    // accel  // µs/s²
    putU16LE(&payload[9], config.maxDecelUsPerSec2);    // decel  // µs/s²
    return sendPacketBlocking(GearControlPacket::SRV_SETTINGS, payload, sizeof(payload));
}

// ============================================================================
// GearControlClient - Configuration
// ============================================================================

CommandResult GearControlClient::setGearConfig(const GearControlGearConfig& config) {
    uint8_t payload[6];
    payload[0] = config.gearId;
    payload[1] = config.flags;
    putU16LE(&payload[2], config.stallCurrent_mA);
    putU16LE(&payload[4], config.timeout_ms);
    return sendPacketBlocking(GearControlPacket::GEAR_CONFIG, payload, sizeof(payload));
}

CommandResult GearControlClient::setDoorConfig(const GearControlDoorConfig& config) {
    uint8_t payload[9];
    payload[0] = config.gearId;
    putU16LE(&payload[1], config.open0_us);
    putU16LE(&payload[3], config.close0_us);
    putU16LE(&payload[5], config.open1_us);
    putU16LE(&payload[7], config.close1_us);
    return sendPacketBlocking(GearControlPacket::DOOR_CONFIG, payload, sizeof(payload));
}

CommandResult GearControlClient::setYawConfig(const GearControlYawConfig& config) {
    uint8_t payload[7];
    payload[0] = config.gearId;
    putU16LE(&payload[1], config.neutral_us);
    putU16LE(&payload[3], config.min_us);
    putU16LE(&payload[5], config.max_us);
    return sendPacketBlocking(GearControlPacket::YAW_CONFIG, payload, sizeof(payload));
}

CommandResult GearControlClient::setYawInput(uint16_t position_us) {
    uint8_t payload[2];
    putU16LE(payload, position_us);
    return sendPacketBlocking(GearControlPacket::YAW_INPUT, payload, sizeof(payload));
}

CommandResult GearControlClient::setBatteryConfig(bool autoDeployOnLowVoltage) {
    uint8_t payload[1];
    payload[0] = autoDeployOnLowVoltage ? 1 : 0;
    return sendPacketBlocking(GearControlPacket::BATTERY_CONFIG, payload, sizeof(payload));
}

CommandResult GearControlClient::setDoorMode(const GearControlDoorModeConfig& config) {
    uint8_t payload[4];
    payload[0] = config.gearId;
    payload[1] = config.mode;
    putU16LE(&payload[2], config.delay_ms);
    return sendPacketBlocking(GearControlPacket::DOOR_MODE, payload, sizeof(payload));
}

// ============================================================================
// GearControlClient - Status
// ============================================================================

CommandResult GearControlClient::requestStatus() {
    return sendPacketBlocking(CorePacket::STATUS_REQ, nullptr, 0);
}

// ============================================================================
// GearControlServer Implementation
// ============================================================================

bool GearControlServer::begin(Stream* serial, const char* moduleName) {
    if (!serial) return false;

    _serial = serial;
    _rxIndex = 0;
    _clientConnected = false;
    _lastRxTimeMs = 0;

    strncpy(_moduleName, moduleName, sizeof(_moduleName) - 1);
    _moduleName[sizeof(_moduleName) - 1] = '\0';

    _initialized = true;
    return true;
}

void GearControlServer::end() {
    _initialized = false;
    _serial = nullptr;
    _clientConnected = false;
}

int GearControlServer::process() {
    if (!_initialized || !_serial) return 0;

    int packetsProcessed = 0;

    while (_serial->available()) {
        uint8_t byte = _serial->read();

        if (byte == FRAME_DELIMITER) {
            if (_rxIndex > 0) {
                processFrame(_rxBuffer, _rxIndex);
                packetsProcessed++;
                _rxIndex = 0;
            }
        } else {
            if (_rxIndex < sizeof(_rxBuffer)) {
                _rxBuffer[_rxIndex++] = byte;
            } else {
                _rxIndex = 0;
            }
        }
    }

    if (_connectionTimeoutMs > 0 && _clientConnected) {
        unsigned long now = millis();
        if (now - _lastRxTimeMs > _connectionTimeoutMs) {
            _clientConnected = false;
        }
    }

    return packetsProcessed;
}

void GearControlServer::processFrame(const uint8_t* frame, size_t frameLen) {
    uint8_t decoded[MAX_PACKET_SIZE];
    size_t decodedLen = cobsDecode(frame, frameLen, decoded, sizeof(decoded));

    if (decodedLen == 0) return;

    uint8_t type;
    const uint8_t* payload;
    size_t payloadLen;

    if (!parsePacket(decoded, decodedLen, &type, &payload, &payloadLen)) {
        return;
    }

    _lastRxTimeMs = millis();
    _clientConnected = true;

    handlePacket(type, payload, payloadLen);
}

CommandHandleResult GearControlServer::tryProcess(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_initialized || !_serial) return CommandHandleResult::NotMyCommand;

    // Check if packet type is in GearControl range (0x60-0x7F)
    if (type >= 0x60 && type <= 0x7F) {
        _lastRxTimeMs = millis();
        _clientConnected = true;
        return handlePacket(type, payload, len);
    }

    return CommandHandleResult::NotMyCommand;
}

CommandHandleResult GearControlServer::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case GearControlPacket::GEAR_DEPLOY: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearDeployCallback, gearId);
        }

        case GearControlPacket::GEAR_RETRACT: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearRetractCallback, gearId);
        }

        case GearControlPacket::GEAR_STOP: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearStopCallback, gearId);
        }

        case GearControlPacket::GEAR_ALL: {
            SFX_REQUIRE_LEN(1);
            uint8_t action = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidAction(action), GearControlError::INVALID_ACTION);
            SFX_DISPATCH(_gearAllCallback, action);
        }

        case GearControlPacket::SERVO_SET: {
            SFX_REQUIRE_LEN(3);
            uint8_t servoId = payload[0];
            uint16_t pulse_us = getU16LE(&payload[1]);
            SFX_VALIDATE(GearControlSpec::isValidServoId(servoId), GearControlError::INVALID_SERVO_ID);
            SFX_VALIDATE(GearControlSpec::isValidServoPulse(pulse_us), GearControlError::SERVO_OUT_OF_RANGE);
            SFX_DISPATCH(_servoSetCallback, servoId, pulse_us);
        }

        case GearControlPacket::SRV_SETTINGS: {
            SFX_REQUIRE_LEN(11);
            GearControlServoConfig config;
            config.servoId = payload[0];
            config.minUs = getU16LE(&payload[1]);              // min_us             // µs
            config.maxUs = getU16LE(&payload[3]);              // max_us             // µs
            config.maxSpeedUsPerSec = getU16LE(&payload[5]);   // speed              // µs/s
            config.maxAccelUsPerSec2 = getU16LE(&payload[7]);  // accel              // µs/s²
            config.maxDecelUsPerSec2 = getU16LE(&payload[9]);  // decel              // µs/s²
            SFX_VALIDATE(GearControlSpec::isValidServoId(config.servoId), GearControlError::INVALID_SERVO_ID);
            SFX_VALIDATE(GearControlSpec::isValidServoPulse(config.minUs) &&
                         GearControlSpec::isValidServoPulse(config.maxUs), GearControlError::SERVO_OUT_OF_RANGE);
            SFX_VALIDATE(config.minUs < config.maxUs, GearControlError::SERVO_OUT_OF_RANGE);
            SFX_DISPATCH(_servoSettingsCallback, config);
        }

        case GearControlPacket::GEAR_CONFIG: {
            SFX_REQUIRE_LEN(6);
            GearControlGearConfig config;
            config.gearId = payload[0];
            config.flags = payload[1];
            config.stallCurrent_mA = getU16LE(&payload[2]);
            config.timeout_ms = getU16LE(&payload[4]);
            SFX_VALIDATE(GearControlSpec::isValidGearId(config.gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearConfigCallback, config);
        }

        case GearControlPacket::DOOR_CONFIG: {
            SFX_REQUIRE_LEN(9);
            GearControlDoorConfig config;
            config.gearId = payload[0];
            config.open0_us = getU16LE(&payload[1]);
            config.close0_us = getU16LE(&payload[3]);
            config.open1_us = getU16LE(&payload[5]);
            config.close1_us = getU16LE(&payload[7]);
            SFX_VALIDATE(GearControlSpec::isValidGearId(config.gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_doorConfigCallback, config);
        }

        case GearControlPacket::YAW_CONFIG: {
            SFX_REQUIRE_LEN(7);
            GearControlYawConfig config;
            config.gearId = payload[0];
            config.neutral_us = getU16LE(&payload[1]);
            config.min_us = getU16LE(&payload[3]);
            config.max_us = getU16LE(&payload[5]);
            SFX_VALIDATE(GearControlSpec::isValidGearId(config.gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_yawConfigCallback, config);
        }

        case GearControlPacket::YAW_INPUT: {
            SFX_REQUIRE_LEN(2);
            uint16_t position_us = getU16LE(payload);
            SFX_DISPATCH(_yawInputCallback, position_us);
        }

        case GearControlPacket::GEAR_CALIBRATE: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearCalibrateCallback, gearId);
        }

        case GearControlPacket::GEAR_CALIB_CANCEL: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearCalibCancelCallback, gearId);
        }

        case GearControlPacket::BATTERY_CONFIG: {
            SFX_REQUIRE_LEN(1);
            bool autoDeploy = payload[0] != 0;
            SFX_DISPATCH(_batteryConfigCallback, autoDeploy);
        }

        case GearControlPacket::DOOR_MODE: {
            SFX_REQUIRE_LEN(4);
            GearControlDoorModeConfig config;
            config.gearId = payload[0];
            config.mode = payload[1];
            config.delay_ms = getU16LE(&payload[2]);
            SFX_VALIDATE(GearControlSpec::isValidGearId(config.gearId), GearControlError::INVALID_GEAR_ID);
            SFX_VALIDATE(GearControlSpec::isValidDoorMode(config.mode), GearControlError::INVALID_ACTION);
            SFX_DISPATCH(_doorModeCallback, config);
        }

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// GearControlServer - Response Methods
// ============================================================================

int GearControlServer::sendRawPacket(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_serial) return -1;

    uint8_t buffer[COBS_BUFFER_SIZE];
    size_t encodedLen = encodePacket(buffer, type, payload, len);

    if (encodedLen == 0) return -1;

    size_t written = _serial->write(buffer, encodedLen);
    _serial->write(FRAME_DELIMITER);

    return (int)written;
}

int GearControlServer::sendAck() {
    return sendRawPacket(CorePacket::ACK, nullptr, 0);
}

int GearControlServer::sendNack(uint8_t errorCode, const char* reason) {
    uint8_t payload[64];
    payload[0] = errorCode;

    const char* msg = (reason && reason[0]) ? reason : GearControlError::getMessage(errorCode);
    size_t msgLen = strlen(msg);
    if (msgLen > sizeof(payload) - 1) {
        msgLen = sizeof(payload) - 1;
    }
    memcpy(&payload[1], msg, msgLen);

    return sendRawPacket(CorePacket::NACK, payload, 1 + msgLen);
}

int GearControlServer::sendError(uint8_t errorCode, const char* message) {
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

    return sendRawPacket(CorePacket::ERROR, payload, len);
}

int GearControlServer::sendCalibStatus(const GearControlCalibStatus& status) {
    uint8_t payload[8];
    payload[0] = status.gearId;
    payload[1] = static_cast<uint8_t>(status.phase);
    putU16LE(&payload[2], status.current_mA);          // mA
    putU16LE(&payload[4], status.peak_mA);              // mA
    putU16LE(&payload[6], status.calibratedStall_mA);   // mA
    return sendRawPacket(GearControlPacket::GEAR_CALIB_STATUS, payload, sizeof(payload));
}
