/*
 * Engine Server — Command Handler for Engine FX Control
 *
 * Handles engine FX packets (0x8C-0x8F):
 *   - Start/stop engine effects
 *   - Engine status query
 *
 * Registered as a separate handler in SfxServer's CommandRouter,
 * keeping engine FX concerns isolated from other HubFX domains.
 */

#ifndef ENGINE_SERVER_H
#define ENGINE_SERVER_H

#include <Arduino.h>
#include <serial/core/bus_server.h>

#include "../board_manager/hubfx_protocol.h"

// Forward declaration — avoids coupling to engine_fx.h
class EngineFX;

// ============================================================================
// EngineServer — ICommandHandler for Engine FX
// ============================================================================

class EngineServer : public BusServer {
public:
    EngineServer() = default;

    const char* handlerName() const override { return "EngineServer"; }

    void setEngineFX(EngineFX* engine) { _engine = engine; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override  { return 0x8C; }
    uint8_t moduleRangeHigh() const override { return 0x8F; }
    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

private:
    void handleStart();
    void handleStop();
    void handleStatusReq();

    EngineFX* _engine = nullptr;
};

#endif // ENGINE_SERVER_H
