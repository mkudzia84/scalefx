/*
 * Slave Server Implementation (ESP32-S3)
 *
 * Handles:
 *   1. Auto-routing of slave-range packets by packet-type range:
 *        GunFX        0x01-0x2F
 *        LightFX      0x40-0x5F
 *        GearControl  0x60-0x7F
 *      Forwards verbatim to the matching attached slave; relays the typed
 *      RESP / ACK / NACK back upstream with the original correlation tag.
 *   2. Slave management commands (0x80-0x83) — list, init, status
 *   3. Slave info query (0xAE) and registry enumeration (0xB1)
 *   4. Per-slot async pumps that forward unsolicited slave packets verbatim
 *      with TAG_ASYNC (slave-range packets only — slaves' core STATUS
 *      broadcasts are aggregated by the hub and not re-emitted).
 *
 * See instructions/13-PASSTHROUGH-ROUTING.md.
 */

#include "slave_server.h"
#include <serial/client/bus_client.h>
#include <usb/sfx_usb_host.h>
#include <platform/diag_log.h>

#define SLAVE_LOG(fmt, ...) SFX_LOG_DEBUG("[SlaveSrv] " fmt, ##__VA_ARGS__)

using namespace CoreProtocol;

// ============================================================================
// tryProcess Override — Auto-route slave-range, handle management commands
// ============================================================================

CommandHandleResult SlaveServer::tryProcess(uint8_t type, const uint8_t* payload, size_t len) {
    // 1. Auto-route slave-range packets by type to the matching attached slave.
    if (slaveTypeForPacketType(type) != SlaveType::Unknown) {
        return forwardToSlave(type, payload, len);
    }

    // 2. Slave info query (0xAE)
    if (type == HubFxPacket::SLAVE_INFO) {
        handleSlaveInfo(payload, len);
        return CommandHandleResult::Handled;
    }

    // 3. Slave registry enumeration (0xB1)
    if (type == HubFxPacket::SLAVE_ENUM_REQ) {
        handleSlaveEnum();
        return CommandHandleResult::Handled;
    }

    // 4. Fall through to BusServer base for management commands (0x80-0x83)
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
// Auto-routing — forward slave-range packet to the matching attached slave
// ============================================================================

CommandHandleResult SlaveServer::forwardToSlave(uint8_t type, const uint8_t* payload, size_t len) {
    SlaveType target = slaveTypeForPacketType(type);
    BusClient* client = registry().getClient(target);

    if (!client) {
        SLAVE_LOG("Auto-route 0x%02X (%s): not connected", type, slaveTypeName(target));
        sendNack(HubFxError::SLAVE_NOT_CONNECTED);
        return CommandHandleResult::Handled;
    }

    SLAVE_LOG("Auto-route 0x%02X → %s len=%u", type, slaveTypeName(target), (unsigned)len);

    // Forward verbatim; sendCommand blocks until ACK / NACK / typed RESP.
    CommandResult result = client->sendCommand(type, payload, len);

    if (result.success) {
        // If the slave returned a typed RESP, BusClient buffered it as the last
        // non-base response. Forward that packet upstream verbatim with the
        // original correlation tag. Otherwise reply with a plain ACK.
        uint8_t respType = client->lastResponseType();
        size_t  respLen  = client->lastResponseLen();
        if (respType != 0 && respType != type) {
            sendRawPacket(respType, currentTag(), client->lastResponsePayload(), respLen);
        } else if (respType != 0 && respLen > 0) {
            // Same-type echo (rare) — still forward.
            sendRawPacket(respType, currentTag(), client->lastResponsePayload(), respLen);
        } else {
            sendAck();
        }
    } else {
        SLAVE_LOG("Auto-route 0x%02X (%s) failed: 0x%02X %s", type, slaveTypeName(target),
                  result.errorCode, result.errorMessage ? result.errorMessage : "");
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

// ============================================================================
// Slave Registry Enumeration (0xB1)
// ============================================================================

void SlaveServer::handleSlaveEnum() {
    // Wire format: [count:u8] then per slot:
    //   [slot:u8][type:u8][connected:u8][ready:u8][nameLen:u8][name:str]
    uint8_t buf[256];
    size_t pos = 0;
    uint8_t count = registry().count();
    buf[pos++] = count;

    for (uint8_t slot = 0; slot < count; ++slot) {
        const SlaveEntry& e = registry()[slot];
        const char* name = "";
        if (e.client && e.client->isServerReady()) name = e.client->serverName();
        uint8_t nameLen = (uint8_t)strlen(name);

        if (pos + 5 + nameLen > sizeof(buf)) break;  // safety

        buf[pos++] = slot;
        buf[pos++] = (uint8_t)e.type;
        buf[pos++] = e.connected ? 1 : 0;
        buf[pos++] = e.ready ? 1 : 0;
        buf[pos++] = nameLen;
        if (nameLen) { memcpy(&buf[pos], name, nameLen); pos += nameLen; }
    }

    SLAVE_LOG("SLAVE_ENUM_RESP: %u slots, %u bytes", count, (unsigned)pos);
    sendRawPacket(HubFxPacket::SLAVE_ENUM_RESP, currentTag(), buf, pos);
}

// ============================================================================
// Async Pumps — verbatim forward of slave-range async packets upstream
// ============================================================================

void SlaveServer::wireAsyncPumps() {
    // For every registered slave, install an async-packet listener that
    // re-emits unsolicited slave-range packets upstream verbatim with
    // TAG_ASYNC. Slave-internal CorePacket broadcasts (STATUS at 1 Hz, etc.)
    // are intentionally NOT forwarded — the hub aggregates board-level state
    // through SLAVE_INFO / SLAVE_LIST_RESP. Idempotent: rebinding overwrites
    // the previous closure.
    uint8_t count = registry().count();
    for (uint8_t slot = 0; slot < count; ++slot) {
        SlaveEntry& e = registry()[slot];
        if (!e.client) continue;
        e.client->onAsyncPacket([this](uint8_t type,
                                       const uint8_t* payload, size_t len) {
            // Only forward slave-range packets; their type alone identifies
            // the source board to the upstream client.
            if (slaveTypeForPacketType(type) == SlaveType::Unknown) return;
            sendRawPacket(type, CoreProtocol::TAG_ASYNC, payload, len);
        });
    }
}
