/*
 * Slave Server — Auto-routing + Slave Management Handler
 *
 * Handles HubFX slave-related packets:
 *   - Slave management (0x80-0x83): list, init, status
 *   - Slave info query (0xAE)
 *   - Slave registry enumeration (0xB1)
 *
 * Inbound slave-range packets are auto-routed by packet-type range to the
 * matching attached slave — no envelope wrapping needed:
 *
 *     GunFX        0x01-0x2F
 *     LightFX      0x40-0x5F
 *     GearControl  0x60-0x7F
 *
 * The slave's response (typed RESP, ACK, or NACK) is forwarded back upstream
 * verbatim with the original correlation tag. Async slave packets in slave
 * ranges are forwarded verbatim with TAG_ASYNC.
 *
 * See instructions/13-PASSTHROUGH-ROUTING.md.
 */

#ifndef SLAVE_SERVER_H
#define SLAVE_SERVER_H

#include <Arduino.h>
#include <serial/core/bus_server.h>
#include <serial/hubfx/hubfx.h>

#include "slave_registry.h"
#include "slave_manager.h"

// ============================================================================
// SlaveServer — Auto-routing + Slave Management Handler
// ============================================================================

class SlaveServer : public BusServer {
public:
    SlaveServer() = default;

    const char* handlerName() const override { return "SlaveServer"; }

    /**
     * @brief Override tryProcess to auto-route slave-range packets and
     *        handle slave management (0x80-0x83), slave info query (0xAE),
     *        and slave registry enumeration (0xB1).
     */
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override;

    /**
     * @brief Wire per-slot async pumps so unsolicited slave packets in slave
     *        ranges get re-emitted upstream verbatim with TAG_ASYNC.
     *
     * Call once after every slave is registered (typically end of setup()).
     * Idempotent — re-calling rebinds the callbacks to the latest slot indices.
     */
    void wireAsyncPumps();

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override  { return 0x80; }
    uint8_t moduleRangeHigh() const override { return 0x83; }
    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

private:
    /**
     * @brief Forward a slave-range packet to the matching attached slave and
     *        relay the response (typed RESP / ACK / NACK) back upstream.
     */
    CommandHandleResult forwardToSlave(uint8_t type, const uint8_t* payload, size_t len);

    void handleSlaveList();
    void handleSlaveEnum();
    void handleSlaveInit(const uint8_t* payload, size_t len);
    void handleSlaveInfo(const uint8_t* payload, size_t len);

    SlaveRegistry& registry() { return SlaveRegistry::instance(); }
};

#endif // SLAVE_SERVER_H
