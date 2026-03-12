/*
 * Engine Server Implementation
 *
 * Handles engine FX commands (0x8C-0x8F):
 *   start, stop, status
 */

#include "engine_server.h"
#include "engine_fx.h"

using namespace CoreProtocol;

// ============================================================================
// handleModulePacket — Engine FX Commands (0x8C-0x8F)
// ============================================================================

CommandHandleResult EngineServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case HubFxPacket::ENGINE_START:
            handleStart();
            return CommandHandleResult::Handled;

        case HubFxPacket::ENGINE_STOP:
            handleStop();
            return CommandHandleResult::Handled;

        case HubFxPacket::ENGINE_STATUS_REQ:
            handleStatusReq();
            return CommandHandleResult::Handled;

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// Engine FX Handlers
// ============================================================================

void EngineServer::handleStart() {
    if (!_engine) {
        sendNack(HubFxError::ENGINE_NOT_AVAILABLE);
        return;
    }
    _engine->forceStart();
    sendAck();
}

void EngineServer::handleStop() {
    if (!_engine) {
        sendNack(HubFxError::ENGINE_NOT_AVAILABLE);
        return;
    }
    _engine->forceStop();
    sendAck();
}

void EngineServer::handleStatusReq() {
    if (!_engine) {
        sendNack(HubFxError::ENGINE_NOT_AVAILABLE);
        return;
    }

    // Response: [state:u8][toggleEngaged:u8][active:u8]
    uint8_t buf[3];
    buf[0] = (uint8_t)_engine->state();
    buf[1] = _engine->isToggleEngaged() ? 1 : 0;
    buf[2] = _engine->isActive() ? 1 : 0;

    sendRawPacket(HubFxPacket::ENGINE_STATUS_RESP, currentTag(), buf, 3);
}
