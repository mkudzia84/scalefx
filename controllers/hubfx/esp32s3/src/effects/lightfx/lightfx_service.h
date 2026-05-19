/*
 * lightfx_service.h — `LightFxEffectServicePolicyT<TTopology, TLandingService>`.
 *
 *   System-service policy slotted into the HubFX `BoardOf<...>` pack.
 *   Owns a `LightControllerT<...>` and dispatches the LIGHTFX_*
 *   wire surface (0xB7..0xBD) to it.
 *
 *   No new capability bit — relies on the parent `TOPOLOGY` flag.
 *   Programs and landing lights are master-side concepts; expanders
 *   don't need to know about either.
 */

#ifndef HUBFX_LIGHTFX_SERVICE_H
#define HUBFX_LIGHTFX_SERVICE_H

#include <cstdint>
#include <cstring>

#include <serial/core/core.h>
#include <serial/diag_log.h>
#include <server/board_server.h>

#include "../effect_id.h"
#include "../../topology/topology_service.h"            // TopologyService concept
#include "../landing_lights/landing_light_service.h"    // LandingLightService concept
#include "led_program.h"
#include "light_controller.h"
#include "lightfx_protocol.h"

namespace hubfx::effects::lightfx {

template <hubfx::topology::TopologyService               TTopology,
          hubfx::effects::landing::LandingLightService   TLandingService>
class LightFxEffectServicePolicyT {
public:
    static constexpr uint32_t kCapabilityBits = 0u;

    LightFxEffectServicePolicyT() = default;

    /// Install the static program list before `board.begin()`.
    void configure(const Program* programs, uint8_t count) {
        _controller.configure(programs, count);
    }

    /// Direct access for board sketches that want to drive the
    /// controller programmatically (e.g. a startup-program selection).
    LightControllerT<TTopology, TLandingService>& controller() {
        return _controller;
    }

    // ── SystemServicePolicy surface ──────────────────────────────────

    bool begin(sfx_core::BoardServerBase* ctx);

    bool ownsType(uint8_t type) const {
        return type == LightFxPacket::LIGHTFX_PROGRAM_LIST_REQ
            || type == LightFxPacket::LIGHTFX_PROGRAM_SELECT
            || type == LightFxPacket::LIGHTFX_PROGRAM_RESET
            || type == LightFxPacket::LIGHTFX_STATUS_REQ
            || type == LightFxPacket::LIGHTFX_MASTER_BRIGHTNESS_SET;
    }

    CommandHandleResult handle(uint8_t type,
                               const uint8_t* payload, size_t len);

    void update() {}

    const char* getErrorMessage(uint8_t code) const {
        return LightFxError::getMessage(code);
    }

private:
    void handleProgramListReq      ();
    void handleProgramSelect       (const uint8_t* p, size_t len);
    void handleProgramReset        ();
    void handleStatusReq           ();
    void handleMasterBrightnessSet (const uint8_t* p, size_t len);

    sfx_core::BoardServerBase*                       _ctx        = nullptr;
    TTopology*                                       _topology   = nullptr;
    TLandingService*                                 _landing    = nullptr;
    LightControllerT<TTopology, TLandingService>     _controller;
};

}  // namespace hubfx::effects::lightfx

#include "lightfx_service.ipp"

#endif  // HUBFX_LIGHTFX_SERVICE_H
