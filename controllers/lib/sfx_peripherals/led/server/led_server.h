/*
 * LED Protocol Server — Shared LED Command Handler
 *
 * Handles the 13 LED protocol commands (LED_SET through LED_ENABLE) from
 * the LightFX packet range (0x40-0x4C).  Delegates all channel management
 * to an ILedManager instance.
 *
 * Designed to be either:
 *   - Used standalone (for boards that only have LEDs)
 *   - Extended by LightFxServer (which adds servo + landing light commands)
 *
 * The LED protocol uses the LightFxPacket constants and LightFxError codes
 * defined in <serial/lightfx/lightfx.h>.
 *
 * Response packets sent:
 *   LED_SEQ_STATUS_RESP (0x5A) — sequence status query response
 *   LED_STATUS_RESP (0x5B)     — all-channel status query response
 *   LED_SEQ_QUEUE_RESP (0x5D)  — sequence queue detail response
 *
 * Usage (standalone):
 *   LedManager<6, ExpanderLedDriver> mgr;
 *   LedProtocolServer ledServer;
 *   ledServer.setLedManager(&mgr);
 *   ledServer.begin(&Serial);
 *   router.addHandler(&ledServer);
 *
 * Usage (as base class — LightFxServer):
 *   class LightFxServer : public LedProtocolServer {
 *       CommandHandleResult handleModulePacket(...) override {
 *           auto r = LedProtocolServer::handleModulePacket(...);
 *           if (r != CommandHandleResult::NotMyCommand) return r;
 *           // handle servo / landing light ...
 *       }
 *       uint8_t moduleRangeHigh() const override { return 0x5F; }
 *   };
 */

#ifndef LED_PROTOCOL_SERVER_H
#define LED_PROTOCOL_SERVER_H

#include <serial/lightfx/lightfx.h>
#include <serial/core/bus_server.h>

class ILedManager;

// ============================================================================
// LedProtocolServer Class
// ============================================================================

/**
 * @brief Shared LED command handler — processes 13 LED packet types.
 *
 * Extends BusServer for use with SfxServer + CommandRouter.
 * All LED operations are delegated to the attached ILedManager.
 */
class LedProtocolServer : public BusServer {
public:
    const char* handlerName() const override { return "LedProtocolServer"; }

    // ========================================================================
    // Manager Binding
    // ========================================================================

    /**
     * @brief Attach the LED channel manager
     *
     * Must be called before any packets are processed.
     */
    void setLedManager(ILedManager* manager) { _ledManager = manager; }

    ILedManager* ledManager() const { return _ledManager; }

    // ========================================================================
    // Response Methods (public — subclasses may also call these)
    // ========================================================================

    int sendSeqStatus(const LightFxSeqStatus& status);
    int sendSeqQueue(const LightFxSeqQueue& queue);
    int sendChannelStatus(const LightFxChannelStatus* channels, uint8_t count);

protected:
    // ========================================================================
    // BusServer Overrides
    // ========================================================================

    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;

    uint8_t moduleRangeLow()  const override { return 0x40; }
    uint8_t moduleRangeHigh() const override { return 0x4C; }

    const char* getModuleErrorMessage(uint8_t code) override {
        return LightFxError::getMessage(code);
    }

private:
    ILedManager* _ledManager = nullptr;
};

#endif // LED_PROTOCOL_SERVER_H
