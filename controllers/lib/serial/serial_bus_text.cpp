/*
 * Serial Bus Text - Implementation
 */

#include "serial_bus_text.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>

// ============================================================================
// SerialBusText Implementation
// ============================================================================

bool SerialBusText::begin(UsbHost* usbHost, int deviceIndex) {
    _usbHost = usbHost;
    _deviceIndex = deviceIndex;
    _initialized = true;  // USB mode - actual stream access via UsbHost
    return true;
}

bool SerialBusText::begin(Stream* stream) {
    if (!stream) return false;
    _stream = stream;
    _initialized = true;
    _rxIndex = 0;
    return true;
}

void SerialBusText::end() {
    _initialized = false;
    _stream = nullptr;
    _usbHost = nullptr;
    _rxIndex = 0;
}

int SerialBusText::sendPacket(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_initialized) return -1;
    
    char line[TEXT_MAX_LINE_LENGTH];
    
    // Get command name from type
    const char* cmd = SerialProtocol::packetTypeToText(type);
    if (!cmd) {
        snprintf(line, sizeof(line), "UNKNOWN_0x%02X", type);
        cmd = line;
    }
    
    // Build text line with payload parameters
    char params[TEXT_MAX_LINE_LENGTH - 32];
    params[0] = '\0';
    encodePayloadToText(params, sizeof(params), type, payload, len);
    
    if (params[0]) {
        snprintf(line, sizeof(line), "%s %s", cmd, params);
    } else {
        snprintf(line, sizeof(line), "%s", cmd);
    }
    
    int result = sendLine(line);
    if (result > 0) {
        _stats.packets_sent++;
    }
    return result;
}

int SerialBusText::sendKeepalive() {
    _lastKeepaliveMs = millis();
    return sendLine("KEEPALIVE");
}

int SerialBusText::sendLine(const char* line) {
    if (!_initialized) return -1;
    
    int len = strlen(line);
    int sent = 0;
    
    if (_stream) {
        sent = _stream->print(line);
        sent += _stream->print("\n");
    }
    // Note: USB host mode would need different handling
    
    return sent;
}

int SerialBusText::sendFormatted(const char* format, ...) {
    char buffer[TEXT_MAX_LINE_LENGTH];
    
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    return sendLine(buffer);
}

int SerialBusText::process() {
    if (!_initialized || !_stream) return 0;
    
    int packetsProcessed = 0;
    
    while (_stream->available()) {
        char c = _stream->read();
        
        if (c == '\n' || c == '\r') {
            if (_rxIndex > 0) {
                _rxBuffer[_rxIndex] = '\0';
                
                // Call raw line callback if set
                if (_lineCallback) {
                    _lineCallback(_rxBuffer);
                }
                
                // Process the line
                processLine(_rxBuffer);
                packetsProcessed++;
                
                _rxIndex = 0;
            }
        } else if (_rxIndex < TEXT_RX_BUFFER_SIZE - 1) {
            _rxBuffer[_rxIndex++] = c;
        }
    }
    
    return packetsProcessed;
}

void SerialBusText::processLine(const char* line) {
    // Skip empty lines
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') return;  // Empty or comment
    
    uint8_t type;
    uint8_t payload[SerialProtocol::MAX_PAYLOAD_SIZE];
    size_t payloadLen = 0;
    
    if (parseLine(line, &type, payload, &payloadLen)) {
        _stats.packets_received++;
        
        if (_rxCallback) {
            _rxCallback(type, payload, payloadLen);
        }
    } else {
        _stats.framing_errors++;
    }
}

bool SerialBusText::parseLine(const char* line, uint8_t* type, uint8_t* payload, size_t* payloadLen) {
    char cmd[32];
    const char* params = TextParse::getCommand(line, cmd, sizeof(cmd));
    
    // Convert command to packet type
    *type = SerialProtocol::textToPacketType(cmd);
    if (*type == 0 && strcmp(cmd, "INIT") != 0) {
        return false;  // Unknown command (except INIT which is 0xF0)
    }
    
    // Decode parameters based on type
    return decodeTextToPayload(params, *type, payload, payloadLen);
}

bool SerialBusText::processKeepalive() {
    if (_keepaliveIntervalMs == 0) return false;
    
    unsigned long now = millis();
    if (now - _lastKeepaliveMs >= _keepaliveIntervalMs) {
        sendKeepalive();
        return true;
    }
    return false;
}

void SerialBusText::encodePayloadToText(char* output, size_t maxLen, uint8_t type,
                                         const uint8_t* payload, size_t payloadLen) {
    output[0] = '\0';
    if (!payload || payloadLen == 0) return;
    
    switch (type) {
        case SerialProtocol::GUNFX_PKT_TRIGGER_ON:
            if (payloadLen >= 2) {
                snprintf(output, maxLen, "rpm=%u", SerialProtocol::getU16LE(payload));
            }
            break;
            
        case SerialProtocol::GUNFX_PKT_TRIGGER_OFF:
            if (payloadLen >= 2) {
                snprintf(output, maxLen, "delay=%u", SerialProtocol::getU16LE(payload));
            }
            break;
            
        case SerialProtocol::GUNFX_PKT_SRV_SET:
            if (payloadLen >= 3) {
                snprintf(output, maxLen, "id=%u us=%u", payload[0], SerialProtocol::getU16LE(payload + 1));
            }
            break;
            
        case SerialProtocol::GUNFX_PKT_SRV_SETTINGS:
            if (payloadLen >= 15) {
                snprintf(output, maxLen, "id=%u min=%u max=%u speed=%u accel=%u decel=%u jerk=%u jerkVar=%u",
                        payload[0],
                        SerialProtocol::getU16LE(payload + 1),
                        SerialProtocol::getU16LE(payload + 3),
                        SerialProtocol::getU16LE(payload + 5),
                        SerialProtocol::getU16LE(payload + 7),
                        SerialProtocol::getU16LE(payload + 9),
                        SerialProtocol::getU16LE(payload + 11),
                        SerialProtocol::getU16LE(payload + 13));
            }
            break;
            
        case SerialProtocol::GUNFX_PKT_SRV_RECOIL_JERK:
            if (payloadLen >= 5) {
                snprintf(output, maxLen, "id=%u jerk=%u var=%u",
                        payload[0],
                        SerialProtocol::getU16LE(payload + 1),
                        SerialProtocol::getU16LE(payload + 3));
            }
            break;
            
        case SerialProtocol::GUNFX_PKT_SMOKE_HEAT:
            if (payloadLen >= 1) {
                snprintf(output, maxLen, "on=%u", payload[0]);
            }
            break;
            
        case SerialProtocol::SFX_PKT_STATUS:
            // GunFX status - complex, encode all fields
            if (payloadLen >= 11) {
                uint8_t flags = payload[0];
                snprintf(output, maxLen, 
                        "firing=%u flash=%u fading=%u heater=%u fan=%u spindown=%u "
                        "fanOff=%u servo1=%u servo2=%u servo3=%u rpm=%u",
                        (flags & 0x01) ? 1 : 0,
                        (flags & 0x02) ? 1 : 0,
                        (flags & 0x04) ? 1 : 0,
                        (flags & 0x08) ? 1 : 0,
                        (flags & 0x10) ? 1 : 0,
                        (flags & 0x20) ? 1 : 0,
                        SerialProtocol::getU16LE(payload + 1),
                        SerialProtocol::getU16LE(payload + 3),
                        SerialProtocol::getU16LE(payload + 5),
                        SerialProtocol::getU16LE(payload + 7),
                        SerialProtocol::getU16LE(payload + 9));
            }
            break;
            
        case SerialProtocol::SFX_PKT_INIT_READY:
            // Module name and board info
            if (payloadLen >= 1) {
                // First byte is name length, followed by name
                uint8_t nameLen = payload[0];
                if (nameLen > 0 && nameLen < payloadLen) {
                    char name[65];
                    memcpy(name, payload + 1, min((size_t)nameLen, sizeof(name) - 1));
                    name[min((size_t)nameLen, sizeof(name) - 1)] = '\0';
                    snprintf(output, maxLen, "name=\"%s\"", name);
                }
            }
            break;
            
        case SerialProtocol::SFX_PKT_ERROR:
            if (payloadLen >= 1) {
                snprintf(output, maxLen, "code=%u", payload[0]);
                if (payloadLen > 1) {
                    char msg[64];
                    size_t msgLen = min(payloadLen - 1, sizeof(msg) - 1);
                    memcpy(msg, payload + 1, msgLen);
                    msg[msgLen] = '\0';
                    snprintf(output + strlen(output), maxLen - strlen(output), " msg=\"%s\"", msg);
                }
            }
            break;
            
        default:
            // For unknown types, hex dump payload
            if (payloadLen > 0) {
                char* p = output;
                size_t remaining = maxLen;
                int n = snprintf(p, remaining, "hex=");
                p += n; remaining -= n;
                for (size_t i = 0; i < payloadLen && remaining > 3; i++) {
                    n = snprintf(p, remaining, "%02X", payload[i]);
                    p += n; remaining -= n;
                }
            }
            break;
    }
}

bool SerialBusText::decodeTextToPayload(const char* params, uint8_t type,
                                         uint8_t* payload, size_t* payloadLen) {
    *payloadLen = 0;
    if (!params) return true;  // No params is valid for many commands
    
    switch (type) {
        case SerialProtocol::GUNFX_PKT_TRIGGER_ON: {
            uint16_t rpm = TextParse::getUInt(params, "rpm", 0);
            SerialProtocol::putU16LE(payload, rpm);
            *payloadLen = 2;
            break;
        }
        
        case SerialProtocol::GUNFX_PKT_TRIGGER_OFF: {
            uint16_t delay = TextParse::getUInt(params, "delay", 0);
            SerialProtocol::putU16LE(payload, delay);
            *payloadLen = 2;
            break;
        }
        
        case SerialProtocol::GUNFX_PKT_SRV_SET: {
            payload[0] = TextParse::getUInt(params, "id", 0);
            SerialProtocol::putU16LE(payload + 1, TextParse::getUInt(params, "us", 1500));
            *payloadLen = 3;
            break;
        }
        
        case SerialProtocol::GUNFX_PKT_SRV_SETTINGS: {
            payload[0] = TextParse::getUInt(params, "id", 0);
            SerialProtocol::putU16LE(payload + 1, TextParse::getUInt(params, "min", 1000));
            SerialProtocol::putU16LE(payload + 3, TextParse::getUInt(params, "max", 2000));
            SerialProtocol::putU16LE(payload + 5, TextParse::getUInt(params, "speed", 0));
            SerialProtocol::putU16LE(payload + 7, TextParse::getUInt(params, "accel", 0));
            SerialProtocol::putU16LE(payload + 9, TextParse::getUInt(params, "decel", 0));
            SerialProtocol::putU16LE(payload + 11, TextParse::getUInt(params, "jerk", 0));
            SerialProtocol::putU16LE(payload + 13, TextParse::getUInt(params, "jerkVar", 0));
            *payloadLen = 15;
            break;
        }
        
        case SerialProtocol::GUNFX_PKT_SRV_RECOIL_JERK: {
            payload[0] = TextParse::getUInt(params, "id", 0);
            SerialProtocol::putU16LE(payload + 1, TextParse::getUInt(params, "jerk", 0));
            SerialProtocol::putU16LE(payload + 3, TextParse::getUInt(params, "var", 0));
            *payloadLen = 5;
            break;
        }
        
        case SerialProtocol::GUNFX_PKT_SMOKE_HEAT: {
            payload[0] = TextParse::getBool(params, "on", false) ? 1 : 0;
            *payloadLen = 1;
            break;
        }
        
        case SerialProtocol::SFX_PKT_STATUS: {
            uint8_t flags = 0;
            if (TextParse::getBool(params, "firing", false)) flags |= 0x01;
            if (TextParse::getBool(params, "flash", false)) flags |= 0x02;
            if (TextParse::getBool(params, "fading", false)) flags |= 0x04;
            if (TextParse::getBool(params, "heater", false)) flags |= 0x08;
            if (TextParse::getBool(params, "fan", false)) flags |= 0x10;
            if (TextParse::getBool(params, "spindown", false)) flags |= 0x20;
            payload[0] = flags;
            SerialProtocol::putU16LE(payload + 1, TextParse::getUInt(params, "fanOff", 0));
            SerialProtocol::putU16LE(payload + 3, TextParse::getUInt(params, "servo1", 1500));
            SerialProtocol::putU16LE(payload + 5, TextParse::getUInt(params, "servo2", 1500));
            SerialProtocol::putU16LE(payload + 7, TextParse::getUInt(params, "servo3", 1500));
            SerialProtocol::putU16LE(payload + 9, TextParse::getUInt(params, "rpm", 0));
            *payloadLen = 11;
            break;
        }
        
        case SerialProtocol::SFX_PKT_ERROR: {
            payload[0] = TextParse::getUInt(params, "code", 0);
            char msg[64];
            if (TextParse::getString(params, "msg", msg, sizeof(msg))) {
                size_t msgLen = strlen(msg);
                memcpy(payload + 1, msg, msgLen);
                *payloadLen = 1 + msgLen;
            } else {
                *payloadLen = 1;
            }
            break;
        }
        
        default:
            // No payload for simple commands
            break;
    }
    
    return true;
}

// ============================================================================
// TextParse Utilities
// ============================================================================

namespace TextParse {

static const char* findKey(const char* line, const char* key) {
    if (!line || !key) return nullptr;
    
    size_t keyLen = strlen(key);
    const char* p = line;
    
    while (*p) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        
        // Check if this position matches key
        if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
            return p + keyLen + 1;  // Return pointer to value
        }
        
        // Skip to next whitespace or end
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    
    return nullptr;
}

int getInt(const char* line, const char* key, int defaultValue) {
    const char* value = findKey(line, key);
    return value ? atoi(value) : defaultValue;
}

unsigned int getUInt(const char* line, const char* key, unsigned int defaultValue) {
    const char* value = findKey(line, key);
    return value ? (unsigned int)strtoul(value, nullptr, 10) : defaultValue;
}

long getLong(const char* line, const char* key, long defaultValue) {
    const char* value = findKey(line, key);
    return value ? strtol(value, nullptr, 10) : defaultValue;
}

unsigned long getULong(const char* line, const char* key, unsigned long defaultValue) {
    const char* value = findKey(line, key);
    return value ? strtoul(value, nullptr, 10) : defaultValue;
}

float getFloat(const char* line, const char* key, float defaultValue) {
    const char* value = findKey(line, key);
    return value ? atof(value) : defaultValue;
}

bool getBool(const char* line, const char* key, bool defaultValue) {
    const char* value = findKey(line, key);
    if (!value) return defaultValue;
    
    if (value[0] == '1' || value[0] == 't' || value[0] == 'T' || 
        strncmp(value, "on", 2) == 0 || strncmp(value, "ON", 2) == 0 ||
        strncmp(value, "yes", 3) == 0 || strncmp(value, "YES", 3) == 0) {
        return true;
    }
    return false;
}

bool getString(const char* line, const char* key, char* output, size_t maxLen) {
    const char* value = findKey(line, key);
    if (!value) return false;
    
    // Check for quoted string
    if (*value == '"') {
        value++;
        const char* end = strchr(value, '"');
        if (!end) return false;
        
        size_t len = min((size_t)(end - value), maxLen - 1);
        memcpy(output, value, len);
        output[len] = '\0';
        return true;
    }
    
    // Unquoted - read until whitespace
    size_t i = 0;
    while (value[i] && value[i] != ' ' && value[i] != '\t' && i < maxLen - 1) {
        output[i] = value[i];
        i++;
    }
    output[i] = '\0';
    return i > 0;
}

bool hasKey(const char* line, const char* key) {
    return findKey(line, key) != nullptr;
}

const char* getCommand(const char* line, char* output, size_t maxLen) {
    if (!line || !output) return nullptr;
    
    // Skip leading whitespace
    while (*line == ' ' || *line == '\t') line++;
    
    // Read command name (until whitespace or '=')
    size_t i = 0;
    while (line[i] && line[i] != ' ' && line[i] != '\t' && line[i] != '=' && i < maxLen - 1) {
        output[i] = line[i];
        i++;
    }
    output[i] = '\0';
    
    // Return pointer to rest of line
    return line + i;
}

} // namespace TextParse

// ============================================================================
// Protocol Type Mapping
// ============================================================================

namespace SerialProtocol {

const char* packetTypeToText(uint8_t type) {
    switch (type) {
        case SFX_PKT_INIT:        return TextCmd::INIT;
        case SFX_PKT_SHUTDOWN:    return TextCmd::SHUTDOWN;
        case SFX_PKT_KEEPALIVE:   return TextCmd::KEEPALIVE;
        case SFX_PKT_INIT_READY:  return TextCmd::READY;
        case SFX_PKT_STATUS:      return TextCmd::STATUS;
        case SFX_PKT_ERROR:       return TextCmd::ERROR;
        case SFX_PKT_ACK:         return TextCmd::ACK;
        case SFX_PKT_NACK:        return TextCmd::NACK;
        case SFX_PKT_REBOOT:      return TextCmd::REBOOT;
        case SFX_PKT_BOOTSEL:     return TextCmd::BOOTSEL;
        
        case GUNFX_PKT_TRIGGER_ON:      return TextCmd::TRIGGER_ON;
        case GUNFX_PKT_TRIGGER_OFF:     return TextCmd::TRIGGER_OFF;
        case GUNFX_PKT_SRV_SET:         return TextCmd::SERVO_SET;
        case GUNFX_PKT_SRV_SETTINGS:    return TextCmd::SERVO_CONFIG;
        case GUNFX_PKT_SRV_RECOIL_JERK: return TextCmd::SERVO_RECOIL_JERK;
        case GUNFX_PKT_SMOKE_HEAT:      return TextCmd::SMOKE_HEAT;
        
        default: return nullptr;
    }
}

uint8_t textToPacketType(const char* cmd) {
    if (!cmd) return 0;
    
    if (strcmp(cmd, TextCmd::INIT) == 0)        return SFX_PKT_INIT;
    if (strcmp(cmd, TextCmd::SHUTDOWN) == 0)    return SFX_PKT_SHUTDOWN;
    if (strcmp(cmd, TextCmd::KEEPALIVE) == 0)   return SFX_PKT_KEEPALIVE;
    if (strcmp(cmd, TextCmd::READY) == 0)       return SFX_PKT_INIT_READY;
    if (strcmp(cmd, TextCmd::STATUS) == 0)      return SFX_PKT_STATUS;
    if (strcmp(cmd, TextCmd::ERROR) == 0)       return SFX_PKT_ERROR;
    if (strcmp(cmd, TextCmd::ACK) == 0)         return SFX_PKT_ACK;
    if (strcmp(cmd, TextCmd::NACK) == 0)        return SFX_PKT_NACK;
    if (strcmp(cmd, TextCmd::REBOOT) == 0)      return SFX_PKT_REBOOT;
    if (strcmp(cmd, TextCmd::BOOTSEL) == 0)     return SFX_PKT_BOOTSEL;
    
    if (strcmp(cmd, TextCmd::TRIGGER_ON) == 0)  return GUNFX_PKT_TRIGGER_ON;
    if (strcmp(cmd, TextCmd::TRIGGER_OFF) == 0) return GUNFX_PKT_TRIGGER_OFF;
    if (strcmp(cmd, TextCmd::SERVO_SET) == 0)   return GUNFX_PKT_SRV_SET;
    if (strcmp(cmd, TextCmd::SERVO_CONFIG) == 0) return GUNFX_PKT_SRV_SETTINGS;
    if (strcmp(cmd, TextCmd::SERVO_RECOIL_JERK) == 0)  return GUNFX_PKT_SRV_RECOIL_JERK;
    if (strcmp(cmd, TextCmd::SMOKE_HEAT) == 0)  return GUNFX_PKT_SMOKE_HEAT;
    
    return 0;  // Unknown
}

} // namespace SerialProtocol
