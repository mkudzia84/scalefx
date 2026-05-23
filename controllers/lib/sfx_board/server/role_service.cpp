/*
 * RoleServicePolicy implementation — attach / detach + per-role dispatch.
 */

#include "role_service.h"

#include <cstring>
#include <variant>

namespace sfx_core {

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
        case RolePacket::SERVO_SET_PROFILE:     handleServoSetProfile(p, len);      break;
        case RolePacket::SERVO_GET_PROFILE_REQ: handleServoGetProfileReq(p, len);   break;

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
        case RolePacket::MOTOR_SET_ELEMENT:     handleMotorSetElement(p, len);      break;
        case RolePacket::MOTOR_GET_ELEMENT_REQ: handleMotorGetElementReq(p, len);   break;
        case RolePacket::MOTOR_SET_PCT:         handleMotorSetPct(p, len);          break;

        // Bi-directional motor
        case RolePacket::BIMOTOR_SET_SIGNED:    handleBiMotorSetSigned(p, len);     break;
        case RolePacket::BIMOTOR_BRAKE:         handleBiMotorBrake(p, len);         break;
        case RolePacket::BIMOTOR_COAST:         handleBiMotorCoast(p, len);         break;
        case RolePacket::BIMOTOR_GET_STATUS_REQ:handleBiMotorGetStatus(p, len);     break;
        case RolePacket::BIMOTOR_SEEK_ENDSTOP:  handleBiMotorSeekEndstop(p, len);   break;

        // Heater
        case RolePacket::HEATER_SET_TARGET:     handleHeaterSetTarget(p, len);      break;
        case RolePacket::HEATER_GET_STATUS_REQ: handleHeaterGetStatus(p, len);      break;
        case RolePacket::HEATER_SET_ELEMENT:    handleHeaterSetElement(p, len);     break;
        case RolePacket::HEATER_GET_ELEMENT_REQ: handleHeaterGetElementReq(p, len); break;

        // SBUS input
        case RolePacket::SBUS_GET_FRAME_REQ:    handleSbusGetFrameReq(p, len);      break;
        case RolePacket::SBUS_SET_BROADCAST_HZ: handleSbusSetBroadcastHz(p, len);   break;

        // Jeti EX input
        case RolePacket::JETIEX_GET_FRAME_REQ:    handleJetiExGetFrameReq(p, len);     break;
        case RolePacket::JETIEX_SET_BROADCAST_HZ: handleJetiExSetBroadcastHz(p, len);  break;

        default:                                return CommandHandleResult::NotMyCommand;
    }
    return CommandHandleResult::Handled;
}

// ── Per-loop tick ────────────────────────────────────────────────────

void RoleServicePolicy::update() {
    if (!_reg) return;

    // One shared clock for the whole pass — every LedAnimator samples the
    // SAME instant, so multi-channel light programs stay phase-locked
    // regardless of how long the per-channel I²C writes take or how
    // jittery the main loop is.  (See LedAnimator::tick.)
    const uint32_t now = millis();

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
        std::visit([now](auto& r) {
            using R = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<R, LedAnimator>) {
                r.tick(now);                 // shared-clock LED tick
            } else if constexpr (!std::is_same_v<R, std::monostate>) {
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
    for (uint8_t i = 0; i < _reg->numInputPorts(); i++) {
        auto* b = _reg->inputAt(i);
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
        case PortKind::Input: {
            auto* b = _reg->inputAt(portIdx);
            if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
            switch (roleKind) {
                case RoleKind::RcPwmInput:  ok = attachRcPwmInput (*b, portIdx, cfg, cfgLen); break;
                case RoleKind::SbusInput:   ok = attachSbusInput  (*b, portIdx, cfg, cfgLen); break;
                case RoleKind::JetiExInput: ok = attachJetiExInput(*b, portIdx, cfg, cfgLen); break;
                default: _ctx->sendNack(RoleError::ROLE_KIND_NOT_SUPPORTED); return;
            }
            break;
        }
        default: _ctx->sendNack(PortError::PORT_NOT_FOUND); return;
    }

    if (ok) {
        _ctx->sendAck();
        emitRoleAttached(portKind, portIdx, roleKind);
    } else {
        _ctx->sendNack(RoleError::ROLE_CONFIG_INVALID);
    }
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
        case PortKind::Input: {
            auto* b = _reg->inputAt(portIdx);
            if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
            // Release the peripheral the previous role had claimed.
            if (b->port) b->port->disable();
            b->role.emplace<std::monostate>();
            break;
        }
        default: _ctx->sendNack(PortError::PORT_NOT_FOUND); return;
    }
    _ctx->sendAck();
    emitRoleDetached(portKind, portIdx);
}

void RoleServicePolicy::handleList() {
    // [count:u8] × [portKind, portIdx, roleKind, flags]
    uint8_t buf[1 + 32*4];
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
    for (uint8_t i = 0; i < _reg->numInputPorts(); i++) {
        auto* b = _reg->inputAt(i);
        if (!b || !b->hasRole()) continue;
        uint8_t rk = RoleKind::None;
        if      (std::holds_alternative<RcPwmInputRole>(b->role))  rk = RoleKind::RcPwmInput;
        else if (std::holds_alternative<SbusInputRole>(b->role))   rk = RoleKind::SbusInput;
        else if (std::holds_alternative<JetiExInputRole>(b->role)) rk = RoleKind::JetiExInput;
        appendIfAttached(PortKind::Input, i, rk);
    }

    buf[0] = count;
    _ctx->sendRawPacket(RolePacket::ROLE_LIST_RESP, _ctx->currentTag(), buf, off);
}

// ── Role-emplacement helpers ────────────────────────────────────────

bool RoleServicePolicy::attachServoActuator(ServoBinding& b, uint8_t portIdx,
                                            const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<ServoActuatorRole>(b.port);
    // Rule 42 storage + Rule 44 editing-surface: the per-port profile
    // travels with the role-attach payload from /hubfx.yaml's ports[]
    // block.  Append-only (Rule 11):
    //   [minUs:u16LE][maxUs:u16LE]                          — calibration limits
    //   [maxSpeedUsPerSec:u16LE]                            — slew limit
    //   [reversed:u8]                                       — REV flag
    //   [centerUs:u16LE]                                    — neutral / failsafe
    //   [maxAccelUsPerSec2:u16LE][maxJerkUsPerSec3:u16LE]   — trapezoidal / S-curve
    if (cfgLen >= 4) {
        const uint16_t mn = SfxWire::getU16LE(&cfg[0]);
        const uint16_t mx = SfxWire::getU16LE(&cfg[2]);
        role.setLimits(mn, mx);
    }
    ServoMotionProfile prof = role.profile();   // start from current (initFromPort)
    if (cfgLen >= 6) prof.maxSpeedUsPerSec = SfxWire::getU16LE(&cfg[4]);
    if (cfgLen >= 7) role.setReversed(cfg[6] != 0);
    if (cfgLen >= 9)  prof.centerUs          = SfxWire::getU16LE(&cfg[7]);
    if (cfgLen >= 11) prof.maxAccelUsPerSec2 = SfxWire::getU16LE(&cfg[9]);
    if (cfgLen >= 13) prof.maxJerkUsPerSec3  = SfxWire::getU16LE(&cfg[11]);
    role.setProfile(prof);
    role.onTargetReached([this, portIdx](uint16_t pos) { emitServoTargetReached(portIdx, pos); });
    return true;
}

bool RoleServicePolicy::attachRcPwmInput(InputBinding& b, uint8_t portIdx,
                                         const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<RcPwmInputRole>();
    if (!role.bind(b.port)) { b.role.emplace<std::monostate>(); return false; }
    // Optional config: [broadcastHz:u8]
    if (cfgLen >= 1) role.setBroadcastHz(cfg[0]);
    role.onBroadcast([this, portIdx](uint16_t us, bool valid) {
        emitRcInValueBroadcast(portIdx, us, valid);
    });
    return true;
}

bool RoleServicePolicy::attachSbusInput(InputBinding& b, uint8_t portIdx,
                                        const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<SbusInputRole>();
    if (!role.bind(b.port)) { b.role.emplace<std::monostate>(); return false; }
    // Optional config: [broadcastHz:u8]
    if (cfgLen >= 1) role.setBroadcastHz(cfg[0]);
    role.onBroadcast([this, portIdx](uint8_t /*ch*/, bool /*valid*/,
                                     bool /*failsafe*/, bool /*frameLost*/) {
        // The broadcast packet rebuilds the full channel payload —
        // walk the role each tick via the registry.
        auto* binding = _reg->inputAt(portIdx);
        if (!binding) return;
        if (auto* r = std::get_if<SbusInputRole>(&binding->role)) {
            emitSbusFrameBroadcast(portIdx, *r);
        }
    });
    return true;
}

bool RoleServicePolicy::attachJetiExInput(InputBinding& b, uint8_t portIdx,
                                          const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<JetiExInputRole>();
    // Optional config: [broadcastHz:u8][baudHi:u8][baudLo:u8]
    //   baud encoded as kbaud (125 / 250); 0 = use default 125 000.
    uint32_t baud = 125000;
    if (cfgLen >= 3) {
        const uint16_t kbaud = ((uint16_t)cfg[1] << 8) | cfg[2];
        if (kbaud == 250) baud = 250000;
        else if (kbaud == 125 || kbaud == 0) baud = 125000;
        else baud = (uint32_t)kbaud * 1000;
    }
    if (!role.bind(b.port, baud)) { b.role.emplace<std::monostate>(); return false; }
    if (cfgLen >= 1) role.setBroadcastHz(cfg[0]);
    role.onBroadcast([this, portIdx](uint8_t /*ch*/, bool /*valid*/,
                                     uint32_t /*rxFrames*/, uint32_t /*rxErrors*/) {
        auto* binding = _reg->inputAt(portIdx);
        if (!binding) return;
        if (auto* r = std::get_if<JetiExInputRole>(&binding->role)) {
            emitJetiExFrameBroadcast(portIdx, *r);
        }
    });
    return true;
}

bool RoleServicePolicy::attachLedAnimator(PwmBinding& b, uint8_t portIdx,
                                          const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<LedAnimator>(b.port);
    // Optional config: [masterBrightnessPct:u8]  — 0..100, matches the
    // LED_SET_BRIGHTNESS packet semantics.
    if (cfgLen >= 1) role.setMasterBrightnessPct(cfg[0]);
    role.onQueueDone([this, portIdx]() { emitLedQueueDone(portIdx); });
    return true;
}

bool RoleServicePolicy::attachDcMotor(PwmBinding& b, uint8_t portIdx,
                                      const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<DcMotorRole>(b.port, b.iSense, b.vSense);
    // Wire scaling context from the port binding (Phase 2 of GunFX
    // rollout, instructions/22).
    role.setPortRailMv(b.voltageMv);
    // Optional config (append-only — Rule 11 still governs fielded
    // firmware, even though the GunFX rollout itself is greenfield):
    //   [stallThreshold_mA:u16LE][stallWindow_ms:u16LE]
    //   [elementMv:u16LE][scaling:u8]
    if (cfgLen >= 4) {
        const uint16_t th = SfxWire::getU16LE(&cfg[0]);
        const uint16_t wn = SfxWire::getU16LE(&cfg[2]);
        role.setStallGuard(th, wn);
    }
    if (cfgLen >= 7) {
        ElementConfig ec;
        ec.elementMv = SfxWire::getU16LE(&cfg[4]);
        ec.mode      = static_cast<ElementScalingMode>(cfg[6]);
        role.setElement(ec);
    }
    role.onStall([this, portIdx](uint16_t peak, uint16_t dur) {
        emitMotorStallEvent(portIdx, peak, dur);
    });
    return true;
}

bool RoleServicePolicy::attachHeater(PwmBinding& b, uint8_t /*portIdx*/,
                                     const uint8_t* cfg, size_t cfgLen) {
    // tSense is optional: present → closed-loop bang-bang;
    // absent → open-loop drive at `drivePct` whenever the target is set.
    auto& role = b.role.emplace<HeaterRole>();
    if (!role.bind(b.port, b.tSense)) { b.role.emplace<std::monostate>(); return false; }
    // Wire scaling context from the port binding (Phase 2 of GunFX
    // rollout, instructions/22 — voltage scaling lives on the role,
    // sourced from the per-port rail declared in Phase 0).
    role.setPortRailMv(b.voltageMv);
    // Optional config:
    //   [target_cx10:i16LE][hysteresis_cx10:i16LE][drivePct:u8]
    //   [elementMv:u16LE][scaling:u8]
    if (cfgLen >= 2) role.setTarget((int16_t)SfxWire::getI16LE(&cfg[0]));
    if (cfgLen >= 4) role.setHysteresis((int16_t)SfxWire::getI16LE(&cfg[2]));
    if (cfgLen >= 5) role.setDrivePct(cfg[4]);
    if (cfgLen >= 8) {
        ElementConfig ec;
        ec.elementMv = SfxWire::getU16LE(&cfg[5]);
        ec.mode      = static_cast<ElementScalingMode>(cfg[7]);
        role.setElement(ec);
    }
    return true;
}

bool RoleServicePolicy::attachBiDcMotor(HBridgeBinding& b, uint8_t portIdx,
                                        const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<BiDcMotorRole>(b.port, b.iSense, b.vSense);
    if (cfgLen >= 4) {
        const uint16_t th = SfxWire::getU16LE(&cfg[0]);
        const uint16_t wn = SfxWire::getU16LE(&cfg[2]);
        role.setStallGuard(th, wn);
    }
    role.onStall([this, portIdx](uint16_t peak, uint16_t dur) {
        emitBiMotorStallEvent(portIdx, peak, dur);
    });
    role.onEndstopResult([this, portIdx](uint8_t outcome, uint16_t travel, uint16_t peak) {
        emitBiMotorEndstopResult(portIdx, outcome, travel, peak);
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

// Live motion-profile retune (Phase 2.9.x — Rule 42).  Same wire shape
// as the role-attach payload tail; the role's `setLimits` + `setReversed`
// + `setProfile` together replace any in-flight slew with the new shape.
// In-flight `target_us` is preserved (clamped into the new range).
void RoleServicePolicy::handleServoSetProfile(const uint8_t* p, size_t len) {
    if (len < 14) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<ServoActuatorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }

    const uint16_t minUs       = SfxWire::getU16LE(&p[1]);
    const uint16_t maxUs       = SfxWire::getU16LE(&p[3]);
    const uint16_t maxSpeed    = SfxWire::getU16LE(&p[5]);
    const bool     reversed    = p[7] != 0;
    const uint16_t centerUs    = SfxWire::getU16LE(&p[8]);
    const uint16_t maxAccel    = SfxWire::getU16LE(&p[10]);
    const uint16_t maxJerk     = SfxWire::getU16LE(&p[12]);

    r->setLimits(minUs, maxUs);
    r->setReversed(reversed);
    ServoMotionProfile prof = r->profile();   // start from current (limits already applied above)
    prof.maxSpeedUsPerSec  = maxSpeed;
    prof.centerUs          = centerUs;
    prof.maxAccelUsPerSec2 = maxAccel;
    prof.maxJerkUsPerSec3  = maxJerk;
    r->setProfile(prof);
    _ctx->sendAck();
}

void RoleServicePolicy::handleServoGetProfileReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<ServoActuatorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }

    const ServoMotionProfile& prof = r->profile();
    uint8_t out[14];
    out[0] = idx;
    SfxWire::putU16LE(&out[1],  prof.minUs);
    SfxWire::putU16LE(&out[3],  prof.maxUs);
    SfxWire::putU16LE(&out[5],  prof.maxSpeedUsPerSec);
    out[7] = prof.inverted ? 1 : 0;
    SfxWire::putU16LE(&out[8],  prof.centerUs);
    SfxWire::putU16LE(&out[10], prof.maxAccelUsPerSec2);
    SfxWire::putU16LE(&out[12], prof.maxJerkUsPerSec3);
    _ctx->sendRawPacket(RolePacket::SERVO_PROFILE_RESP, _ctx->currentTag(), out, sizeof out);
}

// ── RC PWM input role commands ──────────────────────────────────────

void RoleServicePolicy::handleRcInGetValueReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->inputAt(idx);
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
    auto* b = _reg->inputAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<RcPwmInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setBroadcastHz(hz);
    _ctx->sendAck();
}

// ── SBUS input role commands ────────────────────────────────────────

void RoleServicePolicy::handleSbusGetFrameReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->inputAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<SbusInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }

    const uint8_t count = r->channelCount();
    uint8_t flags = 0;
    if (r->valid())     flags |= 0x01;
    if (r->failsafe())  flags |= 0x02;
    if (r->frameLost()) flags |= 0x04;
    if (r->ch17())      flags |= 0x08;
    if (r->ch18())      flags |= 0x10;

    uint8_t out[3 + 16*2];
    out[0] = idx;
    out[1] = count;
    out[2] = flags;
    size_t off = 3;
    for (uint8_t i = 0; i < count && off + 2 <= sizeof out; i++) {
        SfxWire::putU16LE(&out[off], r->channel_us((uint8_t)(i + 1)));
        off += 2;
    }
    _ctx->sendRawPacket(RolePacket::SBUS_FRAME_RESP, _ctx->currentTag(), out, off);
}

void RoleServicePolicy::handleSbusSetBroadcastHz(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    const uint8_t hz  = p[1];
    auto* b = _reg->inputAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<SbusInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setBroadcastHz(hz);
    _ctx->sendAck();
}

// ── Jeti EX input role commands ─────────────────────────────────────

void RoleServicePolicy::handleJetiExGetFrameReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->inputAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<JetiExInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }

    const uint8_t count = r->channelCount();
    uint8_t out[1 + 1 + 1 + 4 + 4 + 16*2];
    size_t off = 0;
    out[off++] = idx;
    out[off++] = count;
    out[off++] = r->valid() ? 1 : 0;
    SfxWire::putU32LE(&out[off], r->rxFrameCount()); off += 4;
    SfxWire::putU32LE(&out[off], r->rxErrorCount()); off += 4;
    for (uint8_t i = 0; i < count && off + 2 <= sizeof out; i++) {
        SfxWire::putU16LE(&out[off], r->channel_us((uint8_t)(i + 1)));
        off += 2;
    }
    _ctx->sendRawPacket(RolePacket::JETIEX_FRAME_RESP, _ctx->currentTag(), out, off);
}

void RoleServicePolicy::handleJetiExSetBroadcastHz(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    const uint8_t hz  = p[1];
    auto* b = _reg->inputAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<JetiExInputRole>(&b->role);
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
    // Wire value is brightness percent (0..100), matching the master-
    // side LightFx encoding.  Driver clamps internally.
    r->setMasterBrightnessPct(p[1]);
    SFX_LOG_DEBUG("[LedAnimator] LED_SET_BRIGHTNESS: port=%u pct=%u",
                  (unsigned)p[0], (unsigned)p[1]);
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
    out[1] = r->masterBrightnessPct();
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

// Live element-config retune (Phase 2.9.x — Rule 42).
void RoleServicePolicy::handleMotorSetElement(const uint8_t* p, size_t len) {
    if (len < 4) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    ElementConfig ec;
    ec.elementMv = SfxWire::getU16LE(&p[1]);
    ec.mode      = static_cast<ElementScalingMode>(p[3]);
    r->setElement(ec);
    _ctx->sendAck();
}

void RoleServicePolicy::handleMotorGetElementReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[6];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], r->element().elementMv);
    out[3] = static_cast<uint8_t>(r->element().mode);
    SfxWire::putU16LE(&out[4], r->portRailMv());
    _ctx->sendRawPacket(RolePacket::MOTOR_ELEMENT_RESP, _ctx->currentTag(), out, sizeof out);
}

// Intent-layer DC motor drive (Phase 2.9.x — Rule 42).  "Drive at N %
// of the element's rated voltage"; the role applies scaleDuty() and
// writes the port-native duty.  Replaces the gun_unit `pct*40` stopgap
// for the smoke fan.
void RoleServicePolicy::handleMotorSetPct(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t pct = p[1];
    if (pct > 100) pct = 100;
    r->setPct(pct);
    _ctx->sendAck();
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

void RoleServicePolicy::handleBiMotorSeekEndstop(const uint8_t* p, size_t len) {
    if (len < 5) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    const int16_t  duty    = (int16_t)SfxWire::getI16LE(&p[1]);
    const uint16_t timeout = SfxWire::getU16LE(&p[3]);   // 0 = no timeout
    r->seekEndstop(duty, timeout);
    _ctx->sendAck();
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

// Live element-config retune (Phase 2.9.x — Rule 42). Element voltage,
// scaling mode, drive percent and hysteresis are all live-tunable. The
// heater's bang-bang `target_cx10` stays separate (HEATER_SET_TARGET).
void RoleServicePolicy::handleHeaterSetElement(const uint8_t* p, size_t len) {
    if (len < 7) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<HeaterRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    ElementConfig ec;
    ec.elementMv = SfxWire::getU16LE(&p[1]);
    ec.mode      = static_cast<ElementScalingMode>(p[3]);
    r->setElement(ec);
    r->setDrivePct(p[4]);
    r->setHysteresis(SfxWire::getI16LE(&p[5]));
    _ctx->sendAck();
}

void RoleServicePolicy::handleHeaterGetElementReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<HeaterRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[9];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], r->element().elementMv);
    out[3] = static_cast<uint8_t>(r->element().mode);
    out[4] = r->drivePct();
    SfxWire::putI16LE(&out[5], r->hysteresis_cx10());
    SfxWire::putU16LE(&out[7], r->portRailMv());
    _ctx->sendRawPacket(RolePacket::HEATER_ELEMENT_RESP, _ctx->currentTag(), out, sizeof out);
}

// ── Async event emitters ────────────────────────────────────────────

void RoleServicePolicy::emitRoleAttached(uint8_t portKind, uint8_t portIdx, uint8_t roleKind) {
    uint8_t buf[3] = { portKind, portIdx, roleKind };
    _ctx->sendRawPacket(RolePacket::ROLE_ATTACHED, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::ROLE_ATTACHED, buf, sizeof buf);
}

void RoleServicePolicy::emitRoleDetached(uint8_t portKind, uint8_t portIdx) {
    uint8_t buf[2] = { portKind, portIdx };
    _ctx->sendRawPacket(RolePacket::ROLE_DETACHED, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::ROLE_DETACHED, buf, sizeof buf);
}

void RoleServicePolicy::emitLedQueueDone(uint8_t portIdx) {
    uint8_t buf[1] = { portIdx };
    _ctx->sendRawPacket(RolePacket::LED_QUEUE_DONE, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::LED_QUEUE_DONE, buf, sizeof buf);
}

void RoleServicePolicy::emitServoTargetReached(uint8_t portIdx, uint16_t pos_us) {
    uint8_t buf[3];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], pos_us);
    _ctx->sendRawPacket(RolePacket::SERVO_TARGET_REACHED, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::SERVO_TARGET_REACHED, buf, sizeof buf);
}

void RoleServicePolicy::emitMotorStallEvent(uint8_t portIdx, uint16_t peak_mA, uint16_t duration_ms) {
    uint8_t buf[5];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], peak_mA);
    SfxWire::putU16LE(&buf[3], duration_ms);
    _ctx->sendRawPacket(RolePacket::MOTOR_STALL_EVENT, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::MOTOR_STALL_EVENT, buf, sizeof buf);
}

void RoleServicePolicy::emitBiMotorStallEvent(uint8_t portIdx, uint16_t peak_mA, uint16_t duration_ms) {
    uint8_t buf[5];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], peak_mA);
    SfxWire::putU16LE(&buf[3], duration_ms);
    _ctx->sendRawPacket(RolePacket::BIMOTOR_STALL_EVENT, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::BIMOTOR_STALL_EVENT, buf, sizeof buf);
}

void RoleServicePolicy::emitBiMotorEndstopResult(uint8_t portIdx, uint8_t outcome,
                                                 uint16_t travel_ms, uint16_t peak_mA) {
    uint8_t buf[6];
    buf[0] = portIdx;
    buf[1] = outcome;
    SfxWire::putU16LE(&buf[2], travel_ms);
    SfxWire::putU16LE(&buf[4], peak_mA);
    _ctx->sendRawPacket(RolePacket::BIMOTOR_ENDSTOP_RESULT, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::BIMOTOR_ENDSTOP_RESULT, buf, sizeof buf);
}

void RoleServicePolicy::emitRcInValueBroadcast(uint8_t portIdx, uint16_t us, bool valid) {
    uint8_t buf[4];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], us);
    buf[3] = valid ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::RCIN_VALUE_BROADCAST, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::RCIN_VALUE_BROADCAST, buf, sizeof buf);
}

void RoleServicePolicy::emitSbusFrameBroadcast(uint8_t portIdx, const SbusInputRole& role) {
    const uint8_t count = role.channelCount();
    uint8_t flags = 0;
    if (role.valid())     flags |= 0x01;
    if (role.failsafe())  flags |= 0x02;
    if (role.frameLost()) flags |= 0x04;
    if (role.ch17())      flags |= 0x08;
    if (role.ch18())      flags |= 0x10;

    // SBUS is protocol-fixed at 16 channels; sized to spec.
    uint8_t buf[3 + 16*2];
    buf[0] = portIdx;
    buf[1] = count;
    buf[2] = flags;
    size_t off = 3;
    for (uint8_t i = 0; i < count && off + 2 <= sizeof buf; i++) {
        SfxWire::putU16LE(&buf[off], role.channel_us((uint8_t)(i + 1)));
        off += 2;
    }
    _ctx->sendRawPacket(RolePacket::SBUS_FRAME_BROADCAST, SfxWire::TAG_ASYNC, buf, off);
    fireLocalAsync(RolePacket::SBUS_FRAME_BROADCAST, buf, off);
}

void RoleServicePolicy::emitJetiExFrameBroadcast(uint8_t portIdx, const JetiExInputRole& role) {
    const uint8_t count = role.channelCount();
    // Jeti EX Bus carries up to 24 proportional channels per frame.
    uint8_t buf[3 + 24*2];
    buf[0] = portIdx;
    buf[1] = count;
    buf[2] = role.valid() ? 1 : 0;
    size_t off = 3;
    for (uint8_t i = 0; i < count && off + 2 <= sizeof buf; i++) {
        SfxWire::putU16LE(&buf[off], role.channel_us((uint8_t)(i + 1)));
        off += 2;
    }
    _ctx->sendRawPacket(RolePacket::JETIEX_FRAME_BROADCAST, SfxWire::TAG_ASYNC, buf, off);
    fireLocalAsync(RolePacket::JETIEX_FRAME_BROADCAST, buf, off);
}

}  // namespace sfx_core
