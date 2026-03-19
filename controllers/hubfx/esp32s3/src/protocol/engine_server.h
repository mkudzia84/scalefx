/*
 * Engine Protocol Server — BusServer for Engine FX commands.
 *
 * Handles ENGINE_START (0x8C), ENGINE_STOP (0x8D), ENGINE_STATUS_REQ (0x8E).
 * Delegates to the EngineFX singleton for state machine control.
 *
 * Usage:
 *   EngineServer engineServer;
 *   engineServer.begin(&Serial);
 *   server.addModuleHandler(&engineServer);
 */

#ifndef ENGINE_SERVER_H
#define ENGINE_SERVER_H

#include <serial/serial.h>
#include <serial/hubfx/hubfx.h>
#include "../effects/engine_fx.h"

class EngineServer : public BusServer {
public:
    EngineServer() = default;

    const char* handlerName() const override { return "EngineServer"; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type,
                                           const uint8_t* payload,
                                           size_t len) override;

    uint8_t moduleRangeLow()  const override { return HubFxPacket::ENGINE_START; }
    uint8_t moduleRangeHigh() const override { return HubFxPacket::ENGINE_STATUS_RESP; }

    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

private:
    /// Get engine singleton
    EngineFX& engine() { return EngineFX::instance(); }

    /**
     * @brief Guard: check engine is initialized. Sends NACK if not.
     * @param action  Log label (e.g. "START", "STOP")
     * @return true if engine is available, false if NACK was sent
     */
    bool requireEngine(const char* action);

    // ---- Command handlers ----
    void handleStart();
    void handleStop();
    void handleStatusReq();
};

#endif // ENGINE_SERVER_H
