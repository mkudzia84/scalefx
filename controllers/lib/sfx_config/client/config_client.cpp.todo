/*
 * Config Client — Protocol Implementation
 *
 * ConfigClient command methods and CONFIG_STATUS_RESP parsing.
 */

#include "config_client.h"

using namespace CoreProtocol;

// ============================================================================
// Module Packet Handler (CONFIG_STATUS_RESP parsing)
// ============================================================================

void ConfigClient::onModulePacket(uint8_t type, uint8_t tag,
                                   const uint8_t* payload, size_t len) {
    switch (type) {
        case HubFxPacket::CONFIG_STATUS_RESP: {
            // Wire: [loaded:u8][fileSize:u16LE][validOk:u8]
            _lastInfo = {};
            if (len >= 4) {
                _lastInfo.loaded   = payload[0] != 0;
                _lastInfo.fileSize = getU16LE(&payload[1]);
                // validOk at payload[3] — extend HubFxConfigInfo if needed
            } else if (len >= 3) {
                // Backward compat: original format without validOk
                _lastInfo.loaded   = payload[0] != 0;
                _lastInfo.fileSize = getU16LE(&payload[1]);
            }

            // Query response is implicit ACK — resolve the pending tag
            if (tag != CoreProtocol::TAG_ASYNC) {
                _resultQueue.resolve(tag, CommandResult::Ack());
            }
            if (_configCallback) _configCallback(_lastInfo);
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Commands
// ============================================================================

CommandResult ConfigClient::configReload(const char* path) {
    if (!path) {
        // No payload — server uses its default path
        return sendCommand(HubFxPacket::CONFIG_RELOAD, nullptr, 0);
    }

    // Send path in payload: [pathLen:u8][path:str]
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    return sendCommand(HubFxPacket::CONFIG_RELOAD, payload, 1 + pathLen);
}

CommandResult ConfigClient::configStatus() {
    return sendCommand(HubFxPacket::CONFIG_STATUS, nullptr, 0);
}
