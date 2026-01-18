/*
 * Serial GunFX Protocol - Implementation
 *
 * Text and binary protocol master/slave for GunFX muzzle flash controller.
 */

#include "serial_gunfx.h"
#include "serial_init.h"

// ============================================================================
// GunFxSerialMaster Implementation (Text Protocol)
// ============================================================================

bool GunFxSerialMaster::begin(Stream* serial) {
    if (!serial) return false;

    _serial = serial;
    _rxIndex = 0;
    _slaveReady = false;
    _slaveName[0] = '\0';
    _connected = false;
    _lastRxTimeMs = 0;
    
    memset(&_boardInfo, 0, sizeof(_boardInfo));
    memset(&_lastStatus, 0, sizeof(_lastStatus));

    _initialized = true;
    return true;
}

void GunFxSerialMaster::end() {
    _initialized = false;
    _serial = nullptr;
    _slaveReady = false;
    _connected = false;
}

int GunFxSerialMaster::process() {
    if (!_initialized || !_serial) return 0;

    int linesProcessed = 0;

    while (_serial->available()) {
        char c = _serial->read();
        
        if (c == '\n' || c == '\r') {
            if (_rxIndex > 0) {
                _rxBuffer[_rxIndex] = '\0';
                processLine(_rxBuffer);
                linesProcessed++;
                _rxIndex = 0;
            }
        } else if (_rxIndex < sizeof(_rxBuffer) - 1) {
            _rxBuffer[_rxIndex++] = c;
        }
    }

    // Check connection timeout
    if (_connectionTimeoutMs > 0 && _connected) {
        unsigned long now = millis();
        if (now - _lastRxTimeMs > _connectionTimeoutMs) {
            _connected = false;
            _slaveReady = false;
        }
    }

    return linesProcessed;
}

void GunFxSerialMaster::processLine(const char* line) {
    _lastRxTimeMs = millis();
    _connected = true;

    char command[32];
    const char* args = nullptr;
    
    const char* space = strchr(line, ' ');
    if (space) {
        size_t cmdLen = space - line;
        if (cmdLen >= sizeof(command)) cmdLen = sizeof(command) - 1;
        strncpy(command, line, cmdLen);
        command[cmdLen] = '\0';
        args = space + 1;
    } else {
        strncpy(command, line, sizeof(command) - 1);
        command[sizeof(command) - 1] = '\0';
    }

    if (strcmp(command, "INIT_READY") == 0) {
        handleInitReady(args);
    } else if (strcmp(command, "STATUS") == 0) {
        handleStatus(args);
    } else if (strcmp(command, "ERROR") == 0) {
        handleError(args);
    } else if (strcmp(command, "ACK") == 0) {
        handleAck();
    } else if (strcmp(command, "NACK") == 0) {
        handleNack(args);
    }
}

void GunFxSerialMaster::handleInitReady(const char* args) {
    _slaveReady = true;
    
    if (args) {
        char* argsCopy = strdup(args);
        if (argsCopy) {
            char name[32] = "";
            char version[16] = "";
            char platform[16] = "";
            int cpuMHz = 0;
            int ramBytes = 0;
            
            TextParse::getString(argsCopy, "name", name, sizeof(name));
            TextParse::getString(argsCopy, "version", version, sizeof(version));
            TextParse::getString(argsCopy, "platform", platform, sizeof(platform));
            cpuMHz = TextParse::getInt(argsCopy, "cpuMHz", 0);
            ramBytes = TextParse::getInt(argsCopy, "ramBytes", 0);
            
            strncpy(_slaveName, name, sizeof(_slaveName) - 1);
            _slaveName[sizeof(_slaveName) - 1] = '\0';
            
            strncpy(_boardInfo.deviceName, name, sizeof(_boardInfo.deviceName) - 1);
            strncpy(_boardInfo.firmwareVersion, version, sizeof(_boardInfo.firmwareVersion) - 1);
            strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
            _boardInfo.cpuFrequencyMHz = cpuMHz;
            _boardInfo.freeRamBytes = ramBytes;
            _boardInfo.versionCompatible = checkVersionCompatibility(version);
            
            free(argsCopy);
        }
    }
    
    if (_readyCallback) {
        _readyCallback(_slaveName);
    }
}

void GunFxSerialMaster::handleStatus(const char* args) {
    if (!args) return;
    
    char* argsCopy = strdup(args);
    if (!argsCopy) return;
    
    _lastStatus.firing = TextParse::getInt(argsCopy, "firing", 0) != 0;
    _lastStatus.flashActive = TextParse::getInt(argsCopy, "flashActive", 0) != 0;
    _lastStatus.flashFading = TextParse::getInt(argsCopy, "flashFading", 0) != 0;
    _lastStatus.heaterOn = TextParse::getInt(argsCopy, "heaterOn", 0) != 0;
    _lastStatus.fanOn = TextParse::getInt(argsCopy, "fanOn", 0) != 0;
    _lastStatus.fanSpindown = TextParse::getInt(argsCopy, "fanSpindown", 0) != 0;
    _lastStatus.fanSpeed = (uint8_t)TextParse::getInt(argsCopy, "fanSpeed", 0);
    _lastStatus.fanOffRemainingMs = TextParse::getInt(argsCopy, "fanOffRemainingMs", 0);
    _lastStatus.servoUs[0] = TextParse::getInt(argsCopy, "servo0", 0);
    _lastStatus.servoUs[1] = TextParse::getInt(argsCopy, "servo1", 0);
    _lastStatus.servoUs[2] = TextParse::getInt(argsCopy, "servo2", 0);
    _lastStatus.rateOfFireRpm = TextParse::getInt(argsCopy, "rpm", 0);
    _lastStatus.shotsFired = (uint32_t)TextParse::getLong(argsCopy, "shotsFired", 0);
    _lastStatus.heaterOnTimeMs = (uint32_t)TextParse::getLong(argsCopy, "heaterOnTimeMs", 0);
    _lastStatus.uptimeMs = (uint32_t)TextParse::getLong(argsCopy, "uptimeMs", 0);
    _lastStatus.freeRam = (uint32_t)TextParse::getLong(argsCopy, "freeRam", 0);
    
    free(argsCopy);
    
    if (_pendingAckNack) {
        _receivedAck = true;
        _pendingAckNack = false;
        _lastCommandResult = CommandResult::Ack();
    }
    
    if (_statusCallback) {
        _statusCallback(_lastStatus);
    }
}

void GunFxSerialMaster::handleError(const char* args) {
    if (!_errorCallback) return;
    
    int code = 0;
    char message[64] = "";
    
    if (args) {
        char* argsCopy = strdup(args);
        if (argsCopy) {
            code = TextParse::getInt(argsCopy, "code", 0);
            TextParse::getString(argsCopy, "msg", message, sizeof(message));
            free(argsCopy);
        }
    }
    
    _errorCallback(code, message);
}

void GunFxSerialMaster::handleAck() {
    _receivedAck = true;
    _pendingAckNack = false;
    _lastCommandResult = CommandResult::Ack();
}

void GunFxSerialMaster::handleNack(const char* args) {
    _receivedNack = true;
    _pendingAckNack = false;
    
    int code = SerialError::UNKNOWN;
    char reason[64] = "";
    
    if (args) {
        char* argsCopy = strdup(args);
        if (argsCopy) {
            code = TextParse::getInt(argsCopy, "code", SerialError::UNKNOWN);
            TextParse::getString(argsCopy, "reason", reason, sizeof(reason));
            free(argsCopy);
        }
    }
    
    _lastNackErrorCode = code;
    strncpy(_lastNackReason, reason[0] ? reason : GunFxError::getMessage(code), sizeof(_lastNackReason) - 1);
    _lastCommandResult = CommandResult::Nack(code, _lastNackReason);
}

bool GunFxSerialMaster::checkVersionCompatibility(const char* version) {
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

int GunFxSerialMaster::sendCommand(const char* command) {
    if (!_initialized || !_serial) return -1;
    
    size_t len = _serial->print(command);
    _serial->print("\n");
    return len;
}

CommandResult GunFxSerialMaster::sendCommandBlocking(const char* command) {
    if (!_initialized || !_serial) {
        _lastCommandResult = CommandResult::NotConnected();
        return _lastCommandResult;
    }
    
    _pendingAckNack = true;
    _receivedAck = false;
    _receivedNack = false;
    _lastNackErrorCode = 0;
    _lastNackReason[0] = '\0';
    
    int sent = sendCommand(command);
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

CommandResult GunFxSerialMaster::waitForAckNack() {
    unsigned long startMs = millis();
    
    while (_pendingAckNack) {
        process();
        
        if (millis() - startMs > _commandTimeoutMs) {
            _pendingAckNack = false;
            _lastCommandResult = CommandResult::Timeout();
            return _lastCommandResult;
        }
        
        delay(1);
    }
    
    return _lastCommandResult;
}

int GunFxSerialMaster::sendInit() {
    return sendCommand("INIT");
}

int GunFxSerialMaster::sendShutdown() {
    return sendCommand("SHUTDOWN");
}

int GunFxSerialMaster::sendReboot() {
    return sendCommand("REBOOT");
}

int GunFxSerialMaster::sendBootsel() {
    return sendCommand("BOOTSEL");
}

int GunFxSerialMaster::sendKeepalive() {
    return sendCommand("KEEPALIVE");
}

CommandResult GunFxSerialMaster::triggerOn(uint16_t rpm) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s rpm=%u", GunFxCmd::TRIGGER_ON, rpm);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMaster::triggerOff(uint16_t fanDelayMs) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s fanDelayMs=%u", GunFxCmd::TRIGGER_OFF, fanDelayMs);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMaster::setServoPosition(uint8_t servoId, uint16_t pulseUs) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s id=%u pulseUs=%u", GunFxCmd::SERVO_SET, servoId, pulseUs);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMaster::setServoConfig(const GunFxServoConfig& config) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s id=%u minUs=%u maxUs=%u maxSpeedUsPerSec=%u maxAccelUsPerSec2=%u maxDecelUsPerSec2=%u",
             GunFxCmd::SERVO_CONFIG,
             config.servoId,
             config.minUs,
             config.maxUs,
             config.maxSpeedUsPerSec,
             config.maxAccelUsPerSec2,
             config.maxDecelUsPerSec2);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMaster::setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s id=%u jerkUs=%u varianceUs=%u",
             GunFxCmd::SERVO_RECOIL_JERK, servoId, jerkUs, varianceUs);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMaster::setSmokeHeater(bool on) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s on=%d", GunFxCmd::SMOKE_HEAT, on ? 1 : 0);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMaster::setSmokeSettings(const GunFxSmokeConfig& config) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s pulsing=%d speed=%u pulseHigh=%u pulseLow=%u pulseMs=%u spindownMs=%u",
             GunFxCmd::SMOKE_SETTINGS,
             config.fanPulsing ? 1 : 0,
             config.fanSpeed,
             config.fanPulseHigh,
             config.fanPulseLow,
             config.fanPulseMs,
             config.fanSpindownMs);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMaster::requestStatus() {
    return sendCommandBlocking("STATUS_REQ");
}

// ============================================================================
// GunFxSerialSlave Implementation (Text Protocol)
// ============================================================================

bool GunFxSerialSlave::begin(Stream* serial, const char* moduleName) {
    if (!serial) return false;

    _serial = serial;
    _rxIndex = 0;
    _masterConnected = false;
    _lastRxTimeMs = 0;

    strncpy(_moduleName, moduleName, sizeof(_moduleName) - 1);
    _moduleName[sizeof(_moduleName) - 1] = '\0';

    _initialized = true;
    return true;
}

void GunFxSerialSlave::end() {
    _initialized = false;
    _serial = nullptr;
    _masterConnected = false;
}

int GunFxSerialSlave::process() {
    if (!_initialized || !_serial) return 0;

    int linesProcessed = 0;

    while (_serial->available()) {
        char c = _serial->read();
        
        if (c == '\n' || c == '\r') {
            if (_rxIndex > 0) {
                _rxBuffer[_rxIndex] = '\0';
                processLine(_rxBuffer);
                linesProcessed++;
                _rxIndex = 0;
            }
        } else if (_rxIndex < sizeof(_rxBuffer) - 1) {
            _rxBuffer[_rxIndex++] = c;
        }
    }

    if (_connectionTimeoutMs > 0 && _masterConnected) {
        unsigned long now = millis();
        if (now - _lastRxTimeMs > _connectionTimeoutMs) {
            _masterConnected = false;
        }
    }

    return linesProcessed;
}

void GunFxSerialSlave::processLine(const char* line) {
    tryProcessCommand(line);
}

CommandHandleResult GunFxSerialSlave::tryProcessCommand(const char* line) {
    if (!line || !_serial) return CommandHandleResult::NotMyCommand;

    _lastRxTimeMs = millis();
    _masterConnected = true;

    char command[32];
    const char* args = nullptr;
    
    const char* space = strchr(line, ' ');
    if (space) {
        size_t cmdLen = space - line;
        if (cmdLen >= sizeof(command)) cmdLen = sizeof(command) - 1;
        strncpy(command, line, cmdLen);
        command[cmdLen] = '\0';
        args = space + 1;
    } else {
        strncpy(command, line, sizeof(command) - 1);
        command[sizeof(command) - 1] = '\0';
    }

    if (strcmp(command, GunFxCmd::TRIGGER_ON) == 0) {
        handleTriggerOn(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, GunFxCmd::TRIGGER_OFF) == 0) {
        handleTriggerOff(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, GunFxCmd::SERVO_SET) == 0) {
        handleServoSet(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, GunFxCmd::SERVO_CONFIG) == 0) {
        handleServoConfig(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, GunFxCmd::SERVO_RECOIL_JERK) == 0) {
        handleRecoilJerk(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, GunFxCmd::SMOKE_HEAT) == 0) {
        handleSmokeHeat(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, GunFxCmd::SMOKE_SETTINGS) == 0) {
        handleSmokeSettings(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, "STATUS_REQ") == 0) {
        if (_statusRequestCallback) {
            GunFxStatus status = _statusRequestCallback();
            sendStatus(status);
        } else {
            sendStatus(GunFxStatus{});
        }
        return CommandHandleResult::Handled;
    }
    
    return CommandHandleResult::NotMyCommand;
}

void GunFxSerialSlave::handleTriggerOn(const char* args) {
    int rpm = 0;
    if (args) {
        char* argsCopy = strdup(args);
        if (argsCopy) {
            rpm = TextParse::getInt(argsCopy, "rpm", 0);
            free(argsCopy);
        }
    }
    
    if (_triggerOnCallback) {
        uint8_t result = _triggerOnCallback(rpm);
        if (result == SerialError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void GunFxSerialSlave::handleTriggerOff(const char* args) {
    int fanDelayMs = 0;
    if (args) {
        char* argsCopy = strdup(args);
        if (argsCopy) {
            fanDelayMs = TextParse::getInt(argsCopy, "fanDelayMs", 0);
            free(argsCopy);
        }
    }
    
    if (_triggerOffCallback) {
        uint8_t result = _triggerOffCallback(fanDelayMs);
        if (result == SerialError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void GunFxSerialSlave::handleServoSet(const char* args) {
    if (!args) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    
    char* argsCopy = strdup(args);
    if (!argsCopy) {
        sendNack(SerialError::INTERNAL_ERROR);
        return;
    }
    
    int id = TextParse::getInt(argsCopy, "id", 0);
    int pulseUs = TextParse::getInt(argsCopy, "pulseUs", 0);
    free(argsCopy);
    
    if (_servoSetCallback) {
        uint8_t result = _servoSetCallback(id, pulseUs);
        if (result == SerialError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void GunFxSerialSlave::handleServoConfig(const char* args) {
    if (!args) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    
    char* argsCopy = strdup(args);
    if (!argsCopy) {
        sendNack(SerialError::INTERNAL_ERROR);
        return;
    }
    
    GunFxServoConfig config = {};
    config.servoId = TextParse::getInt(argsCopy, "id", 0);
    config.minUs = TextParse::getInt(argsCopy, "minUs", 0);
    config.maxUs = TextParse::getInt(argsCopy, "maxUs", 0);
    config.maxSpeedUsPerSec = TextParse::getInt(argsCopy, "maxSpeedUsPerSec", 0);
    config.maxAccelUsPerSec2 = TextParse::getInt(argsCopy, "maxAccelUsPerSec2", 0);
    config.maxDecelUsPerSec2 = TextParse::getInt(argsCopy, "maxDecelUsPerSec2", 0);
    
    free(argsCopy);
    
    if (_servoSettingsCallback) {
        uint8_t result = _servoSettingsCallback(config);
        if (result == SerialError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void GunFxSerialSlave::handleRecoilJerk(const char* args) {
    if (!args) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    
    char* argsCopy = strdup(args);
    if (!argsCopy) {
        sendNack(SerialError::INTERNAL_ERROR);
        return;
    }
    
    GunFxServoConfig config = {};
    config.servoId = TextParse::getInt(argsCopy, "id", 0);
    config.recoilJerkUs = TextParse::getInt(argsCopy, "jerkUs", 0);
    config.recoilJerkVarianceUs = TextParse::getInt(argsCopy, "varianceUs", 0);
    
    free(argsCopy);
    
    if (_servoSettingsCallback) {
        uint8_t result = _servoSettingsCallback(config);
        if (result == SerialError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void GunFxSerialSlave::handleSmokeHeat(const char* args) {
    int on = 0;
    if (args) {
        char* argsCopy = strdup(args);
        if (argsCopy) {
            on = TextParse::getInt(argsCopy, "on", 0);
            free(argsCopy);
        }
    }
    
    if (_smokeHeatCallback) {
        uint8_t result = _smokeHeatCallback(on != 0);
        if (result == SerialError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void GunFxSerialSlave::handleSmokeSettings(const char* args) {
    GunFxSmokeConfig config;
    
    if (args) {
        char* argsCopy = strdup(args);
        if (argsCopy) {
            config.fanPulsing = TextParse::getInt(argsCopy, "pulsing", 0) != 0;
            config.fanSpeed = (uint8_t)constrain(TextParse::getInt(argsCopy, "speed", 255), 0, 255);
            config.fanPulseHigh = (uint8_t)constrain(TextParse::getInt(argsCopy, "pulseHigh", 255), 0, 255);
            config.fanPulseLow = (uint8_t)constrain(TextParse::getInt(argsCopy, "pulseLow", 80), 0, 255);
            config.fanPulseMs = (uint16_t)TextParse::getInt(argsCopy, "pulseMs", 50);
            config.fanSpindownMs = (uint16_t)TextParse::getInt(argsCopy, "spindownMs", 5000);
            free(argsCopy);
        }
    }
    
    if (_smokeSettingsCallback) {
        uint8_t result = _smokeSettingsCallback(config);
        if (result == SerialError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

int GunFxSerialSlave::sendResponse(const char* response) {
    if (!_initialized || !_serial) return -1;
    
    size_t len = _serial->print(response);
    _serial->print("\n");
    return len;
}

int GunFxSerialSlave::sendStatus(const GunFxStatus& status) {
    char buf[384];
    snprintf(buf, sizeof(buf),
             "STATUS firing=%d flashActive=%d flashFading=%d heaterOn=%d fanOn=%d fanSpindown=%d "
             "fanSpeed=%u fanOffRemainingMs=%u servo0=%u servo1=%u servo2=%u rpm=%u "
             "shotsFired=%lu heaterOnTimeMs=%lu uptimeMs=%lu freeRam=%lu",
             status.firing ? 1 : 0,
             status.flashActive ? 1 : 0,
             status.flashFading ? 1 : 0,
             status.heaterOn ? 1 : 0,
             status.fanOn ? 1 : 0,
             status.fanSpindown ? 1 : 0,
             status.fanSpeed,
             status.fanOffRemainingMs,
             status.servoUs[0],
             status.servoUs[1],
             status.servoUs[2],
             status.rateOfFireRpm,
             (unsigned long)status.shotsFired,
             (unsigned long)status.heaterOnTimeMs,
             (unsigned long)status.uptimeMs,
             (unsigned long)status.freeRam);
    return sendResponse(buf);
}

int GunFxSerialSlave::sendError(uint8_t errorCode, const char* message) {
    char buf[128];
    snprintf(buf, sizeof(buf), "ERROR code=%u msg=%s",
             errorCode, message ? message : "");
    return sendResponse(buf);
}

int GunFxSerialSlave::sendAck() {
    return sendResponse("ACK");
}

int GunFxSerialSlave::sendNack(uint8_t errorCode, const char* reason) {
    char buf[128];
    if (reason && reason[0]) {
        snprintf(buf, sizeof(buf), "NACK code=%u reason=%s", errorCode, reason);
    } else {
        snprintf(buf, sizeof(buf), "NACK code=%u reason=%s", errorCode, GunFxError::getMessage(errorCode));
    }
    return sendResponse(buf);
}

// ============================================================================
// GunFxSerialMasterBinary Implementation
// ============================================================================

bool GunFxSerialMasterBinary::begin(UsbHost* usbHost, int deviceIndex) {
    if (!SerialBusBinary::begin(usbHost, deviceIndex)) {
        return false;
    }

    _usbHostRef = usbHost;

    SerialBusBinary::onPacketReceived([this](uint8_t type, const uint8_t* payload, size_t len) {
        handlePacket(type, payload, len);
    });

    _slaveReady = false;
    _slaveName[0] = '\0';

    return true;
}

void GunFxSerialMasterBinary::setCompatibleVersions(const char** versions, size_t count) {
    _compatibleVersions = versions;
    _compatibleVersionCount = count;
}

bool GunFxSerialMasterBinary::checkVersionCompatibility(const char* version) {
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

int GunFxSerialMasterBinary::process() {
    return SerialBusBinary::process();
}

int GunFxSerialMasterBinary::sendInit(unsigned long keepaliveMs) {
    if (!_usbHostRef) return -1;
    
    char buf[64];
    if (keepaliveMs > 0) {
        snprintf(buf, sizeof(buf), "INIT protocol=binary keepalive=%lu", keepaliveMs);
    } else {
        snprintf(buf, sizeof(buf), "INIT protocol=binary keepalive=off");
    }
    
    int written = _usbHostRef->cdcPrintln(SerialBusBinary::deviceIndex(), buf);
    if (written > 0) {
        _lastSendMs = millis();
    }
    return written;
}

CommandResult GunFxSerialMasterBinary::sendPacketBlocking(uint8_t type, const uint8_t* payload, size_t len) {
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

CommandResult GunFxSerialMasterBinary::waitForAckNack() {
    unsigned long startMs = millis();
    
    while (_pendingAckNack) {
        SerialBusBinary::process();
        
        if (millis() - startMs > _commandTimeoutMs) {
            _pendingAckNack = false;
            _lastCommandResult = CommandResult::Timeout();
            return _lastCommandResult;
        }
        
        delay(1);
    }
    
    return _lastCommandResult;
}

CommandResult GunFxSerialMasterBinary::triggerOn(uint16_t rpm) {
    uint8_t payload[2];
    CoreProtocol::putU16LE(payload, rpm);
    return sendPacketBlocking(GunFxPacket::TRIGGER_ON, payload, sizeof(payload));
}

CommandResult GunFxSerialMasterBinary::triggerOff(uint16_t fanDelayMs) {
    uint8_t payload[2];
    CoreProtocol::putU16LE(payload, fanDelayMs);
    return sendPacketBlocking(GunFxPacket::TRIGGER_OFF, payload, sizeof(payload));
}

CommandResult GunFxSerialMasterBinary::setServoPosition(uint8_t servoId, uint16_t pulseUs) {
    uint8_t payload[3];
    payload[0] = servoId;
    CoreProtocol::putU16LE(&payload[1], pulseUs);
    return sendPacketBlocking(GunFxPacket::SRV_SET, payload, sizeof(payload));
}

CommandResult GunFxSerialMasterBinary::setServoConfig(const GunFxServoConfig& config) {
    uint8_t payload[11];
    payload[0] = config.servoId;
    CoreProtocol::putU16LE(&payload[1], config.minUs);
    CoreProtocol::putU16LE(&payload[3], config.maxUs);
    CoreProtocol::putU16LE(&payload[5], config.maxSpeedUsPerSec);
    CoreProtocol::putU16LE(&payload[7], config.maxAccelUsPerSec2);
    CoreProtocol::putU16LE(&payload[9], config.maxDecelUsPerSec2);
    return sendPacketBlocking(GunFxPacket::SRV_SETTINGS, payload, sizeof(payload));
}

CommandResult GunFxSerialMasterBinary::setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs) {
    uint8_t payload[5];
    payload[0] = servoId;
    CoreProtocol::putU16LE(&payload[1], jerkUs);
    CoreProtocol::putU16LE(&payload[3], varianceUs);
    return sendPacketBlocking(GunFxPacket::SRV_RECOIL_JERK, payload, sizeof(payload));
}

CommandResult GunFxSerialMasterBinary::setSmokeHeater(bool on) {
    uint8_t payload[1] = { on ? (uint8_t)1 : (uint8_t)0 };
    return sendPacketBlocking(GunFxPacket::SMOKE_HEAT, payload, sizeof(payload));
}

CommandResult GunFxSerialMasterBinary::setSmokeSettings(const GunFxSmokeConfig& config) {
    uint8_t payload[8];
    payload[0] = config.fanPulsing ? 1 : 0;
    payload[1] = config.fanSpeed;
    payload[2] = config.fanPulseHigh;
    payload[3] = config.fanPulseLow;
    CoreProtocol::putU16LE(&payload[4], config.fanPulseMs);
    CoreProtocol::putU16LE(&payload[6], config.fanSpindownMs);
    return sendPacketBlocking(GunFxPacket::SMOKE_SETTINGS, payload, sizeof(payload));
}

CommandResult GunFxSerialMasterBinary::requestStatus() {
    return sendPacketBlocking(CorePacket::STATUS_REQ, nullptr, 0);
}

void GunFxSerialMasterBinary::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case CorePacket::INIT_READY:
            _slaveReady = true;
            
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
                    strncpy(_slaveName, name, sizeof(_slaveName) - 1);
                    _slaveName[sizeof(_slaveName) - 1] = '\0';
                    strncpy(_boardInfo.deviceName, name, sizeof(_boardInfo.deviceName) - 1);
                }
                if (version) {
                    strncpy(_boardInfo.firmwareVersion, version, sizeof(_boardInfo.firmwareVersion) - 1);
                }
                if (platform) {
                    strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
                }
                if (cpuStr) {
                    _boardInfo.cpuFrequencyMHz = atoi(cpuStr);
                }
                if (ramStr) {
                    _boardInfo.freeRamBytes = atoi(ramStr);
                }
                
                _boardInfo.versionCompatible = checkVersionCompatibility(_boardInfo.firmwareVersion);
            }
            
            if (_readyCallback) {
                _readyCallback(_slaveName);
            }
            break;

        case CorePacket::STATUS:
            if (len >= 28) {
                uint8_t flags = payload[0];
                _lastStatus.firing = (flags & 0x01) != 0;
                _lastStatus.flashActive = (flags & 0x02) != 0;
                _lastStatus.flashFading = (flags & 0x04) != 0;
                _lastStatus.heaterOn = (flags & 0x08) != 0;
                _lastStatus.fanOn = (flags & 0x10) != 0;
                _lastStatus.fanSpindown = (flags & 0x20) != 0;
                
                _lastStatus.fanSpeed = payload[1];
                _lastStatus.fanOffRemainingMs = CoreProtocol::getU16LE(&payload[2]);
                _lastStatus.servoUs[0] = CoreProtocol::getU16LE(&payload[4]);
                _lastStatus.servoUs[1] = CoreProtocol::getU16LE(&payload[6]);
                _lastStatus.servoUs[2] = CoreProtocol::getU16LE(&payload[8]);
                _lastStatus.rateOfFireRpm = CoreProtocol::getU16LE(&payload[10]);
                _lastStatus.shotsFired = CoreProtocol::getU32LE(&payload[12]);
                _lastStatus.heaterOnTimeMs = CoreProtocol::getU32LE(&payload[16]);
                _lastStatus.uptimeMs = CoreProtocol::getU32LE(&payload[20]);
                _lastStatus.freeRam = CoreProtocol::getU32LE(&payload[24]);
                
                if (_pendingAckNack) {
                    _receivedAck = true;
                    _pendingAckNack = false;
                    _lastCommandResult = CommandResult::Ack();
                }
                
                if (_statusCallback) {
                    _statusCallback(_lastStatus);
                }
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
                strncpy(_lastNackReason, reason[0] ? reason : GunFxError::getMessage(errorCode), sizeof(_lastNackReason) - 1);
                _lastCommandResult = CommandResult::Nack(errorCode, _lastNackReason);
            }
            break;

        default:
            break;
    }
}

// ============================================================================
// GunFxSerialSlaveBinary Implementation
// ============================================================================

bool GunFxSerialSlaveBinary::begin(Stream* serial, const char* moduleName) {
    if (!serial) return false;

    _serial = serial;
    _rxIndex = 0;
    _masterConnected = false;
    _lastRxTimeMs = 0;

    strncpy(_moduleName, moduleName, sizeof(_moduleName) - 1);
    _moduleName[sizeof(_moduleName) - 1] = '\0';

    _initialized = true;
    return true;
}

void GunFxSerialSlaveBinary::end() {
    _initialized = false;
    _serial = nullptr;
    _masterConnected = false;
}

int GunFxSerialSlaveBinary::process() {
    if (!_initialized || !_serial) return 0;

    int packetsProcessed = 0;

    while (_serial->available()) {
        uint8_t byte = _serial->read();
        
        if (byte == CoreProtocol::FRAME_DELIMITER) {
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

    if (_connectionTimeoutMs > 0 && _masterConnected) {
        unsigned long now = millis();
        if (now - _lastRxTimeMs > _connectionTimeoutMs) {
            _masterConnected = false;
        }
    }

    return packetsProcessed;
}

void GunFxSerialSlaveBinary::processFrame(const uint8_t* frame, size_t frameLen) {
    uint8_t decoded[CoreProtocol::MAX_PACKET_SIZE];
    size_t decodedLen = CoreProtocol::cobsDecode(frame, frameLen, decoded, sizeof(decoded));
    
    if (decodedLen == 0) return;

    uint8_t type;
    const uint8_t* payload;
    size_t payloadLen;
    
    if (!CoreProtocol::parsePacket(decoded, decodedLen, &type, &payload, &payloadLen)) {
        return;
    }

    _lastRxTimeMs = millis();
    _masterConnected = true;

    handlePacket(type, payload, payloadLen);
}

CommandHandleResult GunFxSerialSlaveBinary::tryProcessPacket(uint8_t type, const uint8_t* payload, size_t len) {
    _lastRxTimeMs = millis();
    _masterConnected = true;
    
    return handlePacket(type, payload, len);
}

CommandHandleResult GunFxSerialSlaveBinary::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case CorePacket::KEEPALIVE:
            return CommandHandleResult::Handled;

        case CorePacket::STATUS_REQ:
            if (_statusRequestCallback) {
                GunFxStatus status = _statusRequestCallback();
                sendStatus(status);
            } else {
                sendStatus(GunFxStatus{});
            }
            return CommandHandleResult::Handled;

        case GunFxPacket::TRIGGER_ON:
            if (len >= 2) {
                uint16_t rpm = CoreProtocol::getU16LE(payload);
                if (_triggerOnCallback) {
                    uint8_t result = _triggerOnCallback(rpm);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case GunFxPacket::TRIGGER_OFF:
            if (len >= 2) {
                uint16_t fanDelayMs = CoreProtocol::getU16LE(payload);
                if (_triggerOffCallback) {
                    uint8_t result = _triggerOffCallback(fanDelayMs);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case GunFxPacket::SRV_SET:
            if (len >= 3) {
                uint8_t servoId = payload[0];
                uint16_t pulseUs = CoreProtocol::getU16LE(&payload[1]);
                if (_servoSetCallback) {
                    uint8_t result = _servoSetCallback(servoId, pulseUs);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case GunFxPacket::SRV_SETTINGS:
            if (len >= 11) {
                GunFxServoConfig config;
                config.servoId = payload[0];
                config.minUs = CoreProtocol::getU16LE(&payload[1]);
                config.maxUs = CoreProtocol::getU16LE(&payload[3]);
                config.maxSpeedUsPerSec = CoreProtocol::getU16LE(&payload[5]);
                config.maxAccelUsPerSec2 = CoreProtocol::getU16LE(&payload[7]);
                config.maxDecelUsPerSec2 = CoreProtocol::getU16LE(&payload[9]);
                if (_servoSettingsCallback) {
                    uint8_t result = _servoSettingsCallback(config);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case GunFxPacket::SRV_RECOIL_JERK:
            if (len >= 5) {
                GunFxServoConfig config;
                config.servoId = payload[0];
                config.recoilJerkUs = CoreProtocol::getU16LE(&payload[1]);
                config.recoilJerkVarianceUs = CoreProtocol::getU16LE(&payload[3]);
                if (_servoSettingsCallback) {
                    uint8_t result = _servoSettingsCallback(config);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case GunFxPacket::SMOKE_HEAT:
            if (len >= 1) {
                bool on = payload[0] != 0;
                if (_smokeHeatCallback) {
                    uint8_t result = _smokeHeatCallback(on);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case GunFxPacket::SMOKE_SETTINGS:
            if (len >= 8) {
                GunFxSmokeConfig config;
                config.fanPulsing = payload[0] != 0;
                config.fanSpeed = payload[1];
                config.fanPulseHigh = payload[2];
                config.fanPulseLow = payload[3];
                config.fanPulseMs = CoreProtocol::getU16LE(&payload[4]);
                config.fanSpindownMs = CoreProtocol::getU16LE(&payload[6]);
                if (_smokeSettingsCallback) {
                    uint8_t result = _smokeSettingsCallback(config);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

int GunFxSerialSlaveBinary::sendRawPacket(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_initialized || !_serial) return -1;

    uint8_t encoded[CoreProtocol::COBS_BUFFER_SIZE];
    size_t encodedLen = CoreProtocol::encodePacket(encoded, type, payload, len);
    
    if (encodedLen == 0) return -1;

    size_t written = _serial->write(encoded, encodedLen);
    _serial->write(CoreProtocol::FRAME_DELIMITER);
    
    return (int)written;
}

int GunFxSerialSlaveBinary::sendStatus(const GunFxStatus& status) {
    uint8_t payload[28];
    
    uint8_t flags = 0;
    if (status.firing) flags |= 0x01;
    if (status.flashActive) flags |= 0x02;
    if (status.flashFading) flags |= 0x04;
    if (status.heaterOn) flags |= 0x08;
    if (status.fanOn) flags |= 0x10;
    if (status.fanSpindown) flags |= 0x20;
    
    payload[0] = flags;
    payload[1] = status.fanSpeed;
    CoreProtocol::putU16LE(&payload[2], status.fanOffRemainingMs);
    CoreProtocol::putU16LE(&payload[4], status.servoUs[0]);
    CoreProtocol::putU16LE(&payload[6], status.servoUs[1]);
    CoreProtocol::putU16LE(&payload[8], status.servoUs[2]);
    CoreProtocol::putU16LE(&payload[10], status.rateOfFireRpm);
    CoreProtocol::putU32LE(&payload[12], status.shotsFired);
    CoreProtocol::putU32LE(&payload[16], status.heaterOnTimeMs);
    CoreProtocol::putU32LE(&payload[20], status.uptimeMs);
    CoreProtocol::putU32LE(&payload[24], status.freeRam);
    
    return sendRawPacket(CorePacket::STATUS, payload, sizeof(payload));
}

int GunFxSerialSlaveBinary::sendError(uint8_t errorCode, const char* message) {
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

int GunFxSerialSlaveBinary::sendAck() {
    return sendRawPacket(CorePacket::ACK, nullptr, 0);
}

int GunFxSerialSlaveBinary::sendNack(uint8_t errorCode, const char* reason) {
    uint8_t payload[64];
    payload[0] = errorCode;
    
    const char* msg = (reason && reason[0]) ? reason : GunFxError::getMessage(errorCode);
    size_t msgLen = strlen(msg);
    if (msgLen > sizeof(payload) - 1) {
        msgLen = sizeof(payload) - 1;
    }
    memcpy(&payload[1], msg, msgLen);
    
    return sendRawPacket(CorePacket::NACK, payload, 1 + msgLen);
}
