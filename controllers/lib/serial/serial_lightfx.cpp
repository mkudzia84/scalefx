/*
 * Serial LightFX Protocol - Implementation
 *
 * Text protocol master/slave for LightFX controller.
 */

#include "serial_lightfx.h"
#include <cstring>

using namespace CoreProtocol;

// ============================================================================
// LightFxSerialMaster Implementation
// ============================================================================

bool LightFxSerialMaster::begin(Stream* serial) {
    if (!serial) return false;
    
    _serial = serial;
    _rxIndex = 0;
    _slaveReady = false;
    _slaveName[0] = '\0';
    _connected = false;
    _lastRxTimeMs = 0;
    
    memset(&_boardInfo, 0, sizeof(_boardInfo));
    
    _initialized = true;
    return true;
}

void LightFxSerialMaster::end() {
    _initialized = false;
    _serial = nullptr;
    _slaveReady = false;
    _connected = false;
}

int LightFxSerialMaster::process() {
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

void LightFxSerialMaster::processLine(const char* line) {
    _lastRxTimeMs = millis();
    _connected = true;

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

    // Route to appropriate handler
    if (strcmp(command, TextCmd::INIT_READY) == 0) {
        handleInitReady(args);
    } else if (strcmp(command, LightFxCmd::LED_SEQ_STATUS) == 0) {
        handleSeqStatus(args);
    } else if (strcmp(command, LightFxCmd::LED_STATUS) == 0) {
        handleChannelStatus(args);
    } else if (strcmp(command, LightFxCmd::POWER_STATUS) == 0) {
        handlePowerStatus(args);
    } else if (strcmp(command, TextCmd::ACK) == 0) {
        handleAck();
    } else if (strcmp(command, TextCmd::NACK) == 0) {
        handleNack(args);
    }
}

void LightFxSerialMaster::handleInitReady(const char* args) {
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
            
            free(argsCopy);
        }
    }
    
    if (_readyCallback) {
        _readyCallback(_slaveName);
    }
}

void LightFxSerialMaster::handleSeqStatus(const char* args) {
    if (!args) return;
    
    char* argsCopy = strdup(args);
    if (!argsCopy) return;
    
    LightFxSeqStatus status;
    status.channel = TextParse::getInt(argsCopy, "channel", 0);
    status.playing = TextParse::getInt(argsCopy, "playing", 0) != 0;
    status.eventCount = TextParse::getInt(argsCopy, "events", 0);
    status.currentIndex = TextParse::getInt(argsCopy, "index", 0);
    status.loopCount = TextParse::getULong(argsCopy, "loops", 0);
    
    free(argsCopy);
    
    if (_seqStatusCallback) {
        _seqStatusCallback(status);
    }
}

void LightFxSerialMaster::handleChannelStatus(const char* args) {
    if (!args) return;
    
    char* argsCopy = strdup(args);
    if (!argsCopy) return;
    
    LightFxChannelStatus status;
    status.channel = TextParse::getInt(argsCopy, "ch", 0);
    status.brightness = TextParse::getInt(argsCopy, "brightness", 0);
    
    // Parse seq=playing or seq=stopped
    char seqState[16] = "";
    TextParse::getString(argsCopy, "seq", seqState, sizeof(seqState));
    status.seqPlaying = (strcmp(seqState, "playing") == 0);
    status.seqEventCount = TextParse::getInt(argsCopy, "events", 0);
    
    free(argsCopy);
    
    if (_channelStatusCallback) {
        _channelStatusCallback(status);
    }
}

void LightFxSerialMaster::handlePowerStatus(const char* args) {
    if (!args) return;
    
    char* argsCopy = strdup(args);
    if (!argsCopy) return;
    
    LightFxPowerStatus status;
    status.voltage = TextParse::getFloat(argsCopy, "voltage", 0.0f);
    status.current = TextParse::getFloat(argsCopy, "current", 0.0f);
    status.power = TextParse::getFloat(argsCopy, "power", 0.0f);
    status.available = TextParse::getInt(argsCopy, "available", 0) != 0;
    
    free(argsCopy);
    
    if (_powerStatusCallback) {
        _powerStatusCallback(status);
    }
}

void LightFxSerialMaster::handleAck() {
    _receivedAck = true;
    _receivedNack = false;
    _lastAckReceived = true;
    _pendingAckNack = false;
}

void LightFxSerialMaster::handleNack(const char* args) {
    _receivedAck = false;
    _receivedNack = true;
    _lastAckReceived = false;
    _pendingAckNack = false;
    
    if (args) {
        _lastNackErrorCode = (uint8_t)atoi(args);
        if (_errorCallback) {
            _errorCallback(_lastNackErrorCode, "Command rejected");
        }
    }
}

int LightFxSerialMaster::sendCommand(const char* command) {
    if (!_initialized || !_serial || !command) return -1;
    return _serial->println(command);
}

bool LightFxSerialMaster::sendCommandBlocking(const char* command) {
    if (sendCommand(command) < 0) return false;
    
    if (_blockingMode) {
        return waitForAckNack();
    }
    return true;
}

bool LightFxSerialMaster::waitForAckNack() {
    _pendingAckNack = true;
    _receivedAck = false;
    _receivedNack = false;
    
    unsigned long startTime = millis();
    while (_pendingAckNack) {
        process();
        
        if (millis() - startTime > _commandTimeoutMs) {
            _pendingAckNack = false;
            _lastAckReceived = false;
            return false;
        }
        
        yield();
    }
    
    return _receivedAck;
}

// ============================================================================
// LED Direct Control Commands
// ============================================================================

bool LightFxSerialMaster::ledSet(uint8_t channel, uint8_t brightness) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d %d", LightFxCmd::LED_SET, channel, brightness);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledOff(uint8_t channel) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d", LightFxCmd::LED_OFF, channel);
    return sendCommandBlocking(cmd);
}

// ============================================================================
// LED Sequence Control Commands
// ============================================================================

bool LightFxSerialMaster::ledSeqClear(uint8_t channel) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d", LightFxCmd::LED_SEQ_CLEAR, channel);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledSeqAddOn(uint8_t channel, uint32_t durationMs, uint8_t brightness) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d ON %lu %d", LightFxCmd::LED_SEQ_ADD, channel, durationMs, brightness);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledSeqAddOff(uint8_t channel, uint32_t durationMs) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d OFF %lu", LightFxCmd::LED_SEQ_ADD, channel, durationMs);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledSeqAddFlash(uint8_t channel, uint16_t intervalMs, uint32_t durationMs,
                                          uint8_t brightness, uint8_t dutyPercent) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "%s %d FLASH %u %lu %d %d", 
             LightFxCmd::LED_SEQ_ADD, channel, intervalMs, durationMs, brightness, dutyPercent);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledSeqAddFadeIn(uint8_t channel, uint32_t durationMs, uint8_t brightness) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d FADE_IN %lu %d", LightFxCmd::LED_SEQ_ADD, channel, durationMs, brightness);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledSeqAddFadeOut(uint8_t channel, uint32_t durationMs, uint8_t brightness) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d FADE_OUT %lu %d", LightFxCmd::LED_SEQ_ADD, channel, durationMs, brightness);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledSeqAddFading(uint8_t channel, uint32_t cycleMs, uint32_t durationMs,
                                           uint8_t minBrightness, uint8_t maxBrightness) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "%s %d FADING %lu %lu %d %d", 
             LightFxCmd::LED_SEQ_ADD, channel, cycleMs, durationMs, minBrightness, maxBrightness);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledSeqStart(uint8_t channel) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d", LightFxCmd::LED_SEQ_START, channel);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledSeqStop(uint8_t channel) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d", LightFxCmd::LED_SEQ_STOP, channel);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledSeqRestart(uint8_t channel) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d", LightFxCmd::LED_SEQ_RESTART, channel);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::ledSeqStatus(uint8_t channel) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d", LightFxCmd::LED_SEQ_STATUS, channel);
    // Status commands don't expect ACK - they get a status response
    return sendCommand(cmd) > 0;
}

bool LightFxSerialMaster::ledStatus() {
    return sendCommand(LightFxCmd::LED_STATUS) > 0;
}

// ============================================================================
// Servo Control Commands
// ============================================================================

bool LightFxSerialMaster::servoSet(uint8_t id, int pulseUs) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d %d", LightFxCmd::SERVO_SET, id, pulseUs);
    return sendCommandBlocking(cmd);
}

bool LightFxSerialMaster::servoSettings(uint8_t id, int minUs, int maxUs,
                                         int speed, int accel, int decel) {
    char cmd[128];
    int offset = snprintf(cmd, sizeof(cmd), "%s %d", LightFxCmd::SERVO_SETTINGS, id);
    
    if (minUs >= 0) offset += snprintf(cmd + offset, sizeof(cmd) - offset, " min=%d", minUs);
    if (maxUs >= 0) offset += snprintf(cmd + offset, sizeof(cmd) - offset, " max=%d", maxUs);
    if (speed >= 0) offset += snprintf(cmd + offset, sizeof(cmd) - offset, " speed=%d", speed);
    if (accel >= 0) offset += snprintf(cmd + offset, sizeof(cmd) - offset, " accel=%d", accel);
    if (decel >= 0) offset += snprintf(cmd + offset, sizeof(cmd) - offset, " decel=%d", decel);
    
    return sendCommandBlocking(cmd);
}

// ============================================================================
// Power Monitor Commands
// ============================================================================

bool LightFxSerialMaster::powerStatus() {
    return sendCommand(LightFxCmd::POWER_STATUS) > 0;
}

// ============================================================================
// Connection Management Commands
// ============================================================================

bool LightFxSerialMaster::sendInit() {
    return sendCommand(TextCmd::INIT) > 0;
}

bool LightFxSerialMaster::sendShutdown() {
    return sendCommandBlocking(TextCmd::SHUTDOWN);
}

bool LightFxSerialMaster::sendKeepalive() {
    return sendCommand(TextCmd::KEEPALIVE) > 0;
}

// ============================================================================
// LightFxSerialSlave Implementation
// ============================================================================

bool LightFxSerialSlave::begin(Stream* serial) {
    if (!serial) return false;
    _serial = serial;
    _initialized = true;
    return true;
}

void LightFxSerialSlave::end() {
    _serial = nullptr;
    _initialized = false;
}

// ============================================================================
// ITextCommandHandler Interface
// ============================================================================

CommandHandleResult LightFxSerialSlave::tryProcessCommand(const char* line) {
    if (!line || !_serial || !_initialized) return CommandHandleResult::NotMyCommand;
    
    // Parse command and arguments
    char command[24];
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
    
    // Route to handler
    if (strcmp(command, LightFxCmd::LED_SET) == 0) {
        handleLedSet(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::LED_OFF) == 0) {
        handleLedOff(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::LED_SEQ_CLEAR) == 0) {
        handleLedSeqClear(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::LED_SEQ_ADD) == 0) {
        handleLedSeqAdd(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::LED_SEQ_START) == 0) {
        handleLedSeqStart(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::LED_SEQ_STOP) == 0) {
        handleLedSeqStop(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::LED_SEQ_RESTART) == 0) {
        handleLedSeqRestart(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::LED_SEQ_STATUS) == 0) {
        handleLedSeqStatus(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::LED_STATUS) == 0) {
        handleLedStatus();
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::SERVO_SET) == 0) {
        handleServoSet(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::SERVO_SETTINGS) == 0) {
        handleServoSettings(args);
        return CommandHandleResult::Handled;
    }
    if (strcmp(command, LightFxCmd::POWER_STATUS) == 0) {
        handlePowerStatus();
        return CommandHandleResult::Handled;
    }
    
    return CommandHandleResult::NotMyCommand;
}

// ============================================================================
// Response Helpers
// ============================================================================

void LightFxSerialSlave::sendAck() {
    if (_serial) {
        _serial->println("ACK");
    }
}

void LightFxSerialSlave::sendNack(uint8_t errorCode) {
    if (_serial) {
        _serial->print("NACK ");
        _serial->println(errorCode);
    }
}

void LightFxSerialSlave::sendLine(const char* line) {
    if (_serial && line) {
        _serial->println(line);
    }
}

// ============================================================================
// LED Command Handlers
// ============================================================================

void LightFxSerialSlave::handleLedSet(const char* args) {
    if (!args) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    // Parse: <channel> <brightness>
    int channel = 0, brightness = 0;
    if (sscanf(args, "%d %d", &channel, &brightness) < 2) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    if (channel < 1 || channel > 8) {
        sendNack(LightFxError::INVALID_CHANNEL);
        return;
    }
    
    if (brightness < 0 || brightness > 255) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    if (_ledSetCallback) {
        uint8_t result = _ledSetCallback(channel, brightness);
        if (result == LightFxError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void LightFxSerialSlave::handleLedOff(const char* args) {
    if (!args) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    // Parse: <channel> (0 = all)
    int channel = 0;
    if (sscanf(args, "%d", &channel) < 1) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    if (channel < 0 || channel > 8) {
        sendNack(LightFxError::INVALID_CHANNEL);
        return;
    }
    
    if (_ledOffCallback) {
        uint8_t result = _ledOffCallback(channel);
        if (result == LightFxError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void LightFxSerialSlave::handleLedSeqClear(const char* args) {
    if (!args) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    int channel = 0;
    if (sscanf(args, "%d", &channel) < 1) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    if (channel < 0 || channel > 8) {
        sendNack(LightFxError::INVALID_CHANNEL);
        return;
    }
    
    if (_ledSeqClearCallback) {
        uint8_t result = _ledSeqClearCallback(channel);
        if (result == LightFxError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void LightFxSerialSlave::handleLedSeqAdd(const char* args) {
    if (!args) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    // Parse: <channel> <event_type> [params...]
    int channel = 0;
    char eventType[16];
    const char* params = nullptr;
    
    // Get channel number
    char* endPtr = nullptr;
    channel = strtol(args, &endPtr, 10);
    
    if (endPtr == args || channel < 1 || channel > 8) {
        sendNack(LightFxError::INVALID_CHANNEL);
        return;
    }
    
    // Skip whitespace to get event type
    while (*endPtr == ' ') endPtr++;
    
    if (*endPtr == '\0') {
        sendNack(LightFxError::INVALID_EVENT);
        return;
    }
    
    // Extract event type
    const char* typeStart = endPtr;
    while (*endPtr != '\0' && *endPtr != ' ') endPtr++;
    
    size_t typeLen = endPtr - typeStart;
    if (typeLen >= sizeof(eventType)) typeLen = sizeof(eventType) - 1;
    strncpy(eventType, typeStart, typeLen);
    eventType[typeLen] = '\0';
    
    // Rest is params (may be empty)
    while (*endPtr == ' ') endPtr++;
    params = endPtr;
    
    if (_ledSeqAddCallback) {
        uint8_t result = _ledSeqAddCallback(channel, eventType, params);
        if (result == LightFxError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void LightFxSerialSlave::handleLedSeqStart(const char* args) {
    if (!args) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    int channel = 0;
    if (sscanf(args, "%d", &channel) < 1) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    if (channel < 0 || channel > 8) {
        sendNack(LightFxError::INVALID_CHANNEL);
        return;
    }
    
    if (_ledSeqStartCallback) {
        uint8_t result = _ledSeqStartCallback(channel);
        if (result == LightFxError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void LightFxSerialSlave::handleLedSeqStop(const char* args) {
    if (!args) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    int channel = 0;
    if (sscanf(args, "%d", &channel) < 1) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    if (channel < 0 || channel > 8) {
        sendNack(LightFxError::INVALID_CHANNEL);
        return;
    }
    
    if (_ledSeqStopCallback) {
        uint8_t result = _ledSeqStopCallback(channel);
        if (result == LightFxError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void LightFxSerialSlave::handleLedSeqRestart(const char* args) {
    if (!args) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    int channel = 0;
    if (sscanf(args, "%d", &channel) < 1) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    if (channel < 0 || channel > 8) {
        sendNack(LightFxError::INVALID_CHANNEL);
        return;
    }
    
    if (_ledSeqRestartCallback) {
        uint8_t result = _ledSeqRestartCallback(channel);
        if (result == LightFxError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void LightFxSerialSlave::handleLedSeqStatus(const char* args) {
    if (!args) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    int channel = 0;
    if (sscanf(args, "%d", &channel) < 1) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    if (channel < 1 || channel > 8) {
        sendNack(LightFxError::INVALID_CHANNEL);
        return;
    }
    
    if (_ledSeqStatusCallback) {
        char buffer[128];
        _ledSeqStatusCallback(channel, buffer, sizeof(buffer));
        sendLine(buffer);
    } else {
        // Default response
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "LED_SEQ_STATUS channel=%d playing=0 events=0 index=0 loops=0", channel);
        sendLine(buffer);
    }
}

void LightFxSerialSlave::handleLedStatus() {
    if (_ledStatusCallback) {
        char buffer[512];
        _ledStatusCallback(buffer, sizeof(buffer));
        sendLine(buffer);
    } else {
        // Default response for all 8 channels
        for (int ch = 1; ch <= 8; ch++) {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "LED_STATUS ch=%d brightness=0 seq=stopped events=0", ch);
            sendLine(buffer);
        }
    }
}

// ============================================================================
// Servo Command Handlers
// ============================================================================

void LightFxSerialSlave::handleServoSet(const char* args) {
    if (!args) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    // Parse: <id> <pulse_us>
    int id = 0, pulseUs = 0;
    if (sscanf(args, "%d %d", &id, &pulseUs) < 2) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    if (id < 1 || id > 3) {
        sendNack(LightFxError::INVALID_SERVO);
        return;
    }
    
    if (_servoSetCallback) {
        uint8_t result = _servoSetCallback(id, pulseUs);
        if (result == LightFxError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

void LightFxSerialSlave::handleServoSettings(const char* args) {
    if (!args) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    // Parse: <id> min=<us> max=<us> speed=<us/s> accel=<us/s2> decel=<us/s2>
    // First get the servo id
    char* endPtr = nullptr;
    int id = strtol(args, &endPtr, 10);
    
    if (endPtr == args || id < 1 || id > 3) {
        sendNack(LightFxError::INVALID_SERVO);
        return;
    }
    
    // Skip to key=value pairs
    while (*endPtr == ' ') endPtr++;
    
    // Make a copy for parsing
    char* argsCopy = strdup(endPtr);
    if (!argsCopy) {
        sendNack(LightFxError::INVALID_PARAM);
        return;
    }
    
    // Parse optional parameters (use -1 as "not provided" sentinel)
    int minUs = TextParse::getInt(argsCopy, "min", -1);
    int maxUs = TextParse::getInt(argsCopy, "max", -1);
    int speed = TextParse::getInt(argsCopy, "speed", -1);
    int accel = TextParse::getInt(argsCopy, "accel", -1);
    int decel = TextParse::getInt(argsCopy, "decel", -1);
    
    free(argsCopy);
    
    if (_servoSettingsCallback) {
        uint8_t result = _servoSettingsCallback(id, minUs, maxUs, speed, accel, decel);
        if (result == LightFxError::OK) {
            sendAck();
        } else {
            sendNack(result);
        }
    } else {
        sendAck();
    }
}

// ============================================================================
// Power Command Handlers
// ============================================================================

void LightFxSerialSlave::handlePowerStatus() {
    if (_powerStatusCallback) {
        char buffer[128];
        _powerStatusCallback(buffer, sizeof(buffer));
        sendLine(buffer);
    } else {
        // Default response
        sendLine("POWER_STATUS voltage=0.00 current=0 power=0 available=0");
    }
}

// ============================================================================
// ============================================================================
// BINARY PROTOCOL IMPLEMENTATION
// ============================================================================
// ============================================================================

// ============================================================================
// LightFxSerialMasterBinary Implementation
// ============================================================================

bool LightFxSerialMasterBinary::begin(UsbHost* usbHost, int deviceIndex) {
    if (!SerialBus::begin(usbHost, deviceIndex)) {
        return false;
    }

    _usbHostRef = usbHost;
    _slaveReady = false;
    _slaveName[0] = '\0';

    // Set up internal packet handler
    SerialBus::onPacketReceived([this](uint8_t type, const uint8_t* payload, size_t len) {
        handlePacket(type, payload, len);
    });

    return true;
}

int LightFxSerialMasterBinary::process() {
    return SerialBus::process();
}

void LightFxSerialMasterBinary::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case CorePacket::INIT_READY:
            _slaveReady = true;
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
                    strncpy(_slaveName, name, sizeof(_slaveName) - 1);
                    strncpy(_boardInfo.deviceName, name, sizeof(_boardInfo.deviceName) - 1);
                }
                if (version) strncpy(_boardInfo.firmwareVersion, version, sizeof(_boardInfo.firmwareVersion) - 1);
                if (platform) strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
                if (cpuStr) _boardInfo.cpuFrequencyMHz = atoi(cpuStr);
                if (ramStr) _boardInfo.freeRamBytes = atoi(ramStr);
            }
            if (_readyCallback) _readyCallback(_slaveName);
            break;

        case LightFxPacket::LED_SEQ_STATUS_RESP:
            if (len >= 8) {
                LightFxSeqStatus status;
                status.channel = payload[0];
                status.playing = payload[1] != 0;
                status.eventCount = payload[2];
                status.currentIndex = payload[3];
                status.loopCount = CoreProtocol::getU32LE(&payload[4]);
                if (_seqStatusCallback) _seqStatusCallback(status);
            }
            break;

        case LightFxPacket::LED_STATUS_RESP:
            // Parse channel status: [ch:u8][brightness:u8][seq_playing:u8][events:u8] per channel
            for (size_t i = 0; i + 4 <= len; i += 4) {
                LightFxChannelStatus status;
                status.channel = payload[i];
                status.brightness = payload[i + 1];
                status.seqPlaying = payload[i + 2] != 0;
                status.seqEventCount = payload[i + 3];
                if (_channelStatusCallback) _channelStatusCallback(status);
            }
            break;

        case LightFxPacket::POWER_STATUS_RESP:
            if (len >= 7) {
                LightFxPowerStatus status;
                status.voltage = CoreProtocol::getU16LE(&payload[0]) / 1000.0f;  // mV to V
                status.current = (float)(int16_t)CoreProtocol::getU16LE(&payload[2]);  // mA
                status.power = (float)CoreProtocol::getU16LE(&payload[4]);  // mW
                status.available = payload[6] != 0;
                if (_powerStatusCallback) _powerStatusCallback(status);
            }
            break;

        case CorePacket::ACK:
            _receivedAck = true;
            _pendingAckNack = false;
            _lastAckReceived = true;
            break;

        case CorePacket::NACK:
            _receivedNack = true;
            _pendingAckNack = false;
            _lastAckReceived = false;
            if (len >= 1) {
                _lastNackErrorCode = payload[0];
                if (_errorCallback) _errorCallback(_lastNackErrorCode, "Command rejected");
            }
            break;

        default:
            break;
    }
}

bool LightFxSerialMasterBinary::sendPacketBlocking(uint8_t type, const uint8_t* payload, size_t len) {
    if (!isConnected()) return false;
    
    _pendingAckNack = true;
    _receivedAck = false;
    _receivedNack = false;
    
    int sent = sendPacket(type, payload, len);
    if (sent < 0) {
        _pendingAckNack = false;
        return false;
    }
    
    if (!_blockingMode) return true;
    return waitForAckNack();
}

bool LightFxSerialMasterBinary::waitForAckNack() {
    unsigned long startMs = millis();
    
    while (_pendingAckNack) {
        SerialBus::process();
        
        if (millis() - startMs > _commandTimeoutMs) {
            _pendingAckNack = false;
            _lastAckReceived = false;
            return false;
        }
        delay(1);
    }
    
    return _receivedAck;
}

int LightFxSerialMasterBinary::sendInit(unsigned long keepaliveMs) {
    if (!_usbHostRef) return -1;
    
    char buf[64];
    if (keepaliveMs > 0) {
        snprintf(buf, sizeof(buf), "INIT protocol=binary keepalive=%lu", keepaliveMs);
    } else {
        snprintf(buf, sizeof(buf), "INIT protocol=binary keepalive=off");
    }
    
    return _usbHostRef->cdcPrintln(SerialBus::deviceIndex(), buf);
}

// LED Direct Control
bool LightFxSerialMasterBinary::ledSet(uint8_t channel, uint8_t brightness) {
    uint8_t payload[2] = { channel, brightness };
    return sendPacketBlocking(LightFxPacket::LED_SET, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledOff(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacketBlocking(LightFxPacket::LED_OFF, payload, sizeof(payload));
}

// LED Sequence Control
bool LightFxSerialMasterBinary::ledSeqClear(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacketBlocking(LightFxPacket::LED_SEQ_CLEAR, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledSeqAddOn(uint8_t channel, uint16_t durationMs, uint8_t brightness) {
    uint8_t payload[5];
    payload[0] = channel;
    payload[1] = LightFxEventType::ON;
    CoreProtocol::putU16LE(&payload[2], durationMs);
    payload[4] = brightness;
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledSeqAddOff(uint8_t channel, uint16_t durationMs) {
    uint8_t payload[4];
    payload[0] = channel;
    payload[1] = LightFxEventType::OFF;
    CoreProtocol::putU16LE(&payload[2], durationMs);
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledSeqAddFlash(uint8_t channel, uint16_t intervalMs, uint16_t durationMs,
                                                uint8_t brightness, uint8_t dutyPercent) {
    uint8_t payload[8];
    payload[0] = channel;
    payload[1] = LightFxEventType::FLASH;
    CoreProtocol::putU16LE(&payload[2], intervalMs);
    CoreProtocol::putU16LE(&payload[4], durationMs);
    payload[6] = brightness;
    payload[7] = dutyPercent;
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledSeqAddFadeIn(uint8_t channel, uint16_t durationMs, uint8_t brightness) {
    uint8_t payload[5];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADE_IN;
    CoreProtocol::putU16LE(&payload[2], durationMs);
    payload[4] = brightness;
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledSeqAddFadeOut(uint8_t channel, uint16_t durationMs, uint8_t brightness) {
    uint8_t payload[5];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADE_OUT;
    CoreProtocol::putU16LE(&payload[2], durationMs);
    payload[4] = brightness;
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledSeqAddFading(uint8_t channel, uint16_t cycleMs, uint16_t durationMs,
                                                 uint8_t minBrightness, uint8_t maxBrightness) {
    uint8_t payload[8];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADING;
    CoreProtocol::putU16LE(&payload[2], cycleMs);
    CoreProtocol::putU16LE(&payload[4], durationMs);
    payload[6] = minBrightness;
    payload[7] = maxBrightness;
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledSeqStart(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacketBlocking(LightFxPacket::LED_SEQ_START, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledSeqStop(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacketBlocking(LightFxPacket::LED_SEQ_STOP, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledSeqRestart(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacketBlocking(LightFxPacket::LED_SEQ_RESTART, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::ledSeqStatus(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacket(LightFxPacket::LED_SEQ_STATUS, payload, sizeof(payload)) > 0;
}

bool LightFxSerialMasterBinary::ledStatus() {
    return sendPacket(LightFxPacket::LED_STATUS, nullptr, 0) > 0;
}

// Servo Control
bool LightFxSerialMasterBinary::servoSet(uint8_t id, int16_t pulseUs) {
    uint8_t payload[3];
    payload[0] = id;
    CoreProtocol::putI16LE(&payload[1], pulseUs);
    return sendPacketBlocking(LightFxPacket::SERVO_SET, payload, sizeof(payload));
}

bool LightFxSerialMasterBinary::servoSettings(uint8_t id, uint16_t minUs, uint16_t maxUs,
                                               uint16_t speed, uint16_t accel, uint16_t decel) {
    uint8_t payload[11];
    payload[0] = id;
    CoreProtocol::putU16LE(&payload[1], minUs);
    CoreProtocol::putU16LE(&payload[3], maxUs);
    CoreProtocol::putU16LE(&payload[5], speed);
    CoreProtocol::putU16LE(&payload[7], accel);
    CoreProtocol::putU16LE(&payload[9], decel);
    return sendPacketBlocking(LightFxPacket::SERVO_SETTINGS, payload, sizeof(payload));
}

// Power Monitor
bool LightFxSerialMasterBinary::powerStatus() {
    return sendPacket(LightFxPacket::POWER_STATUS, nullptr, 0) > 0;
}

// ============================================================================
// LightFxSerialSlaveBinary Implementation
// ============================================================================

bool LightFxSerialSlaveBinary::begin(Stream* serial) {
    if (!serial) return false;
    _serial = serial;
    _initialized = true;
    return true;
}

void LightFxSerialSlaveBinary::end() {
    _serial = nullptr;
    _initialized = false;
}

CommandHandleResult LightFxSerialSlaveBinary::tryProcessPacket(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_initialized || !_serial) return CommandHandleResult::NotMyCommand;

    // Check if packet type is in LightFX range (0x40-0x5F)
    if (type < 0x40 || type > 0x5F) return CommandHandleResult::NotMyCommand;

    switch (type) {
        case LightFxPacket::LED_SET:
            if (len >= 2) {
                uint8_t channel = payload[0];
                uint8_t brightness = payload[1];
                if (_ledSetCallback) {
                    uint8_t result = _ledSetCallback(channel, brightness);
                    if (result == LightFxError::OK) sendAck();
                    else sendNack(result);
                } else {
                    sendAck();
                }
            } else {
                sendNack(LightFxError::INVALID_PARAM);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_OFF:
            if (len >= 1) {
                uint8_t channel = payload[0];
                if (_ledOffCallback) {
                    uint8_t result = _ledOffCallback(channel);
                    if (result == LightFxError::OK) sendAck();
                    else sendNack(result);
                } else {
                    sendAck();
                }
            } else {
                sendNack(LightFxError::INVALID_PARAM);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_SEQ_CLEAR:
            if (len >= 1) {
                uint8_t channel = payload[0];
                if (_ledSeqClearCallback) {
                    uint8_t result = _ledSeqClearCallback(channel);
                    if (result == LightFxError::OK) sendAck();
                    else sendNack(result);
                } else {
                    sendAck();
                }
            } else {
                sendNack(LightFxError::INVALID_PARAM);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_SEQ_ADD:
            if (len >= 4) {
                uint8_t channel = payload[0];
                uint8_t eventType = payload[1];
                uint16_t param1 = CoreProtocol::getU16LE(&payload[2]);
                uint16_t param2 = (len >= 6) ? CoreProtocol::getU16LE(&payload[4]) : 0;
                uint8_t param3 = (len >= 7) ? payload[6] : 255;
                uint8_t param4 = (len >= 8) ? payload[7] : 50;
                
                if (_ledSeqAddCallback) {
                    uint8_t result = _ledSeqAddCallback(channel, eventType, param1, param2, param3, param4);
                    if (result == LightFxError::OK) sendAck();
                    else sendNack(result);
                } else {
                    sendAck();
                }
            } else {
                sendNack(LightFxError::INVALID_PARAM);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_SEQ_START:
            if (len >= 1) {
                uint8_t channel = payload[0];
                if (_ledSeqStartCallback) {
                    uint8_t result = _ledSeqStartCallback(channel);
                    if (result == LightFxError::OK) sendAck();
                    else sendNack(result);
                } else {
                    sendAck();
                }
            } else {
                sendNack(LightFxError::INVALID_PARAM);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_SEQ_STOP:
            if (len >= 1) {
                uint8_t channel = payload[0];
                if (_ledSeqStopCallback) {
                    uint8_t result = _ledSeqStopCallback(channel);
                    if (result == LightFxError::OK) sendAck();
                    else sendNack(result);
                } else {
                    sendAck();
                }
            } else {
                sendNack(LightFxError::INVALID_PARAM);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_SEQ_RESTART:
            if (len >= 1) {
                uint8_t channel = payload[0];
                if (_ledSeqRestartCallback) {
                    uint8_t result = _ledSeqRestartCallback(channel);
                    if (result == LightFxError::OK) sendAck();
                    else sendNack(result);
                } else {
                    sendAck();
                }
            } else {
                sendNack(LightFxError::INVALID_PARAM);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_SEQ_STATUS:
            if (len >= 1 && _ledSeqStatusCallback) {
                uint8_t channel = payload[0];
                LightFxSeqStatus status;
                status.channel = channel;
                _ledSeqStatusCallback(channel, status);
                sendSeqStatus(status);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_STATUS:
            if (_ledStatusCallback) {
                LightFxChannelStatus channels[8];
                for (uint8_t i = 0; i < 8; i++) {
                    channels[i].channel = i + 1;
                    _ledStatusCallback(i + 1, channels[i]);
                }
                sendChannelStatus(channels, 8);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::SERVO_SET:
            if (len >= 3) {
                uint8_t id = payload[0];
                int16_t pulseUs = CoreProtocol::getI16LE(&payload[1]);
                if (_servoSetCallback) {
                    uint8_t result = _servoSetCallback(id, pulseUs);
                    if (result == LightFxError::OK) sendAck();
                    else sendNack(result);
                } else {
                    sendAck();
                }
            } else {
                sendNack(LightFxError::INVALID_PARAM);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::SERVO_SETTINGS:
            if (len >= 11) {
                uint8_t id = payload[0];
                uint16_t minUs = CoreProtocol::getU16LE(&payload[1]);
                uint16_t maxUs = CoreProtocol::getU16LE(&payload[3]);
                uint16_t speed = CoreProtocol::getU16LE(&payload[5]);
                uint16_t accel = CoreProtocol::getU16LE(&payload[7]);
                uint16_t decel = CoreProtocol::getU16LE(&payload[9]);
                if (_servoSettingsCallback) {
                    uint8_t result = _servoSettingsCallback(id, minUs, maxUs, speed, accel, decel);
                    if (result == LightFxError::OK) sendAck();
                    else sendNack(result);
                } else {
                    sendAck();
                }
            } else {
                sendNack(LightFxError::INVALID_PARAM);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::POWER_STATUS:
            if (_powerStatusCallback) {
                LightFxPowerStatus status;
                _powerStatusCallback(status);
                sendPowerStatus(status);
            }
            return CommandHandleResult::Handled;

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

int LightFxSerialSlaveBinary::sendRawPacket(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_serial) return -1;
    
    uint8_t buffer[CoreProtocol::COBS_BUFFER_SIZE];
    size_t encodedLen = CoreProtocol::encodePacket(buffer, type, payload, len);
    
    return _serial->write(buffer, encodedLen);
}

int LightFxSerialSlaveBinary::sendAck() {
    return sendRawPacket(CorePacket::ACK);
}

int LightFxSerialSlaveBinary::sendNack(uint8_t errorCode) {
    uint8_t payload[1] = { errorCode };
    return sendRawPacket(CorePacket::NACK, payload, sizeof(payload));
}

int LightFxSerialSlaveBinary::sendSeqStatus(const LightFxSeqStatus& status) {
    uint8_t payload[8];
    payload[0] = status.channel;
    payload[1] = status.playing ? 1 : 0;
    payload[2] = status.eventCount;
    payload[3] = status.currentIndex;
    CoreProtocol::putU32LE(&payload[4], status.loopCount);
    return sendRawPacket(LightFxPacket::LED_SEQ_STATUS_RESP, payload, sizeof(payload));
}

int LightFxSerialSlaveBinary::sendChannelStatus(const LightFxChannelStatus* channels, uint8_t count) {
    uint8_t payload[32];  // 4 bytes per channel, max 8 channels
    size_t len = 0;
    
    for (uint8_t i = 0; i < count && len + 4 <= sizeof(payload); i++) {
        payload[len++] = channels[i].channel;
        payload[len++] = channels[i].brightness;
        payload[len++] = channels[i].seqPlaying ? 1 : 0;
        payload[len++] = channels[i].seqEventCount;
    }
    
    return sendRawPacket(LightFxPacket::LED_STATUS_RESP, payload, len);
}

int LightFxSerialSlaveBinary::sendPowerStatus(const LightFxPowerStatus& status) {
    uint8_t payload[7];
    CoreProtocol::putU16LE(&payload[0], (uint16_t)(status.voltage * 1000.0f));  // V to mV
    CoreProtocol::putI16LE(&payload[2], (int16_t)status.current);  // mA
    CoreProtocol::putU16LE(&payload[4], (uint16_t)status.power);  // mW
    payload[6] = status.available ? 1 : 0;
    return sendRawPacket(LightFxPacket::POWER_STATUS_RESP, payload, sizeof(payload));
}
