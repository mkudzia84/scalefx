/*
 * Slave Server — Command Handler for Slave Routing and Management
 *
 * Handles HubFX slave-related packets:
 *   - Slave management (0x80-0x83): list, init, status
 *   - Slave routing via subcmd pattern (0x96-0x98):
 *       SLAVE_ROUTE_GUNFX       [subcmd:u8][payload...] → GunFX
 *       SLAVE_ROUTE_LIGHTFX     [subcmd:u8][payload...] → LightFX
 *       SLAVE_ROUTE_GEARCONTROL [subcmd:u8][payload...] → GearControl
 *
 * The subcmd byte is the original slave packet type. The hub extracts it
 * and forwards [subcmd + payload] to the appropriate BusClient.
 *
 * This design frees packet types 0x01-0x7F for other uses, collapsing all
 * slave routing into just 3 HubFX packet types.
 */

#ifndef SLAVE_SERVER_H
#define SLAVE_SERVER_H

#include <Arduino.h>
#include <serial_bus_server.h>

#include "hubfx_protocol.h"
#include "slave_registry.h"

// ============================================================================
// SlaveServer — Routing + Slave Management Handler
// ============================================================================

/**
 * @brief Server-side handler for slave routing and management
 *
 * Handles two non-contiguous sub-ranges via tryProcess() override:
 *   - Slave management: 0x80-0x83 (moduleRangeLow/High)
 *   - Slave routing:    0x96-0x98 (SLAVE_ROUTE_* subcmd packets)
 *
 * For SLAVE_ROUTE_* packets, the first payload byte is the subcmd (the
 * original slave packet type), followed by the original payload.
 */
class SlaveServer : public BusServer {
public:
    SlaveServer() = default;

    const char* handlerName() const override { return "SlaveServer"; }

    /**
     * @brief Override tryProcess to handle slave management (0x80-0x83)
     *        AND slave routing subcmd packets (0x96-0x98)
     */
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override;

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override  { return 0x80; }
    uint8_t moduleRangeHigh() const override { return 0x83; }
    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

private:
    CommandHandleResult routeToSlave(uint8_t type, const uint8_t* payload, size_t len);
    void handleSlaveList();
    void handleSlaveInit(const uint8_t* payload, size_t len);

    SlaveRegistry& registry() { return SlaveRegistry::instance(); }
};

#endif // SLAVE_SERVER_H
