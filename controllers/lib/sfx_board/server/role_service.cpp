/*
 * RoleServicePolicy implementation — attach / detach + per-role dispatch.
 */

#include "role_service.h"
#include <platform/sfx_platform.h>   // SFX_MILLIS()

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
        case RolePacket::SERVO_RECOIL:          handleServoRecoil(p, len);          break;
        case RolePacket::SERVO_SET_BROADCAST_HZ: handleServoSetBroadcastHz(p, len); break;
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
        // BiMotor packets that spilled out of 0x68..0x6F into the spare
        // slots at the top of LED + Heater ranges (range exhausted).
        case RolePacket::BIMOTOR_MOVE_TO_END:   handleBiMotorMoveToEnd(p, len);     break;
        case RolePacket::BIMOTOR_SET_GUARD:     handleBiMotorSetGuard(p, len);      break;

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
    const uint32_t now = SFX_MILLIS();

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

    // Generic servo telemetry — one batched snapshot of every active servo at
    // the host-requested rate, gated on a listening host.  Upload-safe: this
    // whole update() is skipped while the loop is upload-exclusive.
    if (_servoBroadcastHz > 0 && (int32_t)(now - _servoBroadcastNextMs) >= 0) {
        _servoBroadcastNextMs = now + (uint32_t)(1000u / _servoBroadcastHz);
        if (_ctx && _ctx->hostVerboseActive()) emitServoBroadcast(now);
    }
}

void RoleServicePolicy::handleServoSetBroadcastHz(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    uint8_t hz = p[0];
    if (hz > kServoBroadcastMaxHz) hz = kServoBroadcastMaxHz;
    _servoBroadcastHz     = hz;
    _servoBroadcastNextMs = SFX_MILLIS();   // emit promptly on (re)subscribe
    _ctx->sendAck();
}

void RoleServicePolicy::emitServoBroadcast(uint32_t now) {
    (void)now;
    if (!_reg) return;
    // [count:u8]{ [portIdx:u8][pos:u16][target:u16][vel:i16] } × count.
    // Buffer sized for the worst case (numServoPorts ≤ the board's servo count;
    // HubFX has ≤ 10 → 1 + 10·7 = 71 B, well under the payload cap).
    uint8_t buf[1 + 32 * 7];
    size_t  off = 1;
    uint8_t count = 0;
    const uint8_t n = _reg->numServoPorts();
    for (uint8_t i = 0; i < n && count < 32; i++) {
        auto* b = _reg->servoAt(i);
        if (!b || !b->occupied()) continue;
        auto* r = std::get_if<ServoActuatorRole>(&b->role);
        if (!r) continue;
        if (off + 7 > sizeof buf) break;
        buf[off++] = i;
        SfxWire::putU16LE(&buf[off], r->position());          off += 2;
        SfxWire::putU16LE(&buf[off], r->target());            off += 2;
        SfxWire::putI16LE(&buf[off], r->velocity_us_per_s()); off += 2;
        count++;
    }
    if (count == 0) return;            // nothing to report — stay off the wire
    buf[0] = count;
    if (_ctx) _ctx->sendRawPacket(RolePacket::SERVO_MOTION_UPDATE,
                                  SfxWire::TAG_ASYNC, buf, off);
    fireLocalAsync(RolePacket::SERVO_MOTION_UPDATE, buf, off);
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
                case RoleKind::JetiExTelemetry: ok = attachJetiExTelemetry(*b, portIdx, cfg, cfgLen); break;
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
#if SFX_PLATFORM_ESP32
            // The JetiExpander owns BOTH Jeti links; detaching the Rx (IN_1)
            // role tears it down (end() releases both ports) AND clears the
            // paired downstream telemetry marker so IN_2 reverts to no role.
            if (std::holds_alternative<JetiExInputRole>(b->role)) {
                JetiEx::JetiExpander::instance().end();
                for (uint8_t i = 0; i < _reg->numInputPorts(); ++i) {
                    if (i == portIdx) continue;
                    auto* ob = _reg->inputAt(i);
                    if (ob && std::holds_alternative<JetiExTelemetryRole>(ob->role))
                        ob->role.emplace<std::monostate>();
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
        else if (std::holds_alternative<JetiExTelemetryRole>(b->role)) rk = RoleKind::JetiExTelemetry;
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
    // Default 50 Hz so the InputDispatcher (and thus effects) get RC values
    // from boot even with no host connected — field operation has no Studio.
    // The WIRE broadcast is gated on a listening host (hostVerboseActive), so
    // a non-zero default doesn't stream into a dead port; only the LOCAL
    // dispatch runs.  A host can still override the rate via Set*BroadcastHz.
    // Wire broadcast stays OFF at attach (no host yet); the LOCAL effect feed
    // runs at the role's fixed 50 Hz default so the model flies standalone.  A
    // host subscribes to the wire via Set*BroadcastHz when it opens the live-
    // channel view (the config byte no longer auto-enables the wire — that
    // flooded a connected-but-not-viewing host).
    (void)cfg; (void)cfgLen;
    role.onBroadcast([this, portIdx](uint8_t /*count*/, bool /*valid*/) {
        // Rebuild the full PPM channel frame from the role each tick
        // (mirrors the SBUS / Jeti pattern).
        auto* binding = _reg->inputAt(portIdx);
        if (!binding) return;
        if (auto* r = std::get_if<RcPwmInputRole>(&binding->role)) {
            emitPpmFrameBroadcast(portIdx, *r);
        }
    });
    return true;
}

bool RoleServicePolicy::attachSbusInput(InputBinding& b, uint8_t portIdx,
                                        const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<SbusInputRole>();
    if (!role.bind(b.port)) { b.role.emplace<std::monostate>(); return false; }
    // Optional config: [broadcastHz:u8]
    // Default 50 Hz so the InputDispatcher (and thus effects) get RC values
    // from boot even with no host connected — field operation has no Studio.
    // The WIRE broadcast is gated on a listening host (hostVerboseActive), so
    // a non-zero default doesn't stream into a dead port; only the LOCAL
    // dispatch runs.  A host can still override the rate via Set*BroadcastHz.
    // Wire broadcast stays OFF at attach (no host yet); the LOCAL effect feed
    // runs at the role's fixed 50 Hz default so the model flies standalone.  A
    // host subscribes to the wire via Set*BroadcastHz when it opens the live-
    // channel view (the config byte no longer auto-enables the wire — that
    // flooded a connected-but-not-viewing host).
    (void)cfg; (void)cfgLen;
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
    // Optional config: [broadcastHz:u8][baudHi:u8][baudLo:u8][downstream:u8]
    //   baud encoded as kbaud (125 / 250); 0 = use default 125 000.
    //   downstream (Rule 11 append, byte 3): bring up the IN_2 / ESC telemetry
    //   monitor.  Default false — IN_2 stays passive (no UART drained).
    uint32_t baud = 125000;
    if (cfgLen >= 3) {
        const uint16_t kbaud = ((uint16_t)cfg[1] << 8) | cfg[2];
        if (kbaud == 250) baud = 250000;
        else if (kbaud == 125 || kbaud == 0) baud = 125000;
        else baud = (uint32_t)kbaud * 1000;
    }
    const bool useDownstream = (cfgLen >= 4) && (cfg[3] != 0);
    if (!role.bind(b.port, baud)) { b.role.emplace<std::monostate>(); return false; }

#if SFX_PLATFORM_ESP32
    // Start the board-unique JetiExpander on BOTH Jeti links: this port (IN_1,
    // Rx side, we are slave) + the other input port (IN_2, downstream ESC side,
    // we are master) if the board declares one.  The expander (a Core-0 task)
    // owns the UART I/O for both; the roles are thin handles, so they never
    // double-drive the UART.  Protocol-gated: only JetiEX attach starts it.
    sfx_peripherals::InputPort* escPort   = nullptr;
    InputBinding*               escBind   = nullptr;
    uint8_t                     escIdx    = 0xFF;
    for (uint8_t i = 0; i < _reg->numInputPorts(); ++i) {
        if (i == portIdx) continue;
        auto* ob = _reg->inputAt(i);
        if (ob && ob->port) { escPort = ob->port; escBind = ob; escIdx = i; break; }
    }
    JetiEx::JetiExpander::instance().begin(b.port, escPort,
                                           /*usn=*/0xA400, /*lsn=*/0x0100, "HubFx", baud,
                                           /*useDownstream=*/useDownstream);

    // Reflect the IN_1→IN_2 pairing in the registry: stamp the downstream port
    // with the JetiExTelemetry role so topology (and thus the Studio diagram)
    // shows IN_2 as the expander's telemetry link — regardless of whether the
    // operator set IN_1 via Studio or /hubfx.yaml.  The expander owns the UART;
    // this role is just the marker.  (Idempotent — skip if already telemetry.)
    if (escBind && escIdx != 0xFF &&
        !std::holds_alternative<JetiExTelemetryRole>(escBind->role)) {
        auto& tr = escBind->role.emplace<JetiExTelemetryRole>();
        tr.bind(escBind->port, baud);
        tr.setPortIdx(escIdx);
    }
#endif

    // Default 50 Hz so the InputDispatcher (and thus effects) get RC values
    // from boot even with no host connected — field operation has no Studio.
    // The WIRE broadcast is gated on a listening host (hostVerboseActive), so
    // a non-zero default doesn't stream into a dead port; only the LOCAL
    // dispatch runs.  A host can still override the rate via Set*BroadcastHz.
    // Wire broadcast stays OFF at attach (no host yet); the LOCAL effect feed
    // runs at the role's fixed 50 Hz default so the model flies standalone.  A
    // host subscribes to the wire via Set*BroadcastHz when it opens the live-
    // channel view (the config byte no longer auto-enables the wire — that
    // flooded a connected-but-not-viewing host).
    (void)cfg; (void)cfgLen;
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

bool RoleServicePolicy::attachJetiExTelemetry(InputBinding& b, uint8_t portIdx,
                                              const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<JetiExTelemetryRole>();
    // Optional config: [broadcastHz:u8][baudHi:u8][baudLo:u8] — same encoding
    // as the Jeti EX input role; 0 = default 125 000.
    uint32_t baud = 125000;
    if (cfgLen >= 3) {
        const uint16_t kbaud = ((uint16_t)cfg[1] << 8) | cfg[2];
        if (kbaud == 250) baud = 250000;
        else if (kbaud == 125 || kbaud == 0) baud = 125000;
        else baud = (uint32_t)kbaud * 1000;
    }
    if (!role.bind(b.port, baud)) { b.role.emplace<std::monostate>(); return false; }
    role.setPortIdx(portIdx);   // for the SFX_INSTRUMENTATION [jtelem] diag log
    // Monitor-only this phase: the role decodes downstream telemetry into the
    // shared JetiTelemetryHub; the master channel's responder (phase 2) serves
    // it to the Rx.  No wire broadcast — health is visible via the gated
    // [jtelem] log on the diag stream.
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

    // Attach config layout — Rule 11 append-only.  Older masters send
    // only the first 4 bytes (Fixed-mode threshold + window); a Strategy A
    // master appends the LiveRatio config + initial position.
    //
    //   [0..3]  Fixed-mode:  threshold_mA:u16LE | window_ms:u16LE
    //   [4]     guardMode:u8  (0=Fixed, 1=LiveRatio)  -- Rule 11 ext
    //   [5..6]  a:u16LE       -- LiveRatio: ratio_x100 (e.g. 250 = 2.5×)
    //   [7..8]  b:u16LE       -- LiveRatio: runSample_ms
    //   [9..10] c:u16LE       -- LiveRatio: inrushBlank_ms
    //   [11..12] d:u16LE      -- LiveRatio: maxTravel_ms (failsafe, 0 = none)
    //   [13]    initialPosition:u8  (0=Unknown, 1=A, 2=B) -- Strategy A restore
    if (cfgLen >= 4) {
        const uint16_t th = SfxWire::getU16LE(&cfg[0]);
        const uint16_t wn = SfxWire::getU16LE(&cfg[2]);
        role.setStallGuard(th, wn);
    }
    if (cfgLen >= 13) {
        const uint8_t  mode = cfg[4];
        const uint16_t a    = SfxWire::getU16LE(&cfg[5]);
        const uint16_t bv   = SfxWire::getU16LE(&cfg[7]);
        const uint16_t cv   = SfxWire::getU16LE(&cfg[9]);
        const uint16_t dv   = SfxWire::getU16LE(&cfg[11]);
        if (mode == 1) role.setStallGuardRatio(a, bv, cv, dv);
        // mode == 0 keeps Fixed-mode config from the first 4 bytes.
    }
    if (cfgLen >= 14) {
        role.setPosition(static_cast<BiDcMotorRole::Position>(cfg[13]));
    }

    role.onStall([this, portIdx](uint16_t peak, uint16_t dur) {
        emitBiMotorStallEvent(portIdx, peak, dur);
    });
    role.onEndstopResult([this, portIdx](uint8_t outcome, uint16_t travel, uint16_t peak,
                                         BiDcMotorRole::Position pos) {
        emitBiMotorEndstopResult(portIdx, outcome, travel, peak,
                                 static_cast<uint8_t>(pos));
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

void RoleServicePolicy::handleServoRecoil(const uint8_t* p, size_t len) {
    // Add a transient recoil offset on top of the aim for `durationMs`, then
    // auto-remove ("de-jerk"). The motion profile keeps tracking its target
    // underneath, so the kick works whether the servo is moving or stationary.
    // GunFx calls this once per shot with a random ± offset.
    // Payload: [portIdx:u8][offsetUs:i16LE][durationMs:u16LE]
    if (len < 5) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t  idx        = p[0];
    const int16_t  offsetUs   = SfxWire::getI16LE(&p[1]);
    const uint16_t durationMs = SfxWire::getU16LE(&p[3]);
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (auto* r = std::get_if<ServoActuatorRole>(&b->role)) {
        r->applyRecoil(offsetUs, durationMs);
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
    // Rule 11 extension: append [position][guardMode] tail.  Older
    // masters parse only the first 8 bytes; newer masters see the
    // Strategy A position + active stall-detection mode.
    uint8_t out[10];
    out[0] = idx;
    SfxWire::putI16LE(&out[1], r->signedDuty());
    SfxWire::putI16LE(&out[3], r->voltage_mV());
    SfxWire::putI16LE(&out[5], r->current_mA());
    out[7] = r->stalled() ? 1 : 0;
    out[8] = static_cast<uint8_t>(r->position());
    out[9] = static_cast<uint8_t>(r->guardMode());
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

// Strategy A move-to-end: like SEEK_ENDSTOP but records `endLabel` as
// the destination so the role's position state advances on Reached.
// `signed_duty == 0` is the position-restore special case (no motion).
void RoleServicePolicy::handleBiMotorMoveToEnd(const uint8_t* p, size_t len) {
    if (len < 6) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    const auto    end     = static_cast<BiDcMotorRole::Position>(p[1]);
    const int16_t duty    = (int16_t)SfxWire::getI16LE(&p[2]);
    const uint16_t timeout = SfxWire::getU16LE(&p[4]);
    r->moveToEnd(end, duty, timeout);
    _ctx->sendAck();
}

// Live retune of stall-guard mode + parameters.  Used by the Studio
// calibration dialog when the operator switches between Fixed and
// LiveRatio without wanting to re-attach the role (which would erase
// the cached position + any in-progress seek state).
void RoleServicePolicy::handleBiMotorSetGuard(const uint8_t* p, size_t len) {
    if (len < 12) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    const uint8_t  mode      = p[1];
    const uint16_t window_ms = SfxWire::getU16LE(&p[2]);
    const uint16_t a         = SfxWire::getU16LE(&p[4]);
    const uint16_t bv        = SfxWire::getU16LE(&p[6]);
    const uint16_t cv        = SfxWire::getU16LE(&p[8]);
    const uint16_t dv        = SfxWire::getU16LE(&p[10]);
    if (mode == 0) {
        r->setStallGuard(a, window_ms != 0 ? window_ms : r->windowMs());
    } else if (mode == 1) {
        r->setStallGuardRatio(a, bv, cv, dv);
        if (window_ms != 0) r->setStallWindowMs(window_ms);
    } else {
        _ctx->sendNack(RoleError::ROLE_CONFIG_INVALID);
        return;
    }
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
                                                 uint16_t travel_ms, uint16_t peak_mA,
                                                 uint8_t position) {
    // Rule 11 extension — trailing position byte.  Older masters parse
    // the first 6 bytes and ignore the tail; newer masters see the
    // resulting endstop label (Strategy A) without a follow-up STATUS.
    uint8_t buf[7];
    buf[0] = portIdx;
    buf[1] = outcome;
    SfxWire::putU16LE(&buf[2], travel_ms);
    SfxWire::putU16LE(&buf[4], peak_mA);
    buf[6] = position;
    _ctx->sendRawPacket(RolePacket::BIMOTOR_ENDSTOP_RESULT, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::BIMOTOR_ENDSTOP_RESULT, buf, sizeof buf);
}

void RoleServicePolicy::emitPpmFrameBroadcast(uint8_t portIdx, const RcPwmInputRole& role) {
    // [portIdx:u8][count:u8][valid:u8][channels:u16LE × count] — same shape
    // as the Jeti EX frame so the master's input dispatcher / Go decoder
    // treat all framed inputs uniformly.  PPM carries up to 24 channels.
    const uint8_t count = role.channelCount();
    uint8_t buf[3 + RcPwmInputRole::kMaxChannels * 2];
    buf[0] = portIdx;
    buf[1] = count;
    buf[2] = role.valid() ? 1 : 0;
    size_t off = 3;
    for (uint8_t i = 0; i < count && off + 2 <= sizeof buf; i++) {
        SfxWire::putU16LE(&buf[off], role.channel_us((uint8_t)(i + 1)));
        off += 2;
    }
    // Wire broadcast = host live-view only; gate on a listening host so we
    // don't stream into a dead port.  The LOCAL dispatch ALWAYS fires — it
    // feeds the on-board InputDispatcher that drives effects, which must run
    // standalone with no host connected.
    if (_ctx && _ctx->hostVerboseActive() && role.wireEnabled())
        _ctx->sendRawPacket(RolePacket::PPM_FRAME_BROADCAST, SfxWire::TAG_ASYNC, buf, off);
    fireLocalAsync(RolePacket::PPM_FRAME_BROADCAST, buf, off);
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
    if (_ctx && _ctx->hostVerboseActive() && role.wireEnabled())  // wire = host live-view; local always
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
    if (_ctx && _ctx->hostVerboseActive() && role.wireEnabled())  // wire = host live-view; local always
        _ctx->sendRawPacket(RolePacket::JETIEX_FRAME_BROADCAST, SfxWire::TAG_ASYNC, buf, off);
    fireLocalAsync(RolePacket::JETIEX_FRAME_BROADCAST, buf, off);
}

}  // namespace sfx_core
