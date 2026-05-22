/*
 * lightfx_service.ipp — template-method definitions.
 */

#ifndef HUBFX_LIGHTFX_SERVICE_IPP
#define HUBFX_LIGHTFX_SERVICE_IPP

namespace hubfx::effects::lightfx {

// ─── Lifecycle ──────────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
bool LightFxEffectServicePolicyT<TTopology, TLandingService>::begin(
        sfx_core::BoardServerBase* ctx) {
    _ctx = ctx;
    if (!_ctx) return false;

    _topology = ctx->template findPolicy<TTopology>();
    _landing  = ctx->template findPolicy<TLandingService>();
    if (!_topology || !_landing) {
        SFX_LOG_ERROR("[lightfx-svc] missing TopologyService or LandingLightService in policy pack");
        return false;
    }

    _controller.bind(_topology, _landing);
    // NOTE: do NOT claimPorts() here — programs aren't loaded until the
    // config chain calls configure() later in setup().  configure()
    // claims the ports once the program list exists.
    SFX_LOG_INFO("[lightfx-svc] ready (%u programs, master=%u%%)",
                 (unsigned)_controller.numPrograms(),
                 (unsigned)_controller.masterBrightnessPct());
    return true;
}

// ─── Wire dispatch ──────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
CommandHandleResult
LightFxEffectServicePolicyT<TTopology, TLandingService>::handle(
        uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case LightFxPacket::LIGHTFX_PROGRAM_LIST_REQ:
            handleProgramListReq();
            return CommandHandleResult::Handled;
        case LightFxPacket::LIGHTFX_PROGRAM_SELECT:
            handleProgramSelect(payload, len);
            return CommandHandleResult::Handled;
        case LightFxPacket::LIGHTFX_PROGRAM_RESET:
            handleProgramReset();
            return CommandHandleResult::Handled;
        case LightFxPacket::LIGHTFX_STATUS_REQ:
            handleStatusReq();
            return CommandHandleResult::Handled;
        case LightFxPacket::LIGHTFX_MASTER_BRIGHTNESS_SET:
            handleMasterBrightnessSet(payload, len);
            return CommandHandleResult::Handled;
        default:
            return CommandHandleResult::NotMyCommand;
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void LightFxEffectServicePolicyT<TTopology, TLandingService>::
        handleProgramListReq() {
    uint8_t buf[1 + kMaxPrograms * (1 + 1 + kProgramNameMax)];
    size_t  off = 1;
    const uint8_t n = _controller.numPrograms();
    buf[0] = n;
    for (uint8_t i = 0; i < n; ++i) {
        const Program* p = _controller.programAt(i);
        if (!p) continue;
        if (off + 2 + kProgramNameMax > sizeof(buf)) break;
        buf[off++] = i;
        uint8_t nlen = (uint8_t)std::strlen(p->name);
        if (nlen > kProgramNameMax - 1) nlen = kProgramNameMax - 1;
        buf[off++] = nlen;
        std::memcpy(&buf[off], p->name, nlen);
        off += nlen;
    }
    _ctx->sendRawPacket(LightFxPacket::LIGHTFX_PROGRAM_LIST_RESP,
                        _ctx->currentTag(), buf, off);
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void LightFxEffectServicePolicyT<TTopology, TLandingService>::
        handleProgramSelect(const uint8_t* p, size_t len) {
    if (len < 1) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    if (!_controller.selectProgram(p[0])) {
        _ctx->sendNack(LightFxError::UNKNOWN_PROGRAM);
        return;
    }
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void LightFxEffectServicePolicyT<TTopology, TLandingService>::
        handleProgramReset() {
    _controller.reset();
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void LightFxEffectServicePolicyT<TTopology, TLandingService>::
        handleStatusReq() {
    uint8_t buf[2];
    const int8_t ai = _controller.activeIndex();
    buf[0] = (ai < 0) ? kLightFxNoActiveProgram : static_cast<uint8_t>(ai);
    buf[1] = _controller.masterBrightnessPct();
    _ctx->sendRawPacket(LightFxPacket::LIGHTFX_STATUS_RESP,
                        _ctx->currentTag(), buf, sizeof(buf));
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void LightFxEffectServicePolicyT<TTopology, TLandingService>::
        handleMasterBrightnessSet(const uint8_t* p, size_t len) {
    if (len < 1) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    const uint8_t pct = p[0];
    if (pct > 100) {
        _ctx->sendNack(LightFxError::BRIGHTNESS_RANGE);
        return;
    }
    _controller.setMasterBrightness(pct);
    _ctx->sendAck();
}

}  // namespace hubfx::effects::lightfx

#endif  // HUBFX_LIGHTFX_SERVICE_IPP
