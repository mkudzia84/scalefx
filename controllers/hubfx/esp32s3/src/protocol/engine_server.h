/*
 * EngineServicePolicy — system-service policy for Engine FX commands.
 *
 * Handles ENGINE_START (0x8C), ENGINE_STOP (0x8D), ENGINE_STATUS_REQ (0x8E).
 * Delegates to the EngineFX singleton for state machine control.
 *
 * Joined to a `BoardServer<...>` policy pack via SfxServer's UserPolicies.
 */

#ifndef ENGINE_SERVER_H
#define ENGINE_SERVER_H

#include <serial/serial.h>
#include <serial/hubfx/hubfx.h>
#include <serial/core/system_service.h>     // SystemServicePolicy + ServiceContext
#include "../effects/engine_fx.h"

class EngineServicePolicy {
public:
    /// Engine-FX capability bit.
    static constexpr uint32_t kCapabilityBits = CoreCapability::ENGINE;

    EngineServicePolicy() = default;

    // ── SystemServicePolicy surface ───────────────────────────────────

    bool begin(sfx_core::ServiceContext* ctx) {
        _ctx = ctx;
        return _ctx != nullptr;
    }

    bool ownsType(uint8_t type) const {
        return type >= HubFxPacket::ENGINE_START
            && type <= HubFxPacket::ENGINE_STATUS_RESP;
    }

    CommandHandleResult handle(uint8_t type, const uint8_t* payload, size_t len);

    void update() { /* EngineFX::process() is called from the board's loop */ }

    const char* getErrorMessage(uint8_t code) const {
        return HubFxError::getMessage(code);
    }

protected:
    // Wire-helper wrappers — see other policies.
    int     sendAck()                                                  { return _ctx->sendAck(); }
    int     sendNack(uint8_t errorCode, const char* reason = nullptr)  { return _ctx->sendNack(errorCode, reason); }
    int     sendRawPacket(uint8_t t, uint8_t tag, const uint8_t* p = nullptr, size_t l = 0)
                                                                       { return _ctx->sendRawPacket(t, tag, p, l); }
    uint8_t currentTag() const                                         { return _ctx->currentTag(); }

private:
    sfx_core::ServiceContext* _ctx = nullptr;

    /// Get engine singleton
    EngineFX& engine() { return EngineFX::instance(); }

    /**
     * @brief Guard: check engine is initialized. Sends NACK if not.
     */
    bool requireEngine(const char* action);

    void handleStart();
    void handleStop();
    void handleStatusReq();
};

/// @deprecated Alias for in-flight callers.
using EngineServer = EngineServicePolicy;

#endif // ENGINE_SERVER_H
