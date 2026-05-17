/*
 * Engine Protocol Server — Implementation
 *
 * Handles ENGINE_START, ENGINE_STOP, ENGINE_STATUS_REQ commands.
 * All commands are "instant" category — ACK/NACK via SFX_DISPATCH.
 * Status request returns ENGINE_STATUS_RESP (query category — implicit ACK).
 */

#include "engine_server.h"
#include <platform/diag_log.h>

// ============================================================================
// Packet Dispatch
// ============================================================================

CommandHandleResult EngineServicePolicy::handle(uint8_t type,
                                                      const uint8_t* payload,
                                                      size_t len) {
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
// Shared Guard
// ============================================================================

bool EngineServicePolicy::requireEngine(const char* action) {
    if (!engine().isInitialized()) {
        SFX_LOG_WARN("[EngineServer] %s — engine not available", action);
        sendNack(HubFxError::ENGINE_NOT_AVAILABLE, "Engine not initialized");
        return false;
    }
    return true;
}

// ============================================================================
// ENGINE_START (0x8C) — Force-start the engine sound effect
// ============================================================================

void EngineServicePolicy::handleStart() {
    if (!requireEngine("START")) return;

    if (!engine().isEnabled()) {
        SFX_LOG_WARN("[EngineServer] START — engine disabled in config");
        sendNack(HubFxError::ENGINE_NOT_AVAILABLE, "Engine disabled");
        return;
    }

    SFX_LOG_INFO("[EngineServer] ENGINE_START");
    engine().forceStart();
    sendAck();
}

// ============================================================================
// ENGINE_STOP (0x8D) — Force-stop the engine sound effect
// ============================================================================

void EngineServicePolicy::handleStop() {
    if (!requireEngine("STOP")) return;

    SFX_LOG_INFO("[EngineServer] ENGINE_STOP");
    engine().forceStop();
    sendAck();
}

// ============================================================================
// ENGINE_STATUS_REQ (0x8E) → ENGINE_STATUS_RESP (0x8F)
// ============================================================================
// Response: [state:u8][toggleEngaged:u8][active:u8]

void EngineServicePolicy::handleStatusReq() {
    auto& eng = engine();
    uint8_t resp[3] = {0};

    if (eng.isInitialized()) {
        resp[0] = static_cast<uint8_t>(eng.state());
        resp[1] = eng.isToggleEngaged() ? 1 : 0;
        resp[2] = eng.isActive() ? 1 : 0;
    }
    // If uninitialized, all zeros = Stopped/no toggle/not active

    sendRawPacket(HubFxPacket::ENGINE_STATUS_RESP, currentTag(), resp, sizeof(resp));
    SFX_LOG_DEBUG("[EngineServer] STATUS_RESP: state=%s toggle=%d active=%d",
                  engineStateString(eng.state()), resp[1], resp[2]);
}
