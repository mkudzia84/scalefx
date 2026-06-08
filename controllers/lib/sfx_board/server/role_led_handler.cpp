/*
 * LedRoleHandler implementation — bodies moved verbatim from the former
 * RoleServicePolicy LED handlers; the queue-done callback now goes through
 * the bound RoleEventEmitter.
 */

#include "role_led_handler.h"
#include "role_event_emitter.h"
#include "board_server.h"
#include <serial/roles.h>          // RolePacket
#include <serial/wire.h>           // SfxWire
#include <serial/core/core.h>      // SerialError / PortError / RoleError

#include <variant>

namespace sfx_core {

bool LedRoleHandler::attach(PwmBinding& b, uint8_t portIdx,
                            const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<LedAnimator>(b.port);
    // Optional config: [masterBrightnessPct:u8]  — 0..100, matches the
    // LED_SET_BRIGHTNESS packet semantics.
    if (cfgLen >= 1) role.setMasterBrightnessPct(cfg[0]);
    role.onQueueDone([this, portIdx]() { _emit->emitLedQueueDone(portIdx); });
    return true;
}

void LedRoleHandler::handleQueueLoad(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx   = p[0];
    const uint8_t count = p[1];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<LedAnimator>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    if (count > LedAnimator::MAX_EVENTS) { _ctx->sendNack(RoleError::ROLE_QUEUE_FULL); return; }

    // Fixed-size wire records — keep aligned with `LedAnimator::WIRE_EVENT_SIZE`
    // AND the master-side `hubfx::effects::lightfx::kEventWireSize`.
    //   [kind:u8]
    //   [durationMs:u16LE]
    //   [cycleMs:u16LE]
    //   [brightnessPct:u8]
    //   [minPct:u8]
    //   [maxPct:u8]
    //   [flashPct:u8]
    //   [flags:u8]
    const size_t needed = 2 + (size_t)count * LedAnimator::WIRE_EVENT_SIZE;
    if (len < needed) {
        SFX_LOG_DEBUG("[LedAnimator] LED_QUEUE_LOAD short payload: have %u, need %u",
                      (unsigned)len, (unsigned)needed);
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    LedAnimator::Event ev[LedAnimator::MAX_EVENTS];
    size_t off = 2;
    for (uint8_t i = 0; i < count; i++) {
        ev[i].kind          = p[off + 0];
        ev[i].durationMs    = SfxWire::getU16LE(&p[off + 1]);
        ev[i].cycleMs       = SfxWire::getU16LE(&p[off + 3]);
        ev[i].brightnessPct = p[off + 5];
        ev[i].minPct        = p[off + 6];
        ev[i].maxPct        = p[off + 7];
        ev[i].flashPct      = p[off + 8];
        ev[i].flags         = p[off + 9];
        off += LedAnimator::WIRE_EVENT_SIZE;
        if (ev[i].kind > LedAnimator::EV_BEACON) {
            SFX_LOG_DEBUG("[LedAnimator] LED_QUEUE_LOAD: unknown kind %u",
                          (unsigned)ev[i].kind);
            _ctx->sendNack(SerialError::INVALID_PARAM);
            return;
        }
    }
    SFX_LOG_DEBUG("[LedAnimator] LED_QUEUE_LOAD: port=%u count=%u",
                  (unsigned)idx, (unsigned)count);
    if (!r->loadQueue(ev, count)) { _ctx->sendNack(RoleError::ROLE_QUEUE_FULL); return; }
    _ctx->sendAck();
}

void LedRoleHandler::handleStart(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<LedAnimator>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->start();
    _ctx->sendAck();
}

void LedRoleHandler::handleStop(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<LedAnimator>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->stop();
    _ctx->sendAck();
}

void LedRoleHandler::handleSetBrightness(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<LedAnimator>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    // Wire value is brightness percent (0..100), matching the master-
    // side LightFx encoding.  Driver clamps internally.
    r->setMasterBrightnessPct(p[1]);
    SFX_LOG_DEBUG("[LedAnimator] LED_SET_BRIGHTNESS: port=%u pct=%u",
                  (unsigned)p[0], (unsigned)p[1]);
    _ctx->sendAck();
}

void LedRoleHandler::handleGetStatusReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<LedAnimator>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[4];
    out[0] = idx;
    out[1] = r->masterBrightnessPct();
    out[2] = r->isPlaying() ? 1 : 0;
    out[3] = r->queueDepth();
    _ctx->sendRawPacket(RolePacket::LED_STATUS_RESP, _ctx->currentTag(), out, sizeof out);
}

}  // namespace sfx_core
