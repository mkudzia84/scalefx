/*
 * Serial GunFX Text Protocol - Master/Slave Implementation
 * Human-readable text commands for testing and debugging
 */

#include "serial_gunfx_text.h"

// Namespace alias for TextCmd
namespace TextCmd = SerialProtocol::TextCmd;

// ============================================================================
// GunFxSerialMasterText Implementation
// ============================================================================

bool GunFxSerialMasterText::begin(UsbHost* usbHost, int deviceIndex) {
    // For USB host, we would need to get the stream from UsbHost
    // This is provided for API compatibility but text protocol typically uses direct Stream
    (void)usbHost;
    (void)deviceIndex;
    return false;  // Not supported - use begin(Stream*) instead
}

bool GunFxSerialMasterText::begin(Stream* serial) {
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

void GunFxSerialMasterText::end() {
    _initialized = false;
    _serial = nullptr;
    _slaveReady = false;
    _connected = false;
}

int GunFxSerialMasterText::process() {
    if (!_initialized || !_serial) return 0;

    int linesProcessed = 0;

    // Read available bytes
    while (_serial->available()) {
        char c = _serial->read();
        
        if (c == '\n' || c == '\r') {
            // End of line - process if we have data
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

void GunFxSerialMasterText::processLine(const char* line) {
    // Update connection status
    _lastRxTimeMs = millis();
    _connected = true;

    // Parse command and arguments
    char command[32];
    const char* args = nullptr;
    
    // Extract command (first word)
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

    // Route to appropriate handler
    if (strcmp(command, TextCmd::INIT_READY) == 0) {
        handleInitReady(args);
    } else if (strcmp(command, TextCmd::STATUS) == 0) {
        handleStatus(args);
    } else if (strcmp(command, TextCmd::ERROR) == 0) {
        handleError(args);
    } else if (strcmp(command, TextCmd::ACK) == 0) {
        handleAck();
    } else if (strcmp(command, TextCmd::NACK) == 0) {
        handleNack(args);
    }
}

void GunFxSerialMasterText::handleInitReady(const char* args) {
    _slaveReady = true;
    
    if (args) {
        // Parse: name=X version=Y platform=Z cpuMHz=N ramBytes=N
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

void GunFxSerialMasterText::handleStatus(const char* args) {
    if (!args) return;
    
    char* argsCopy = strdup(args);
    if (!argsCopy) return;
    
    // Parse status flags and values (including new metrics)
    int firing = 0, flashActive = 0, flashFading = 0;
    int heaterOn = 0, fanOn = 0, fanSpindown = 0;
    int fanSpeed = 0, fanOffRemainingMs = 0;
    int servo0 = 0, servo1 = 0, servo2 = 0;
    int rpm = 0;
    long shotsFired = 0, heaterOnTimeMs = 0, uptimeMs = 0, freeRam = 0;
    
    firing = TextParse::getInt(argsCopy, "firing", 0);
    flashActive = TextParse::getInt(argsCopy, "flashActive", 0);
    flashFading = TextParse::getInt(argsCopy, "flashFading", 0);
    heaterOn = TextParse::getInt(argsCopy, "heaterOn", 0);
    fanOn = TextParse::getInt(argsCopy, "fanOn", 0);
    fanSpindown = TextParse::getInt(argsCopy, "fanSpindown", 0);
    fanSpeed = TextParse::getInt(argsCopy, "fanSpeed", 0);
    fanOffRemainingMs = TextParse::getInt(argsCopy, "fanOffRemainingMs", 0);
    servo0 = TextParse::getInt(argsCopy, "servo0", 0);
    servo1 = TextParse::getInt(argsCopy, "servo1", 0);
    servo2 = TextParse::getInt(argsCopy, "servo2", 0);
    rpm = TextParse::getInt(argsCopy, "rpm", 0);
    shotsFired = TextParse::getLong(argsCopy, "shotsFired", 0);
    heaterOnTimeMs = TextParse::getLong(argsCopy, "heaterOnTimeMs", 0);
    uptimeMs = TextParse::getLong(argsCopy, "uptimeMs", 0);
    freeRam = TextParse::getLong(argsCopy, "freeRam", 0);
    
    _lastStatus.firing = firing != 0;
    _lastStatus.flashActive = flashActive != 0;
    _lastStatus.flashFading = flashFading != 0;
    _lastStatus.heaterOn = heaterOn != 0;
    _lastStatus.fanOn = fanOn != 0;
    _lastStatus.fanSpindown = fanSpindown != 0;
    _lastStatus.fanSpeed = (uint8_t)fanSpeed;
    _lastStatus.fanOffRemainingMs = fanOffRemainingMs;
    _lastStatus.servoUs[0] = servo0;
    _lastStatus.servoUs[1] = servo1;
    _lastStatus.servoUs[2] = servo2;
    _lastStatus.rateOfFireRpm = rpm;
    _lastStatus.shotsFired = (uint32_t)shotsFired;
    _lastStatus.heaterOnTimeMs = (uint32_t)heaterOnTimeMs;
    _lastStatus.uptimeMs = (uint32_t)uptimeMs;
    _lastStatus.freeRam = (uint32_t)freeRam;
    
    free(argsCopy);
    
    // STATUS response completes a pending STATUS_REQ command
    if (_pendingAckNack) {
        _receivedAck = true;
        _pendingAckNack = false;
        _lastCommandResult = CommandResult::Ack();
    }
    
    if (_statusCallback) {
        _statusCallback(_lastStatus);
    }
}

void GunFxSerialMasterText::handleError(const char* args) {
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

void GunFxSerialMasterText::handleAck() {
    _receivedAck = true;
    _pendingAckNack = false;
    _lastCommandResult = CommandResult::Ack();
}

void GunFxSerialMasterText::handleNack(const char* args) {
    _receivedNack = true;
    _pendingAckNack = false;
    
    // Parse error code and reason: code=N reason=...
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
    // Use GunFxError::getMessage for domain-specific messages, falls back to SerialError
    strncpy(_lastNackReason, reason[0] ? reason : GunFxError::getMessage(code), sizeof(_lastNackReason) - 1);
    _lastCommandResult = CommandResult::Nack(code, _lastNackReason);
}

bool GunFxSerialMasterText::checkVersionCompatibility(const char* version) {
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

// ----------------------------------------------------------------------------
// Command Transmission
// ----------------------------------------------------------------------------

int GunFxSerialMasterText::sendCommand(const char* command) {
    if (!_initialized || !_serial) return -1;
    
    size_t len = _serial->print(command);
    _serial->print("\n");
    return len;
}

CommandResult GunFxSerialMasterText::sendCommandBlocking(const char* command) {
    if (!_initialized || !_serial) {
        _lastCommandResult = CommandResult::NotConnected();
        return _lastCommandResult;
    }
    
    // Clear ACK/NACK state
    _pendingAckNack = true;
    _receivedAck = false;
    _receivedNack = false;
    _lastNackErrorCode = 0;
    _lastNackReason[0] = '\0';
    
    // Send command
    int sent = sendCommand(command);
    if (sent < 0) {
        _pendingAckNack = false;
        _lastCommandResult = CommandResult::SendFailed();
        return _lastCommandResult;
    }
    
    // If not blocking, return immediately
    if (!_blockingMode) {
        _lastCommandResult = CommandResult::Ack();  // Optimistically assume success
        return _lastCommandResult;
    }
    
    // Wait for ACK/NACK
    return waitForAckNack();
}

CommandResult GunFxSerialMasterText::waitForAckNack() {
    unsigned long startMs = millis();
    
    while (_pendingAckNack) {
        // Process incoming data
        process();
        
        // Check timeout
        if (millis() - startMs > _commandTimeoutMs) {
            _pendingAckNack = false;
            _lastCommandResult = CommandResult::Timeout();
            return _lastCommandResult;
        }
        
        // Small delay to avoid busy-waiting
        delay(1);
    }
    
    return _lastCommandResult;
}

int GunFxSerialMasterText::sendInit() {
    return sendCommand(TextCmd::INIT);
}

int GunFxSerialMasterText::sendShutdown() {
    return sendCommand(TextCmd::SHUTDOWN);
}

// Fire-and-forget: device reboots immediately, no ACK expected
int GunFxSerialMasterText::sendReboot() {
    return sendCommand(TextCmd::REBOOT);
}

// Fire-and-forget: device enters bootloader immediately, no ACK expected
int GunFxSerialMasterText::sendBootsel() {
    return sendCommand(TextCmd::BOOTSEL);
}

int GunFxSerialMasterText::sendKeepalive() {
    return sendCommand(TextCmd::KEEPALIVE);
}

CommandResult GunFxSerialMasterText::triggerOn(uint16_t rpm) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s rpm=%u", TextCmd::TRIGGER_ON, rpm);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMasterText::triggerOff(uint16_t fanDelayMs) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s fanDelayMs=%u", TextCmd::TRIGGER_OFF, fanDelayMs);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMasterText::setServoPosition(uint8_t servoId, uint16_t pulseUs) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s id=%u pulseUs=%u", TextCmd::SERVO_SET, servoId, pulseUs);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMasterText::setServoConfig(const GunFxServoConfig& config) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s id=%u minUs=%u maxUs=%u maxSpeedUsPerSec=%u maxAccelUsPerSec2=%u maxDecelUsPerSec2=%u",
             TextCmd::SERVO_CONFIG,
             config.servoId,
             config.minUs,
             config.maxUs,
             config.maxSpeedUsPerSec,
             config.maxAccelUsPerSec2,
             config.maxDecelUsPerSec2);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMasterText::setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s id=%u jerkUs=%u varianceUs=%u",
             TextCmd::SERVO_RECOIL_JERK, servoId, jerkUs, varianceUs);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMasterText::setSmokeHeater(bool on) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s on=%d", TextCmd::SMOKE_HEAT, on ? 1 : 0);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMasterText::setSmokeSettings(const GunFxSmokeConfig& config) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s pulsing=%d speed=%u pulseHigh=%u pulseLow=%u pulseMs=%u spindownMs=%u",
             TextCmd::SMOKE_SETTINGS,
             config.fanPulsing ? 1 : 0,
             config.fanSpeed,
             config.fanPulseHigh,
             config.fanPulseLow,
             config.fanPulseMs,
             config.fanSpindownMs);
    return sendCommandBlocking(buf);
}

CommandResult GunFxSerialMasterText::requestStatus() {
    // Send STATUS_REQ - slave responds with STATUS line
    return sendCommandBlocking(TextCmd::STATUS_REQ);
}

// ============================================================================
// GunFxSerialSlaveText Implementation
// ============================================================================

bool GunFxSerialSlaveText::begin(Stream* serial, const char* moduleName) {
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

void GunFxSerialSlaveText::end() {
    _initialized = false;
    _serial = nullptr;
    _masterConnected = false;
}

int GunFxSerialSlaveText::process() {
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
    if (_connectionTimeoutMs > 0 && _masterConnected) {
        unsigned long now = millis();
        if (now - _lastRxTimeMs > _connectionTimeoutMs) {
            _masterConnected = false;
        }
    }

    return linesProcessed;
}

CommandHandleResult GunFxSerialSlaveText::tryProcessCommand(const char* line) {
    if (!line || !_serial) return CommandHandleResult::NotMyCommand;

    // Update connection status
    _lastRxTimeMs = millis();
    _masterConnected = true;

    // Parse command and arguments
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

    // Route to handler - return Handled if command is recognized
    // Note: KEEPALIVE is handled by SerialInitHandler, not here
    if (strcmp(command, TextCmd::TRIGGER_ON) == 0) {
        handleTriggerOn(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, TextCmd::TRIGGER_OFF) == 0) {
        handleTriggerOff(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, TextCmd::SERVO_SET) == 0) {
        handleServoSet(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, TextCmd::SERVO_CONFIG) == 0) {
        handleServoConfig(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, TextCmd::SERVO_RECOIL_JERK) == 0) {
        handleRecoilJerk(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, TextCmd::SMOKE_HEAT) == 0) {
        handleSmokeHeat(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, TextCmd::SMOKE_SETTINGS) == 0) {
        handleSmokeSettings(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, TextCmd::STATUS_REQ) == 0) {
        // Master requests status - invoke callback and send response
        if (_statusRequestCallback) {
            GunFxStatus status = _statusRequestCallback();
            sendStatus(status);
        } else {
            sendStatus(GunFxStatus{});
        }
        return CommandHandleResult::Handled;
    }
    
    // Command not recognized - pass to next handler
    return CommandHandleResult::NotMyCommand;
}

// Legacy method - now just calls tryProcessCommand
void GunFxSerialSlaveText::processLine(const char* line) {
    tryProcessCommand(line);
}

void GunFxSerialSlaveText::handleTriggerOn(const char* args) {
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
        sendAck();  // No callback = accept command
    }
}

void GunFxSerialSlaveText::handleTriggerOff(const char* args) {
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

void GunFxSerialSlaveText::handleServoSet(const char* args) {
    if (!args) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    
    char* argsCopy = strdup(args);
    if (!argsCopy) {
        sendNack(SerialError::INTERNAL_ERROR);
        return;
    }
    
    int id = 0, pulseUs = 0;
    id = TextParse::getInt(argsCopy, "id", 0);
    pulseUs = TextParse::getInt(argsCopy, "pulseUs", 0);
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

void GunFxSerialSlaveText::handleServoConfig(const char* args) {
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
    int id = 0, minUs = 0, maxUs = 0, maxSpeed = 0, maxAccel = 0, maxDecel = 0;
    
    id = TextParse::getInt(argsCopy, "id", 0);
    minUs = TextParse::getInt(argsCopy, "minUs", 0);
    maxUs = TextParse::getInt(argsCopy, "maxUs", 0);
    maxSpeed = TextParse::getInt(argsCopy, "maxSpeedUsPerSec", 0);
    maxAccel = TextParse::getInt(argsCopy, "maxAccelUsPerSec2", 0);
    maxDecel = TextParse::getInt(argsCopy, "maxDecelUsPerSec2", 0);
    
    config.servoId = id;
    config.minUs = minUs;
    config.maxUs = maxUs;
    config.maxSpeedUsPerSec = maxSpeed;
    config.maxAccelUsPerSec2 = maxAccel;
    config.maxDecelUsPerSec2 = maxDecel;
    
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

void GunFxSerialSlaveText::handleRecoilJerk(const char* args) {
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
    int id = 0, jerkUs = 0, varianceUs = 0;
    
    id = TextParse::getInt(argsCopy, "id", 0);
    jerkUs = TextParse::getInt(argsCopy, "jerkUs", 0);
    varianceUs = TextParse::getInt(argsCopy, "varianceUs", 0);
    
    config.servoId = id;
    config.recoilJerkUs = jerkUs;
    config.recoilJerkVarianceUs = varianceUs;
    
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

void GunFxSerialSlaveText::handleSmokeHeat(const char* args) {
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

void GunFxSerialSlaveText::handleSmokeSettings(const char* args) {
    GunFxSmokeConfig config;
    int pulsing = 0;
    int speed = 255;
    int pulseHigh = 255;
    int pulseLow = 80;
    int pulseMs = 50;
    int spindownMs = 5000;
    
    if (args) {
        char* argsCopy = strdup(args);
        if (argsCopy) {
            pulsing = TextParse::getInt(argsCopy, "pulsing", pulsing);
            speed = TextParse::getInt(argsCopy, "speed", speed);
            pulseHigh = TextParse::getInt(argsCopy, "pulseHigh", pulseHigh);
            pulseLow = TextParse::getInt(argsCopy, "pulseLow", pulseLow);
            pulseMs = TextParse::getInt(argsCopy, "pulseMs", pulseMs);
            spindownMs = TextParse::getInt(argsCopy, "spindownMs", spindownMs);
            free(argsCopy);
        }
    }
    
    config.fanPulsing = pulsing != 0;
    config.fanSpeed = (uint8_t)constrain(speed, 0, 255);
    config.fanPulseHigh = (uint8_t)constrain(pulseHigh, 0, 255);
    config.fanPulseLow = (uint8_t)constrain(pulseLow, 0, 255);
    config.fanPulseMs = (uint16_t)pulseMs;
    config.fanSpindownMs = (uint16_t)spindownMs;
    
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

// ----------------------------------------------------------------------------
// Response Transmission
// ----------------------------------------------------------------------------

int GunFxSerialSlaveText::sendResponse(const char* response) {
    if (!_initialized || !_serial) return -1;
    
    size_t len = _serial->print(response);
    _serial->print("\n");
    return len;
}

int GunFxSerialSlaveText::sendStatus(const GunFxStatus& status) {
    char buf[384];
    snprintf(buf, sizeof(buf),
             "%s firing=%d flashActive=%d flashFading=%d heaterOn=%d fanOn=%d fanSpindown=%d "
             "fanSpeed=%u fanOffRemainingMs=%u servo0=%u servo1=%u servo2=%u rpm=%u "
             "shotsFired=%lu heaterOnTimeMs=%lu uptimeMs=%lu freeRam=%lu",
             TextCmd::STATUS,
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

int GunFxSerialSlaveText::sendError(uint8_t errorCode, const char* message) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s code=%u msg=%s",
             TextCmd::ERROR, errorCode, message ? message : "");
    return sendResponse(buf);
}

int GunFxSerialSlaveText::sendAck() {
    return sendResponse(TextCmd::ACK);
}

int GunFxSerialSlaveText::sendNack(uint8_t errorCode, const char* reason) {
    char buf[128];
    if (reason && reason[0]) {
        snprintf(buf, sizeof(buf), "%s code=%u reason=%s", TextCmd::NACK, errorCode, reason);
    } else {
        snprintf(buf, sizeof(buf), "%s code=%u reason=%s", TextCmd::NACK, errorCode, GunFxError::getMessage(errorCode));
    }
    return sendResponse(buf);
}
