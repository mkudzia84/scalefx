/*
 * Slave Server Implementation (ESP32-S3)
 *
 * Handles:
 *   1. Slave routing via subcmd pattern (0x96-0x98) — extracts subcmd byte
 *      from payload and forwards to appropriate BusClient
 *   2. Slave management commands (0x80-0x83) — list, init, status
 *
 * Ported from HubFX Pico reference (controllers/archive/hubfx-pico/).
 * Uses FreeRTOS vTaskDelay() instead of Pico busy_wait_ms().
 */

#include "slave_server.h"
#include <serial/client/bus_client.h>
#include <usb/sfx_usb_host.h>
#include <platform/diag_log.h>

#define SLAVE_LOG(fmt, ...) SFX_LOG_DEBUG("[SlaveSrv] " fmt, ##__VA_ARGS__)

using namespace CoreProtocol;

// ============================================================================
// tryProcess Override — Handle routing subcmds and management commands
// ============================================================================

CommandHandleResult SlaveServer::tryProcess(uint8_t type, const uint8_t* payload, size_t len) {
    // 1. Check if this is a SLAVE_ROUTE_* packet (0x96-0x98) → route via subcmd
    if (type >= HubFxPacket::SLAVE_ROUTE_GUNFX && type <= HubFxPacket::SLAVE_ROUTE_GEARCONTROL) {
        return routeToSlave(type, payload, len);
    }

    // 2. Check slave info query (0xAE)
    if (type == HubFxPacket::SLAVE_INFO) {
        handleSlaveInfo(payload, len);
        return CommandHandleResult::Handled;
    }

    // 3. Fall through to BusServer base for management commands (0x80-0x83)
    return BusServer::tryProcess(type, payload, len);
}

// ============================================================================
// handleModulePacket — Slave Management Commands (0x80-0x83)
// ============================================================================

CommandHandleResult SlaveServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case HubFxPacket::SLAVE_LIST:
            handleSlaveList();
            return CommandHandleResult::Handled;

        case HubFxPacket::SLAVE_INIT:
            handleSlaveInit(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::SLAVE_STATUS:
            sendAck();  // Status comes via core STATUS callback
            return CommandHandleResult::Handled;

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// Slave Command Routing (subcmd pattern)
// ============================================================================

CommandHandleResult SlaveServer::routeToSlave(uint8_t type, const uint8_t* payload, size_t len) {
    // Extract subcmd from first payload byte
    if (len < 1) {
        sendNack(SerialError::MISSING_PARAMETER);
        return CommandHandleResult::Handled;
    }

    uint8_t subcmd = payload[0];
    const uint8_t* slavePayload = (len > 1) ? &payload[1] : nullptr;
    size_t slavePayloadLen = (len > 1) ? len - 1 : 0;

    // Determine target slave from the SLAVE_ROUTE_* packet type
    SlaveType target = slaveTypeForRoutePacket(type);
    BusClient* client = registry().getClient(target);

    if (!client) {
        SLAVE_LOG("Route %s: not connected", slaveTypeName(target));
        sendNack(HubFxError::SLAVE_NOT_CONNECTED);
        return CommandHandleResult::Handled;
    }

    // Core-range subcmds (0xF0+) need special handling
    if (subcmd >= CorePacket::INIT) {
        return routeCoreToSlave(subcmd, client, target);
    }

    // Forward the subcmd + payload to the slave as a normal command
    SLAVE_LOG("Route → %s subcmd=0x%02X len=%d", slaveTypeName(target), subcmd, slavePayloadLen);
    CommandResult result = client->sendCommand(subcmd, slavePayload, slavePayloadLen);
    if (result.success) {
        sendAck();
    } else {
        SLAVE_LOG("Route %s failed: 0x%02X %s", slaveTypeName(target), result.errorCode,
                  result.errorMessage ? result.errorMessage : "");
        sendNack(result.errorCode, result.errorMessage);
    }

    return CommandHandleResult::Handled;
}

// ============================================================================
// Core Command Routing to Slaves
// ============================================================================

CommandHandleResult SlaveServer::routeCoreToSlave(uint8_t coreCmd, BusClient* client, SlaveType target) {
    SLAVE_LOG("CoreRoute → %s cmd=0x%02X", slaveTypeName(target), coreCmd);

    switch (coreCmd) {
        // --- Fire-and-forget commands (slave does NOT ACK) ---
        case CorePacket::REBOOT:
            client->sendReboot();
            registry().setReady(target, false);
            sendAck();
            return CommandHandleResult::Handled;

        case CorePacket::BOOTSEL:
            client->sendBootsel();
            registry().setReady(target, false);
            sendAck();
            return CommandHandleResult::Handled;

        // --- ACK-based commands (slave sends ACK/NACK) ---
        case CorePacket::SHUTDOWN: {
            CommandResult result = client->sendCommand(CorePacket::SHUTDOWN, nullptr, 0);
            if (result.success) {
                registry().setReady(target, false);
                sendAck();
            } else {
                sendNack(result.errorCode, result.errorMessage);
            }
            return CommandHandleResult::Handled;
        }

        case CorePacket::KEEPALIVE: {
            CommandResult result = client->sendCommand(CorePacket::KEEPALIVE, nullptr, 0);
            if (result.success) {
                sendAck();
            } else {
                sendNack(result.errorCode, result.errorMessage);
            }
            return CommandHandleResult::Handled;
        }

        // --- STATUS_REQ: forward raw STATUS response from slave to CLI ---
        case CorePacket::STATUS_REQ: {
            CommandResult result = client->sendCommand(CorePacket::STATUS_REQ, nullptr, 0);
            if (result.success) {
                // Forward the raw STATUS packet data from the slave
                if (client->lastResponseType() == CorePacket::STATUS && client->lastResponseLen() > 0) {
                    sendRawPacket(CorePacket::STATUS, currentTag(),
                                  client->lastResponsePayload(), client->lastResponseLen());
                } else {
                    sendAck();  // Fallback if no STATUS data captured
                }
            } else {
                sendNack(result.errorCode, result.errorMessage);
            }
            return CommandHandleResult::Handled;
        }

        default:
            SLAVE_LOG("CoreRoute %s: unsupported cmd 0x%02X", slaveTypeName(target), coreCmd);
            sendNack(SerialError::NOT_SUPPORTED);
            return CommandHandleResult::Handled;
    }
}

// ============================================================================
// Slave Management Commands
// ============================================================================

void SlaveServer::handleSlaveList() {
    if (registry().count() == 0) {
        uint8_t payload[1] = { 0 };
        sendRawPacket(HubFxPacket::SLAVE_LIST_RESP, currentTag(), payload, 1);
        return;
    }

    // Build response: [count:u8] + per-slave [type:u8][connected:u8][ready:u8][name_len:u8][name...]
    uint8_t buf[256];
    size_t pos = 0;

    buf[pos++] = registry().count();

    for (uint8_t i = 0; i < registry().count(); i++) {
        const SlaveEntry& slave = registry()[i];
        buf[pos++] = (uint8_t)slave.type;
        buf[pos++] = slave.connected ? 1 : 0;
        buf[pos++] = slave.ready ? 1 : 0;

        const char* name = "";
        if (slave.client && slave.client->isServerReady()) {
            name = slave.client->serverName();
        }
        uint8_t nameLen = (uint8_t)strlen(name);
        buf[pos++] = nameLen;
        if (nameLen > 0) {
            memcpy(&buf[pos], name, nameLen);
            pos += nameLen;
        }

        if (pos > sizeof(buf) - 40) break;  // Safety margin
    }

    SLAVE_LOG("SLAVE_LIST_RESP: %d slaves, %d bytes", registry().count(), pos);
    sendRawPacket(HubFxPacket::SLAVE_LIST_RESP, currentTag(), buf, pos);
}

void SlaveServer::handleSlaveInit(const uint8_t* payload, size_t len) {
    if (len < 1) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    SlaveType type = (SlaveType)payload[0];
    if (type == SlaveType::Unknown || (uint8_t)type >= (uint8_t)SlaveType::COUNT) {
        sendNack(SerialError::INVALID_PARAM);
        return;
    }

    SlaveEntry* slave = registry().find(type);
    if (!slave || !slave->client) {
        sendNack(HubFxError::SLAVE_NOT_FOUND);
        return;
    }

    if (!slave->connected) {
        sendNack(HubFxError::SLAVE_NOT_CONNECTED);
        return;
    }

    SLAVE_LOG("SLAVE_INIT: %s (usbIndex=%d)", slaveTypeName(type), slave->usbIndex);

    // Send INIT to the slave
    int sent = slave->client->sendInit();
    if (sent < 0) {
        sendNack(HubFxError::SLAVE_INIT_FAILED);
        return;
    }

    // Wait for INIT_READY (up to 3s)
    if (awaitSlaveReady(*slave->client)) {
        registry().setReady(type, true);
        SLAVE_LOG("SLAVE_INIT OK: %s → %s", slaveTypeName(type), slave->client->serverName());
        sendAck();
    } else {
        SLAVE_LOG("SLAVE_INIT TIMEOUT: %s", slaveTypeName(type));
        sendNack(HubFxError::SLAVE_INIT_FAILED);
    }
}

// ============================================================================
// Slave Info Query — Return cached boardInfo for a slave
// ============================================================================

void SlaveServer::handleSlaveInfo(const uint8_t* payload, size_t len) {
    if (len < 1) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    SlaveType type = (SlaveType)payload[0];
    if (type == SlaveType::Unknown || (uint8_t)type >= (uint8_t)SlaveType::COUNT) {
        sendNack(SerialError::INVALID_PARAM);
        return;
    }

    SlaveEntry* slave = registry().find(type);
    if (!slave) {
        sendNack(HubFxError::SLAVE_NOT_FOUND);
        return;
    }

    // Build SLAVE_INFO_RESP from cached BusClient boardInfo
    // Format: [slaveType:u8][ready:u8][connected:u8]
    //         [nameLen:u8][name][verLen:u8][ver][platLen:u8][plat]
    //         [cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]
    uint8_t buf[128];
    size_t pos = 0;

    buf[pos++] = (uint8_t)type;
    buf[pos++] = slave->ready ? 1 : 0;
    buf[pos++] = slave->connected ? 1 : 0;

    if (slave->client && slave->client->isServerReady()) {
        const BusClientBoardInfo& info = slave->client->boardInfo();

        // Name (length-prefixed)
        uint8_t nameLen = (uint8_t)strlen(info.deviceName);
        buf[pos++] = nameLen;
        if (nameLen > 0) {
            memcpy(&buf[pos], info.deviceName, nameLen);
            pos += nameLen;
        }

        // Version (length-prefixed)
        uint8_t verLen = (uint8_t)strlen(info.firmwareVersion);
        buf[pos++] = verLen;
        if (verLen > 0) {
            memcpy(&buf[pos], info.firmwareVersion, verLen);
            pos += verLen;
        }

        // Platform (length-prefixed)
        uint8_t platLen = (uint8_t)strlen(info.platform);
        buf[pos++] = platLen;
        if (platLen > 0) {
            memcpy(&buf[pos], info.platform, platLen);
            pos += platLen;
        }

        // CPU MHz, free RAM, build number (u32 LE)
        CoreProtocol::putU32LE(&buf[pos], info.cpuFrequencyMHz); pos += 4;
        CoreProtocol::putU32LE(&buf[pos], info.freeRamBytes);    pos += 4;
        CoreProtocol::putU32LE(&buf[pos], info.buildNumber);     pos += 4;
    } else {
        // No board info available — zero-length strings and zeroed fields
        buf[pos++] = 0;  // nameLen
        buf[pos++] = 0;  // verLen
        buf[pos++] = 0;  // platLen
        CoreProtocol::putU32LE(&buf[pos], 0); pos += 4;  // cpuMHz
        CoreProtocol::putU32LE(&buf[pos], 0); pos += 4;  // freeRam
        CoreProtocol::putU32LE(&buf[pos], 0); pos += 4;  // buildNum
    }

    SLAVE_LOG("SLAVE_INFO_RESP: %s ready=%d connected=%d %d bytes",
              slaveTypeName(type), slave->ready, slave->connected, pos);
    sendRawPacket(HubFxPacket::SLAVE_INFO_RESP, currentTag(), buf, pos);
}
