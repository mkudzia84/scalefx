/*
 * RoleServicePolicy implementation — DISPATCH + LIFECYCLE core.
 *
 * This TU keeps only the role layer's cross-family duties: the 0x40..0x7F
 * packet router, the per-loop tick, and the registry-mutation surface
 * (attach / bulk-attach / detach / list + the applyAttach port-kind router).
 * Every per-family command/query body lives in its own handler TU
 * (role_servo_handler.cpp, role_led_handler.cpp, …); async telemetry-out
 * lives in role_event_emitter.cpp.
 */

#include "role_service.h"
#include "role_registry.h"           // roleKindOf / forEachAttachedRole — single role-kind map
#include "effect_clock.h"            // sfx_core::EffectClock — shared synchronised clock
#include <platform/sfx_platform.h>   // SFX_MILLIS()

#include <cstring>
#include <variant>

#if SFX_PLATFORM_ESP32
#  include <jeti_ex/jeti_expander.h>   // downstream UART handoff on esc-telemetry attach
#endif

namespace sfx_core {

// ── Top-level dispatch ──────────────────────────────────────────────

CommandHandleResult RoleServicePolicy::handle(uint8_t type, const uint8_t* p, size_t len) {
    if (!_reg) { _ctx->sendNack(SerialError::INTERNAL_ERROR); return CommandHandleResult::Handled; }

    switch (type) {
        case RolePacket::ROLE_ATTACH:           handleAttach(p, len);               break;
        case RolePacket::ROLE_BULK_ATTACH:      handleBulkAttach(p, len);           break;
        case RolePacket::ROLE_DETACH:           handleDetach(p, len);               break;
        case RolePacket::ROLE_LIST_REQ:         handleList();                       break;

        // Servo actuator
        case RolePacket::SERVO_SET_TARGET:      _servo.handleSetTarget(p, len);     break;
        case RolePacket::SERVO_SET_POS_NORM:    _servo.handleSetPosNorm(p, len);    break;
        case RolePacket::SERVO_RECOIL:          _servo.handleRecoil(p, len);        break;
        case RolePacket::SERVO_SET_BROADCAST_HZ: _servo.handleSetBroadcastHz(p, len); break;
        case RolePacket::SERVO_GET_STATUS_REQ:  _servo.handleGetStatusReq(p, len);  break;
        case RolePacket::SERVO_SET_PROFILE:     _servo.handleSetProfile(p, len);    break;
        case RolePacket::SERVO_GET_PROFILE_REQ: _servo.handleGetProfileReq(p, len); break;

        // RC PWM input
        case RolePacket::RCIN_GET_VALUE_REQ:    _rcpwm.handleGetValueReq(p, len);    break;
        case RolePacket::RCIN_SET_BROADCAST_HZ: _rcpwm.handleSetBroadcastHz(p, len); break;

        // LED animator
        case RolePacket::LED_QUEUE_LOAD:        _led.handleQueueLoad(p, len);       break;
        case RolePacket::LED_START:             _led.handleStart(p, len);           break;
        case RolePacket::LED_STOP:              _led.handleStop(p, len);            break;
        case RolePacket::LED_SET_BRIGHTNESS:    _led.handleSetBrightness(p, len);   break;
        case RolePacket::LED_GET_STATUS_REQ:    _led.handleGetStatusReq(p, len);    break;

        // DC motor
        case RolePacket::MOTOR_SET_DUTY:        _motor.handleSetDuty(p, len);       break;
        case RolePacket::MOTOR_BRAKE:           _motor.handleBrake(p, len);         break;
        case RolePacket::MOTOR_GET_STATUS_REQ:  _motor.handleGetStatusReq(p, len);  break;
        case RolePacket::MOTOR_SET_ELEMENT:     _motor.handleSetElement(p, len);    break;
        case RolePacket::MOTOR_GET_ELEMENT_REQ: _motor.handleGetElementReq(p, len); break;
        case RolePacket::MOTOR_SET_PCT:         _motor.handleSetPct(p, len);        break;

        // Bi-directional motor
        case RolePacket::BIMOTOR_SET_SIGNED:    _bimotor.handleSetSigned(p, len);   break;
        case RolePacket::BIMOTOR_BRAKE:         _bimotor.handleBrake(p, len);       break;
        case RolePacket::BIMOTOR_COAST:         _bimotor.handleCoast(p, len);       break;
        case RolePacket::BIMOTOR_GET_STATUS_REQ:_bimotor.handleGetStatus(p, len);   break;
        case RolePacket::BIMOTOR_SEEK_ENDSTOP:  _bimotor.handleSeekEndstop(p, len); break;
        // BiMotor packets that spilled out of 0x68..0x6F into the spare
        // slots at the top of LED + Heater ranges (range exhausted).
        case RolePacket::BIMOTOR_MOVE_TO_END:   _bimotor.handleMoveToEnd(p, len);   break;
        case RolePacket::BIMOTOR_SET_GUARD:     _bimotor.handleSetGuard(p, len);    break;

        // Heater
        case RolePacket::HEATER_SET_TARGET:     _heater.handleSetTarget(p, len);    break;
        case RolePacket::HEATER_GET_STATUS_REQ: _heater.handleGetStatus(p, len);    break;
        case RolePacket::HEATER_SET_ELEMENT:    _heater.handleSetElement(p, len);   break;
        case RolePacket::HEATER_GET_ELEMENT_REQ:_heater.handleGetElementReq(p, len);break;

        // SBUS input
        case RolePacket::SBUS_GET_FRAME_REQ:    _sbus.handleGetFrameReq(p, len);    break;
        case RolePacket::SBUS_SET_BROADCAST_HZ: _sbus.handleSetBroadcastHz(p, len); break;

        // Jeti EX input
        case RolePacket::JETIEX_GET_FRAME_REQ:    _jeti.handleGetFrameReq(p, len);    break;
        case RolePacket::JETIEX_SET_BROADCAST_HZ: _jeti.handleSetBroadcastHz(p, len); break;

        default:                                return CommandHandleResult::NotMyCommand;
    }
    return CommandHandleResult::Handled;
}

// ── Per-loop tick ────────────────────────────────────────────────────

void RoleServicePolicy::update() {
    if (!_reg) return;

    // One shared clock for the whole pass — every LedAnimator samples the
    // SAME instant, so multi-channel light programs stay phase-locked
    // regardless of how long the per-channel I²C writes take or how jittery the
    // main loop is.  Sourced from the EffectClock (latched once per process()
    // pass) — NOT raw SFX_MILLIS() — so LED animation is synchronised with
    // every OTHER effect/role on the same clock (servo motion, engine, etc.),
    // not just phase-locked among LEDs.  (See LedAnimator::tick.)
    const uint32_t now = sfx_core::EffectClock::instance().nowMs();

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

    // Generic servo telemetry — schedule + emit owned by the servo handler;
    // upload-safe because this whole update() is skipped while the loop is
    // upload-exclusive.
    _servo.maybeBroadcast(now);
}

// ── ROLE_ATTACH / ROLE_DETACH / ROLE_LIST ───────────────────────────

// applyAttach attaches ONE role without touching the wire (no ACK/NACK).
// Returns 0 on success (and emits ROLE_ATTACHED), else the wire error code.
// Shared by handleAttach (single, ACKs the result) and handleBulkAttach (many,
// one ACK for the batch).  Routes the (portKind, roleKind) pair to the right
// family handler's attach(), so the per-kind construction lives with the
// family it builds.
uint8_t RoleServicePolicy::applyAttach(uint8_t portKind, uint8_t portIdx,
                                       uint8_t roleKind, const uint8_t* cfg,
                                       uint8_t cfgLen) {
    bool ok = false;
    switch (portKind) {
        case PortKind::Servo: {
            auto* b = _reg->servoAt(portIdx);
            if (!b || !b->occupied()) return PortError::PORT_NOT_FOUND;
            switch (roleKind) {
                case RoleKind::ServoActuator: ok = _servo.attach(*b, portIdx, cfg, cfgLen); break;
                default: return RoleError::ROLE_KIND_NOT_SUPPORTED;
            }
            break;
        }
        case PortKind::Pwm: {
            auto* b = _reg->pwmAt(portIdx);
            if (!b || !b->occupied()) return PortError::PORT_NOT_FOUND;
            switch (roleKind) {
                case RoleKind::LedAnimator: ok = _led.attach   (*b, portIdx, cfg, cfgLen); break;
                case RoleKind::DcMotor:     ok = _motor.attach (*b, portIdx, cfg, cfgLen); break;
                case RoleKind::Heater:      ok = _heater.attach(*b, portIdx, cfg, cfgLen); break;
                default: return RoleError::ROLE_KIND_NOT_SUPPORTED;
            }
            break;
        }
        case PortKind::HBridge: {
            auto* b = _reg->hbridgeAt(portIdx);
            if (!b || !b->occupied()) return PortError::PORT_NOT_FOUND;
            switch (roleKind) {
                case RoleKind::BiDcMotor: ok = _bimotor.attach(*b, portIdx, cfg, cfgLen); break;
                default: return RoleError::ROLE_KIND_NOT_SUPPORTED;
            }
            break;
        }
        case PortKind::Input: {
            auto* b = _reg->inputAt(portIdx);
            if (!b || !b->occupied()) return PortError::PORT_NOT_FOUND;
            switch (roleKind) {
                case RoleKind::RcPwmInput:  ok = _rcpwm.attach(*b, portIdx, cfg, cfgLen); break;
                case RoleKind::SbusInput:   ok = _sbus.attach (*b, portIdx, cfg, cfgLen); break;
                case RoleKind::JetiExInput: ok = _jeti.attachInput(*b, portIdx, cfg, cfgLen); break;
                case RoleKind::EscTelemetry: {
                    // [protocol:u8][baudKHi:u8][baudKLo:u8][ratioHi:u8][ratioLo:u8]
                    // (Rule 11 append-only) — zero-length = legacy jeti-exbus
                    // downstream marker (old yaml files).  ratio = RPM divider
                    // ×100 (pole pairs × gearbox); 0 = 1.00.
                    const uint8_t proto = (cfgLen >= 1) ? cfg[0]
                                          : EscTelemetryRole::kProtoJetiExBus;
                    uint32_t baud = 0;
                    if (cfgLen >= 3) baud = (uint32_t)(((uint16_t)cfg[1] << 8) | cfg[2]) * 1000u;
                    uint16_t ratioX100 = 0;
                    if (cfgLen >= 5) ratioX100 = (uint16_t)(((uint16_t)cfg[3] << 8) | cfg[4]);
#if SFX_PLATFORM_ESP32
                    // UART handoff with a running JetiExpander: a NATIVE
                    // protocol takes the port away from the expander's EX
                    // downstream link (else both drain the same UART and the
                    // expander's poll TX walks over the ESC's stream); the
                    // jeti-exbus marker offers it (back).  Must happen BEFORE
                    // bind() so the native UART config is applied last.
                    {
                        auto& jx = JetiEx::JetiExpander::instance();
                        if (jx.running()) {
                            if (proto != EscTelemetryRole::kProtoJetiExBus) {
                                if (jx.downstreamPort() == b->port)
                                    jx.setDownstreamPort(nullptr);
                            } else {
                                jx.setDownstreamPort(b->port);
                            }
                        }
                    }
#endif
                    auto& er = b->role.emplace<EscTelemetryRole>();
                    if (!er.bind(b->port, proto, baud, ratioX100)) {
                        b->role.emplace<std::monostate>();
                        ok = false;
                    } else {
                        er.setPortIdx(portIdx);
                        ok = true;
                    }
                    break;
                }
                default: return RoleError::ROLE_KIND_NOT_SUPPORTED;
            }
            break;
        }
        default: return PortError::PORT_NOT_FOUND;
    }
    if (!ok) return RoleError::ROLE_CONFIG_INVALID;
    _emit.emitRoleAttached(portKind, portIdx, roleKind);
    return 0;
}

void RoleServicePolicy::handleAttach(const uint8_t* p, size_t len) {
    if (len < 4) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t cfgLen = p[3];
    if (len < (size_t)4 + cfgLen) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t err = applyAttach(p[0], p[1], p[2], &p[4], cfgLen);
    if (err == 0) _ctx->sendAck();
    else          _ctx->sendNack(err);
}

// handleBulkAttach applies a FULL role set in one packet — the declarative
// expander-bringup path (the hub pushes the board's whole /hubfx.yaml role
// config at connect instead of N racy single attaches).  Payload:
//   [count:u8] { [portKind][portIdx][roleKind][cfgLen][cfg:cfgLen] } × count
// Each entry applies via applyAttach (emitting ROLE_ATTACHED so the hub roster
// updates).  ONE ACK for the batch — partial per-entry failures are logged but
// don't fail bringup (a missing/incompatible port shouldn't wedge the board).
void RoleServicePolicy::handleBulkAttach(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t count = p[0];
    size_t off = 1;
    uint8_t applied = 0, failed = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (off + 4 > len) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
        const uint8_t portKind = p[off], portIdx = p[off + 1],
                      roleKind = p[off + 2], cfgLen = p[off + 3];
        off += 4;
        if (off + cfgLen > len) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
        const uint8_t err = applyAttach(portKind, portIdx, roleKind, &p[off], cfgLen);
        off += cfgLen;
        if (err == 0) ++applied;
        else {
            ++failed;
            SFX_LOG_WARN("[role] bulk attach #%u {kind=%u idx=%u}->%u err=0x%02X",
                         (unsigned)i, (unsigned)portKind, (unsigned)portIdx,
                         (unsigned)roleKind, (unsigned)err);
        }
    }
    _ctx->sendAck();
    SFX_LOG_INFO("[role] bulk role-config applied: %u ok, %u failed",
                 (unsigned)applied, (unsigned)failed);
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
#if SFX_PLATFORM_ESP32
            // The JetiExpander owns BOTH Jeti links; detaching the Rx (IN_1)
            // role tears it down (end() releases both ports) AND clears the
            // paired downstream telemetry marker so IN_2 reverts to no role.
            if (std::holds_alternative<JetiExInputRole>(b->role)) {
                JetiEx::JetiExpander::instance().end();
                for (uint8_t i = 0; i < _reg->numInputPorts(); ++i) {
                    if (i == portIdx) continue;
                    auto* ob = _reg->inputAt(i);
                    if (ob) {
                        auto* er = std::get_if<EscTelemetryRole>(&ob->role);
                        if (er && er->protocol() == EscTelemetryRole::kProtoJetiExBus)
                            ob->role.emplace<std::monostate>();   // clear the pairing marker only
                    }
                }
            }
#endif
            // Release the peripheral the previous role had claimed.
            if (b->port) b->port->disable();
            b->role.emplace<std::monostate>();
            break;
        }
        default: _ctx->sendNack(PortError::PORT_NOT_FOUND); return;
    }
    _ctx->sendAck();
    _emit.emitRoleDetached(portKind, portIdx);
}

void RoleServicePolicy::handleList() {
    // [count:u8] × [portKind, portIdx, roleKind, flags]
    uint8_t buf[1 + 32*4];
    size_t  off = 1;
    uint8_t count = 0;

    // One walk; the role-type → RoleKind mapping lives in role_registry.h
    // (forEachAttachedRole), not hand-rolled here. Rule 58.
    forEachAttachedRole(*_reg, [&](uint8_t portKind, uint8_t portIdx, uint8_t roleKind) {
        if (off + 4 > sizeof buf) return;
        buf[off++] = portKind;
        buf[off++] = portIdx;
        buf[off++] = roleKind;
        buf[off++] = 0;             // flags reserved
        count++;
    });

    buf[0] = count;
    _ctx->sendRawPacket(RolePacket::ROLE_LIST_RESP, _ctx->currentTag(), buf, off);
}

}  // namespace sfx_core
