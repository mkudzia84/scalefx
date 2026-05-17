/*
 * RoleServicePolicy implementation — attach / detach + per-role dispatch.
 */

#include "role_service.h"

#include <cstring>
#include <variant>

namespace sfx_core {

namespace {

// Map a role command opcode to the port-kind the command targets.
// (Servo: 0x48..0x57; Pwm: 0x58..0x67 + 0x70..0x77; HBridge: 0x68..0x6F)
constexpr uint8_t portKindForRoleCmd(uint8_t op) {
    if (op >= 0x48 && op <= 0x57) return PortKind::Servo;
    if (op >= 0x58 && op <= 0x67) return PortKind::Pwm;
    if (op >= 0x68 && op <= 0x6F) return PortKind::HBridge;
    if (op >= 0x70 && op <= 0x77) return PortKind::Pwm;
    return PortKind::Unknown;
}

}  // namespace

// ── Top-level dispatch ──────────────────────────────────────────────

CommandHandleResult RoleServicePolicy::handle(uint8_t type, const uint8_t* p, size_t len) {
    if (!_reg) { _ctx->sendNack(SerialError::INTERNAL_ERROR); return CommandHandleResult::Handled; }

    switch (type) {
        case RolePacket::ROLE_ATTACH:           handleAttach(p, len);              break;
        case RolePacket::ROLE_DETACH:           handleDetach(p, len);              break;
        case RolePacket::ROLE_LIST_REQ:         handleList();                       break;

        // Servo actuator
        case RolePacket::SERVO_SET_TARGET:      handleServoSetTarget(p, len);       break;
        case RolePacket::SERVO_GET_STATUS_REQ:  handleServoGetStatusReq(p, len);    break;

        // RC PWM input
        case RolePacket::RCIN_GET_VALUE_REQ:    handleRcInGetValueReq(p, len);      break;
        case RolePacket::RCIN_SET_BROADCAST_HZ: handleRcInSetBroadcastHz(p, len);   break;

        // LED animator
        case RolePacket::LED_QUEUE_LOAD:        handleLedQueueLoad(p, len);         break;
        case RolePacket::LED_START:             handleLedStart(p, len);             break;
        case RolePacket::LED_STOP:              handleLedStop(p, len);              break;
        case RolePacket::LED_SET_BRIGHTNESS:    handleLedSetBrightness(p, len);     break;
        case RolePacket::LED_GET_STATUS_REQ:    handleLedGetStatusReq(p, len);      break;

        // DC motor
        case RolePacket::MOTOR_SET_DUTY:        handleMotorSetDuty(p, len);         break;
        case RolePacket::MOTOR_BRAKE:           handleMotorBrake(p, len);           break;
        case RolePacket::MOTOR_GET_STATUS_REQ:  handleMotorGetStatusReq(p, len);    break;

        // Bi-directional motor
        case RolePacket::BIMOTOR_SET_SIGNED:    handleBiMotorSetSigned(p, len);     break;
        case RolePacket::BIMOTOR_BRAKE:         handleBiMotorBrake(p, len);         break;
        case RolePacket::BIMOTOR_COAST:         handleBiMotorCoast(p, len);         break;
        case RolePacket::BIMOTOR_GET_STATUS_REQ:handleBiMotorGetStatus(p, len);     break;

        // Heater
        case RolePacket::HEATER_SET_TARGET:     handleHeaterSetTarget(p, len);      break;
        case RolePacket::HEATER_GET_STATUS_REQ: handleHeaterGetStatus(p, len);      break;

        default:                                return CommandHandleResult::NotMyCommand;
    }
    return CommandHandleResult::Handled;
}

// ── Per-loop tick ────────────────────────────────────────────────────

void RoleServicePolicy::update() {
    if (!_reg) return;

    for (uint8_t i = 0; i < _reg->numServoPorts(); i++) {
        auto* b = _reg->servoAt(i);
        if (!b) continue;
        std::visit([](auto& r) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(r)>, std::monostate>) {
                r.tick();
            }
        }, b->role);
    }
    for (uint8_t i = 0; i < _reg->numPwmPorts(); i++) {
        auto* b = _reg->pwmAt(i);
        if (!b) continue;
        std::visit([](auto& r) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(r)>, std::monostate>) {
                r.tick();
            }
        }, b->role);
    }
    for (uint8_t i = 0; i < _reg->numHBridgePorts(); i++) {
        auto* b = _reg->hbridgeAt(i);
        if (!b) continue;
        std::visit([](auto& r) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(r)>, std::monostate>) {
                r.tick();
            }
        }, b->role);
    }
}

// ── ROLE_ATTACH / ROLE_DETACH / ROLE_LIST ───────────────────────────

void RoleServicePolicy::handleAttach(const uint8_t* p, size_t len) {
    if (len < 4) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t portKind  = p[0];
    const uint8_t portIdx   = p[1];
    const uint8_t roleKind  = p[2];
    const uint8_t cfgLen    = p[3];
    if (len < (size_t)4 + cfgLen) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t* cfg = &p[4];

    bool ok = false;
    switch (portKind) {
        case PortKind::Servo: {
            auto* b = _reg->servoAt(portIdx);
            if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
            switch (roleKind) {
                case RoleKind::ServoActuator: ok = attachServoActuator(*b, portIdx, cfg, cfgLen); break;
                case RoleKind::RcPwmInput:    ok = attachRcPwmInput   (*b, portIdx, cfg, cfgLen); break;
                default: _ctx->sendNack(RoleError::ROLE_KIND_NOT_SUPPORTED); return;
            }
            break;
        }
        case PortKind::Pwm: {
            auto* b = _reg->pwmAt(portIdx);
            if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
            switch (roleKind) {
                case RoleKind::LedAnimator: ok = attachLedAnimator(*b, portIdx, cfg, cfgLen); break;
                case RoleKind::DcMotor:     ok = attachDcMotor    (*b, portIdx, cfg, cfgLen); break;
                case RoleKind::Heater:      ok = attachHeater     (*b, portIdx, cfg, cfgLen); break;
                default: _ctx->sendNack(RoleError::ROLE_KIND_NOT_SUPPORTED); return;
            }
            break;
        }
        case PortKind::HBridge: {
            auto* b = _reg->hbridgeAt(portIdx);
            if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
            switch (roleKind) {
                case RoleKind::BiDcMotor: ok = attachBiDcMotor(*b, portIdx, cfg, cfgLen); break;
                default: _ctx->sendNack(RoleError::ROLE_KIND_NOT_SUPPORTED); return;
            }
            break;
        }
        default: _ctx->sendNack(PortError::PORT_NOT_FOUND); return;
    }

    if (ok) _ctx->sendAck();
    else    _ctx->sendNack(RoleError::ROLE_CONFIG_INVALID);
}

void RoleServicePolicy::handleDetach(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t portKind = p[0];
    const uint8_t portIdx  = p[1];

    switch (portKind) {
        case PortKind::Servo: {
            auto* b = _reg->servoAt(portIdx);
            if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
            b->role.emplace<std::monostate>();
            break;
        }
        case PortKind::Pwm: {
            auto* b = _reg->pwmAt(portIdx);
            if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
            // Stop any in-flight role output before detaching.
            if (b->port) b->port->setDuty(0);
            b->role.emplace<std::monostate>();
            break;
        }
        case PortKind::HBridge: {
            auto* b = _reg->hbridgeAt(portIdx);
            if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
            if (b->port) b->port->coast();
            b->role.emplace<std::monostate>();
            break;
        }
        default: _ctx->sendNack(PortError::PORT_NOT_FOUND); return;
    }
    _ctx->sendAck();
}

void RoleServicePolicy::handleList() {
    // [count:u8] × [portKind, portIdx, roleKind, flags]
    uint8_t buf[1 + 16*4];
    size_t  off = 1;
    uint8_t count = 0;

    auto appendIfAttached = [&](uint8_t portKind, uint8_t portIdx, uint8_t roleKind) {
        if (off + 4 > sizeof buf) return;
        buf[off++] = portKind;
        buf[off++] = portIdx;
        buf[off++] = roleKind;
        buf[off++] = 0;             // flags reserved
        count++;
    };

    for (uint8_t i = 0; i < _reg->numServoPorts(); i++) {
        auto* b = _reg->servoAt(i);
        if (!b || !b->hasRole()) continue;
        uint8_t rk = RoleKind::None;
        if (std::holds_alternative<ServoActuatorRole>(b->role)) rk = RoleKind::ServoActuator;
        else if (std::holds_alternative<RcPwmInputRole>(b->role)) rk = RoleKind::RcPwmInput;
        appendIfAttached(PortKind::Servo, i, rk);
    }
    for (uint8_t i = 0; i < _reg->numPwmPorts(); i++) {
        auto* b = _reg->pwmAt(i);
        if (!b || !b->hasRole()) continue;
        uint8_t rk = RoleKind::None;
        if      (std::holds_alternative<LedAnimator>(b->role)) rk = RoleKind::LedAnimator;
        else if (std::holds_alternative<DcMotorRole>(b->role)) rk = RoleKind::DcMotor;
        else if (std::holds_alternative<HeaterRole>(b->role))  rk = RoleKind::Heater;
        appendIfAttached(PortKind::Pwm, i, rk);
    }
    for (uint8_t i = 0; i < _reg->numHBridgePorts(); i++) {
        auto* b = _reg->hbridgeAt(i);
        if (!b || !b->hasRole()) continue;
        uint8_t rk = RoleKind::None;
        if (std::holds_alternative<BiDcMotorRole>(b->role)) rk = RoleKind::BiDcMotor;
        appendIfAttached(PortKind::HBridge, i, rk);
    }

    buf[0] = count;
    _ctx->sendRawPacket(RolePacket::ROLE_LIST_RESP, _ctx->currentTag(), buf, off);
}

// ── Role-emplacement helpers ────────────────────────────────────────

bool RoleServicePolicy::attachServoActuator(ServoBinding& b, uint8_t /*portIdx*/,
                                            const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<ServoActuatorRole>(b.port);
    // Optional config: [minUs:u16LE][maxUs:u16LE][maxVel_us_per_s:u16LE][reversed:u8]
    if (cfgLen >= 4) {
        const uint16_t mn = SfxWire::getU16LE(&cfg[0]);
        const uint16_t mx = SfxWire::getU16LE(&cfg[2]);
        role.setLimits(mn, mx);
    }
    if (cfgLen >= 6) role.setMaxVelocity_us_per_s(SfxWire::getU16LE(&cfg[4]));
    if (cfgLen >= 7) role.setReversed(cfg[6] != 0);
    const uint8_t portIdx = (uint8_t)(&b - _reg->servoAt(0));
    role.onTargetReached([this, portIdx](uint16_t pos) { emitServoTargetReached(portIdx, pos); });
    return true;
}

bool RoleServicePolicy::attachRcPwmInput(ServoBinding& b, uint8_t /*portIdx*/,
                                         const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<RcPwmInputRole>();
    if (!role.bind(b.port)) { b.role.emplace<std::monostate>(); return false; }
    // Optional config: [broadcastHz:u8]
    if (cfgLen >= 1) role.setBroadcastHz(cfg[0]);
    const uint8_t portIdx = (uint8_t)(&b - _reg->servoAt(0));
    role.onBroadcast([this, portIdx](uint16_t us, bool valid) {
        emitRcInValueBroadcast(portIdx, us, valid);
    });
    return true;
}

bool RoleServicePolicy::attachLedAnimator(PwmBinding& b, uint8_t /*portIdx*/,
                                          const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<LedAnimator>(b.port);
    // Optional config: [masterBrightness:u8]
    if (cfgLen >= 1) role.setMasterBrightness(cfg[0]);
    const uint8_t portIdx = (uint8_t)(&b - _reg->pwmAt(0));
    role.onQueueDone([this, portIdx]() { emitLedQueueDone(portIdx); });
    return true;
}

bool RoleServicePolicy::attachDcMotor(PwmBinding& b, uint8_t /*portIdx*/,
                                      const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<DcMotorRole>(b.port, b.iSense, b.vSense);
    // Optional config: [stallThreshold_mA:u16LE][stallWindow_ms:u16LE]
    if (cfgLen >= 4) {
        const uint16_t th = SfxWire::getU16LE(&cfg[0]);
        const uint16_t wn = SfxWire::getU16LE(&cfg[2]);
        role.setStallGuard(th, wn);
    }
    const uint8_t portIdx = (uint8_t)(&b - _reg->pwmAt(0));
    role.onStall([this, portIdx](uint16_t peak, uint16_t dur) {
        emitMotorStallEvent(portIdx, peak, dur);
    });
    return true;
}

bool RoleServicePolicy::attachHeater(PwmBinding& b, uint8_t /*portIdx*/,
                                     const uint8_t* cfg, size_t cfgLen) {
    if (!b.tSense) { return false; }
    auto& role = b.role.emplace<HeaterRole>();
    if (!role.bind(b.port, b.tSense)) { b.role.emplace<std::monostate>(); return false; }
    // Optional config: [target_cx10:i16LE][hysteresis_cx10:i16LE][driveDuty:u16LE]
    if (cfgLen >= 2) role.setTarget((int16_t)SfxWire::getI16LE(&cfg[0]));
    if (cfgLen >= 4) role.setHysteresis((int16_t)SfxWire::getI16LE(&cfg[2]));
    if (cfgLen >= 6) role.setDriveDuty(SfxWire::getU16LE(&cfg[4]));
    return true;
}

bool RoleServicePolicy::attachBiDcMotor(HBridgeBinding& b, uint8_t /*portIdx*/,
                                        const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<BiDcMotorRole>(b.port, b.iSense, b.vSense);
    if (cfgLen >= 4) {
        const uint16_t th = SfxWire::getU16LE(&cfg[0]);
        const uint16_t wn = SfxWire::getU16LE(&cfg[2]);
        role.setStallGuard(th, wn);
    }
    const uint8_t portIdx = (uint8_t)(&b - _reg->hbridgeAt(0));
    role.onStall([this, portIdx](uint16_t peak, uint16_t dur) {
        emitBiMotorStallEvent(portIdx, peak, dur);
    });
    return true;
}

// ── Servo actuator role commands ────────────────────────────────────

void RoleServicePolicy::handleServoSetTarget(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t  idx    = p[0];
    const uint16_t target = SfxWire::getU16LE(&p[1]);
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (auto* r = std::get_if<ServoActuatorRole>(&b->role)) {
        r->setTarget(target);
        _ctx->sendAck();
    } else {
        _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH);
    }
}

void RoleServicePolicy::handleServoGetStatusReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<ServoActuatorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }

    uint8_t out[8];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], r->position());
    SfxWire::putU16LE(&out[3], r->target());
    SfxWire::putI16LE(&out[5], r->velocity_us_per_s());
    out[7] = r->atTarget() ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::SERVO_STATUS_RESP, _ctx->currentTag(), out, sizeof out);
}

// ── RC PWM input role commands ──────────────────────────────────────

void RoleServicePolicy::handleRcInGetValueReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<RcPwmInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[4];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], r->latest_us());
    out[3] = r->valid() ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::RCIN_VALUE_RESP, _ctx->currentTag(), out, sizeof out);
}

void RoleServicePolicy::handleRcInSetBroadcastHz(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    const uint8_t hz  = p[1];
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<RcPwmInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setBroadcastHz(hz);
    _ctx->sendAck();
}

// ── LED animator role commands ──────────────────────────────────────

void RoleServicePolicy::handleLedQueueLoad(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx   = p[0];
    const uint8_t count = p[1];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<LedAnimator>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    if (count > LedAnimator::MAX_EVENTS) { _ctx->sendNack(RoleError::ROLE_QUEUE_FULL); return; }

    // Each event: [kind:u8] then payload depending on kind.
    LedAnimator::Event ev[LedAnimator::MAX_EVENTS];
    size_t off = 2;
    for (uint8_t i = 0; i < count; i++) {
        if (off >= len) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
        ev[i].kind = p[off++];
        switch (ev[i].kind) {
            case LedAnimator::EV_ON:
                if (off + 1 > len) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
                ev[i].brightness = p[off++];
                break;
            case LedAnimator::EV_OFF:
                break;
            case LedAnimator::EV_FADE:
                if (off + 3 > len) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
                ev[i].brightness  = p[off++];
                ev[i].duration_ms = SfxWire::getU16LE(&p[off]); off += 2;
                break;
            case LedAnimator::EV_HOLD:
                if (off + 2 > len) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
                ev[i].duration_ms = SfxWire::getU16LE(&p[off]); off += 2;
                break;
            case LedAnimator::EV_REPEAT:
                break;
            default:
                _ctx->sendNack(SerialError::INVALID_PARAM); return;
        }
    }
    if (!r->loadQueue(ev, count)) { _ctx->sendNack(RoleError::ROLE_QUEUE_FULL); return; }
    _ctx->sendAck();
}

void RoleServicePolicy::handleLedStart(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<LedAnimator>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->start();
    _ctx->sendAck();
}

void RoleServicePolicy::handleLedStop(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<LedAnimator>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->stop();
    _ctx->sendAck();
}

void RoleServicePolicy::handleLedSetBrightness(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<LedAnimator>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setMasterBrightness(p[1]);
    _ctx->sendAck();
}

void RoleServicePolicy::handleLedGetStatusReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<LedAnimator>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[4];
    out[0] = idx;
    out[1] = r->masterBrightness();
    out[2] = r->isPlaying() ? 1 : 0;
    out[3] = r->queueDepth();
    _ctx->sendRawPacket(RolePacket::LED_STATUS_RESP, _ctx->currentTag(), out, sizeof out);
}

// ── DC motor role commands ──────────────────────────────────────────

void RoleServicePolicy::handleMotorSetDuty(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setDuty(SfxWire::getU16LE(&p[1]));
    _ctx->sendAck();
}

void RoleServicePolicy::handleMotorBrake(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->brake();
    _ctx->sendAck();
}

void RoleServicePolicy::handleMotorGetStatusReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[8];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], r->duty());
    SfxWire::putI16LE(&out[3], r->voltage_mV());
    SfxWire::putI16LE(&out[5], r->current_mA());
    out[7] = r->stalled() ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::MOTOR_STATUS_RESP, _ctx->currentTag(), out, sizeof out);
}

// ── Bi-directional motor role commands ──────────────────────────────

void RoleServicePolicy::handleBiMotorSetSigned(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setSigned((int16_t)SfxWire::getI16LE(&p[1]));
    _ctx->sendAck();
}

void RoleServicePolicy::handleBiMotorBrake(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->brake();
    _ctx->sendAck();
}

void RoleServicePolicy::handleBiMotorCoast(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->coast();
    _ctx->sendAck();
}

void RoleServicePolicy::handleBiMotorGetStatus(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->hbridgeAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[8];
    out[0] = idx;
    SfxWire::putI16LE(&out[1], r->signedDuty());
    SfxWire::putI16LE(&out[3], r->voltage_mV());
    SfxWire::putI16LE(&out[5], r->current_mA());
    out[7] = r->stalled() ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::BIMOTOR_STATUS_RESP, _ctx->currentTag(), out, sizeof out);
}

// ── Heater role commands ────────────────────────────────────────────

void RoleServicePolicy::handleHeaterSetTarget(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<HeaterRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setTarget((int16_t)SfxWire::getI16LE(&p[1]));
    _ctx->sendAck();
}

void RoleServicePolicy::handleHeaterGetStatus(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<HeaterRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[8];
    out[0] = idx;
    SfxWire::putI16LE(&out[1], r->target());
    SfxWire::putI16LE(&out[3], r->actual_cx10());
    SfxWire::putU16LE(&out[5], r->commandedDuty());
    out[7] = r->heating() ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::HEATER_STATUS_RESP, _ctx->currentTag(), out, sizeof out);
}

// ── Async event emitters ────────────────────────────────────────────

void RoleServicePolicy::emitLedQueueDone(uint8_t portIdx) {
    uint8_t buf[1] = { portIdx };
    _ctx->sendRawPacket(RolePacket::LED_QUEUE_DONE, SfxWire::TAG_ASYNC, buf, sizeof buf);
}

void RoleServicePolicy::emitServoTargetReached(uint8_t portIdx, uint16_t pos_us) {
    uint8_t buf[3];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], pos_us);
    _ctx->sendRawPacket(RolePacket::SERVO_TARGET_REACHED, SfxWire::TAG_ASYNC, buf, sizeof buf);
}

void RoleServicePolicy::emitMotorStallEvent(uint8_t portIdx, uint16_t peak_mA, uint16_t duration_ms) {
    uint8_t buf[5];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], peak_mA);
    SfxWire::putU16LE(&buf[3], duration_ms);
    _ctx->sendRawPacket(RolePacket::MOTOR_STALL_EVENT, SfxWire::TAG_ASYNC, buf, sizeof buf);
}

void RoleServicePolicy::emitBiMotorStallEvent(uint8_t portIdx, uint16_t peak_mA, uint16_t duration_ms) {
    uint8_t buf[5];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], peak_mA);
    SfxWire::putU16LE(&buf[3], duration_ms);
    _ctx->sendRawPacket(RolePacket::BIMOTOR_STALL_EVENT, SfxWire::TAG_ASYNC, buf, sizeof buf);
}

void RoleServicePolicy::emitRcInValueBroadcast(uint8_t portIdx, uint16_t us, bool valid) {
    uint8_t buf[4];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], us);
    buf[3] = valid ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::RCIN_VALUE_BROADCAST, SfxWire::TAG_ASYNC, buf, sizeof buf);
}

}  // namespace sfx_core
