/*
 * Slave Server Implementation
 *
 * Handles:
 *   1. Slave routing via subcmd pattern (0x96-0x98) — extracts subcmd byte
 *      from payload and forwards to appropriate BusClient
 *   2. Slave management commands (0x80-0x83) — list, init, status
 *   3. USB host diagnostics (0xA7-0xA8) — list connected USB CDC devices
 */

#include "slave_server.h"
#include <usb/usb_host.h>

using namespace CoreProtocol;

// ============================================================================
// tryProcess Override — Handle routing subcmds and management commands
// ============================================================================

CommandHandleResult SlaveServer::tryProcess(uint8_t type, const uint8_t* payload, size_t len) {
    // 1. Check if this is a SLAVE_ROUTE_* packet (0x96-0x98) → route via subcmd
    if (type >= HubFxPacket::SLAVE_ROUTE_GUNFX && type <= HubFxPacket::SLAVE_ROUTE_GEARCONTROL) {
        return routeToSlave(type, payload, len);
    }

    // 2. Check if this is a USB host diagnostics packet (0xA7-0xA8)
    if (type == HubFxPacket::USB_DEVICES_REQ) {
        handleUsbDevices();
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
        sendNack(HubFxError::SLAVE_NOT_CONNECTED);
        return CommandHandleResult::Handled;
    }

    // Forward the subcmd + payload to the slave as a normal command
    CommandResult result = client->sendCommand(subcmd, slavePayload, slavePayloadLen);
    if (result.success) {
        sendAck();
    } else {
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

        if (pos > sizeof(buf) - 40) break;
    }

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
        busy_wait_ms(1);
    }

    if (slave->client->isServerReady()) {
        registry().setReady(type, true);
        sendAck();
    } else {
        sendNack(HubFxError::SLAVE_INIT_FAILED);
    }
}

// ============================================================================
// USB Host Diagnostics
// ============================================================================

void SlaveServer::handleUsbDevices() {
    UsbHost& usb = UsbHost::instance();

    // Response: [initialized:u8][taskRunning:u8]
    //           [backendLen:u8][backend:str]
    //           [deviceCount:u8]
    //           per-device: [addr:u8][vid:u16LE][pid:u16LE][state:u8][slaveType:u8]
    uint8_t buf[128];
    size_t pos = 0;

    buf[pos++] = usb.isInitialized() ? 1 : 0;
    buf[pos++] = usb.isTaskRunning() ? 1 : 0;

    const char* backend = usb.backendName();
    uint8_t backendLen = (uint8_t)strlen(backend);
    buf[pos++] = backendLen;
    memcpy(&buf[pos], backend, backendLen);
    pos += backendLen;

    int cdcCount = usb.cdcDeviceCount();
    buf[pos++] = (uint8_t)cdcCount;

    for (int i = 0; i < cdcCount && pos + 8 <= sizeof(buf); i++) {
        const CdcDeviceInfo* dev = usb.getCdcDevice(i);
        if (!dev) continue;

        buf[pos++] = dev->dev_addr;
        CoreProtocol::putU16LE(&buf[pos], dev->vid);
        pos += 2;
        CoreProtocol::putU16LE(&buf[pos], dev->pid);
        pos += 2;
        buf[pos++] = (uint8_t)dev->state;

        // Cross-reference with slave registry to find slave type for this USB index
        const SlaveEntry* slave = registry().findByUsbIndex(i);
        buf[pos++] = slave ? (uint8_t)slave->type : 0;
    }

    sendRawPacket(HubFxPacket::USB_DEVICES_RESP, currentTag(), buf, pos);
}
