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

    // 2. Fall through to BusServer base for management commands (0x80-0x83)
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
    unsigned long start = millis();
    while (!slave->client->isServerReady() && (millis() - start < 3000)) {
        slave->client->process();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (slave->client->isServerReady()) {
        registry().setReady(type, true);
        SLAVE_LOG("SLAVE_INIT OK: %s → %s", slaveTypeName(type), slave->client->serverName());
        sendAck();
    } else {
        SLAVE_LOG("SLAVE_INIT TIMEOUT: %s", slaveTypeName(type));
        sendNack(HubFxError::SLAVE_INIT_FAILED);
    }
}
