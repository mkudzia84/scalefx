/*
 * SerialInitHandler - Protocol Negotiation Implementation
 */

#include "serial_init.h"
#include <cstring>

// Namespace alias for TextCmd
namespace TextCmd = SerialProtocol::TextCmd;

// ============================================================================
// SerialInitHandler Implementation
// ============================================================================

bool SerialInitHandler::begin(Stream* serial, const char* moduleName) {
    if (!serial) return false;

    _serial = serial;
    _initialized = false;
    _protocolMode = ProtocolMode::Text;
    _lastActivityMs = 0;
    _keepaliveIntervalMs = 0;

    // Copy module name
    strncpy(_moduleName, moduleName, sizeof(_moduleName) - 1);
    _moduleName[sizeof(_moduleName) - 1] = '\0';

    // Copy to board info as well
    strncpy(_boardInfo.deviceName, moduleName, sizeof(_boardInfo.deviceName) - 1);
    _boardInfo.deviceName[sizeof(_boardInfo.deviceName) - 1] = '\0';

    return true;
}

void SerialInitHandler::setBoardInfo(const char* firmwareVersion, uint32_t buildNumber,
                                      const char* platform, uint32_t cpuFrequencyMHz,
                                      uint32_t freeRamBytes) {
    if (firmwareVersion) {
        // Strip "v" or "V" prefix if present
        const char* ver = firmwareVersion;
        if (ver[0] == 'v' || ver[0] == 'V') {
            ver++;
        }
        strncpy(_boardInfo.firmwareVersion, ver, sizeof(_boardInfo.firmwareVersion) - 1);
        _boardInfo.firmwareVersion[sizeof(_boardInfo.firmwareVersion) - 1] = '\0';
    }
    _boardInfo.buildNumber = buildNumber;
    if (platform) {
        strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
        _boardInfo.platform[sizeof(_boardInfo.platform) - 1] = '\0';
    }
    _boardInfo.cpuFrequencyMHz = cpuFrequencyMHz;
    _boardInfo.freeRamBytes = freeRamBytes;
}

void SerialInitHandler::reset() {
    _initialized = false;
    _protocolMode = ProtocolMode::Text;
    _keepaliveIntervalMs = 0;
    
    // Notify callback
    if (_initResetCallback) {
        _initResetCallback();
    }
}

CommandHandleResult SerialInitHandler::tryProcessCommand(const char* line) {
    if (!line || !_serial) return CommandHandleResult::NotMyCommand;

    // Update activity timestamp for any command we try to process
    _lastActivityMs = millis();

    // Check for INIT command
    if (strncmp(line, TextCmd::INIT, strlen(TextCmd::INIT)) == 0) {
        // Must be exactly "INIT" or "INIT " followed by args
        char nextChar = line[strlen(TextCmd::INIT)];
        if (nextChar == '\0' || nextChar == ' ') {
            const char* args = (nextChar == ' ') ? line + strlen(TextCmd::INIT) + 1 : nullptr;
            handleInit(args);
            return CommandHandleResult::Handled;
        }
    }
    
    // Check for SHUTDOWN command
    if (strcmp(line, TextCmd::SHUTDOWN) == 0) {
        if (_shutdownCallback) _shutdownCallback();
        return CommandHandleResult::Handled;
    }
    
    // Check for REBOOT command
    if (strcmp(line, TextCmd::REBOOT) == 0) {
        if (_rebootCallback) _rebootCallback();
        return CommandHandleResult::Handled;
    }
    
    // Check for BOOTSEL command
    if (strcmp(line, TextCmd::BOOTSEL) == 0) {
        if (_bootselCallback) _bootselCallback();
        return CommandHandleResult::Handled;
    }
    
    // Check for KEEPALIVE command (fire-and-forget, just updates activity timer)
    if (strcmp(line, TextCmd::KEEPALIVE) == 0) {
        // Activity timer already updated at start of this function
        return CommandHandleResult::Handled;
    }
    
    // Not a system command - pass to next handler
    return CommandHandleResult::NotMyCommand;
}

bool SerialInitHandler::checkTimeout(unsigned long timeoutMs) {
    if (timeoutMs == 0 || _lastActivityMs == 0) {
        return false;
    }
    
    unsigned long now = millis();
    if (now - _lastActivityMs > timeoutMs) {
        // Notify connection loss before reset
        if (_initialized && _connectionLossCallback) {
            _connectionLossCallback();
        }
        if (_initialized) {
            reset();
        }
        return true;
    }
    return false;
}

void SerialInitHandler::handleInit(const char* args) {
    // If already initialized, this is a reconnection
    if (_initialized) {
        reset();
    }

    // Parse protocol mode (default to text if not specified)
    _protocolMode = ProtocolMode::Text;
    _keepaliveIntervalMs = 0;  // Default: disabled
    
    if (args) {
        // Make a copy for parsing
        char argsCopy[64];
        strncpy(argsCopy, args, sizeof(argsCopy) - 1);
        argsCopy[sizeof(argsCopy) - 1] = '\0';
        
        // Look for protocol=xxx
        char protocol[16] = "";
        TextParse::getString(argsCopy, "protocol", protocol, sizeof(protocol));
        
        if (strcmp(protocol, "binary") == 0) {
            _protocolMode = ProtocolMode::Binary;
        } else if (strcmp(protocol, "text") == 0) {
            _protocolMode = ProtocolMode::Text;
        }
        // Unknown protocol value defaults to text
        
        // Look for keepalive=xxx (ms interval, or "off")
        char keepalive[16] = "";
        TextParse::getString(argsCopy, "keepalive", keepalive, sizeof(keepalive));
        
        if (keepalive[0] != '\0' && strcmp(keepalive, "off") != 0) {
            // Parse as integer (milliseconds)
            _keepaliveIntervalMs = atol(keepalive);
        }
    }

    // Send INIT_READY response (always text)
    sendInitReady();

    // Mark as initialized
    _initialized = true;

    // Notify callback
    if (_initCompleteCallback) {
        _initCompleteCallback(_protocolMode);
    }
}

void SerialInitHandler::sendInitReady() {
    if (!_serial) return;

    // Format: INIT_READY name=X version=Y build=N platform=Z cpuMHz=N ramBytes=N
    _serial->print(TextCmd::INIT_READY);
    _serial->print(" name=");
    _serial->print(_boardInfo.deviceName);
    _serial->print(" version=");
    _serial->print(_boardInfo.firmwareVersion);
    _serial->print(" build=");
    _serial->print(_boardInfo.buildNumber);
    _serial->print(" platform=");
    _serial->print(_boardInfo.platform);
    _serial->print(" cpuMHz=");
    _serial->print(_boardInfo.cpuFrequencyMHz);
    _serial->print(" ramBytes=");
    _serial->println(_boardInfo.freeRamBytes);
}

// ============================================================================
// SerialInitSender Implementation (Master Side)
// ============================================================================

int SerialInitSender::sendInit(Stream* serial, ProtocolMode mode, unsigned long keepaliveMs) {
    if (!serial) return -1;

    int len = 0;
    len += serial->print(TextCmd::INIT);
    len += serial->print(" protocol=");
    len += serial->print(mode == ProtocolMode::Binary ? "binary" : "text");
    
    // Add keepalive parameter
    len += serial->print(" keepalive=");
    if (keepaliveMs > 0) {
        len += serial->print(keepaliveMs);
    } else {
        len += serial->print("off");
    }
    
    len += serial->println();
    return len;
}

bool SerialInitSender::isInitReady(const char* line) {
    if (!line) return false;
    return strncmp(line, TextCmd::INIT_READY, strlen(TextCmd::INIT_READY)) == 0;
}

bool SerialInitSender::parseInitReady(const char* line, BoardInfo& info) {
    if (!line) return false;
    
    // Check prefix
    if (!isInitReady(line)) {
        return false;
    }

    // Get arguments after "INIT_READY"
    const char* args = line + strlen(TextCmd::INIT_READY);
    if (*args == ' ') args++;

    // Make a copy for parsing (TextParse modifies the string)
    char argsCopy[128];
    strncpy(argsCopy, args, sizeof(argsCopy) - 1);
    argsCopy[sizeof(argsCopy) - 1] = '\0';

    // Parse fields
    TextParse::getString(argsCopy, "name", info.deviceName, sizeof(info.deviceName));
    TextParse::getString(argsCopy, "version", info.firmwareVersion, sizeof(info.firmwareVersion));
    TextParse::getString(argsCopy, "platform", info.platform, sizeof(info.platform));
    
    int cpuMHz = 0, ramBytes = 0;
    cpuMHz = TextParse::getInt(argsCopy, "cpuMHz", 0);
    ramBytes = TextParse::getInt(argsCopy, "ramBytes", 0);
    info.cpuFrequencyMHz = cpuMHz;
    info.freeRamBytes = ramBytes;

    return true;
}
