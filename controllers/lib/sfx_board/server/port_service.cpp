/*
 * PortServicePolicy implementation.
 */

#include "port_service.h"

#include <cstring>

namespace sfx_core {

CommandHandleResult PortServicePolicy::handle(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_reg) {
        _ctx->sendNack(SerialError::INTERNAL_ERROR);
        return CommandHandleResult::Handled;
    }

    switch (type) {
        case PortPacket::PORT_LIST_REQ:        handlePortListReq();              break;

        case PortPacket::SERVO_PORT_SET_US:    handleServoSetUs(payload, len);   break;
        case PortPacket::SERVO_PORT_READ_US:   handleServoReadUs(payload, len);  break;

        case PortPacket::PWM_PORT_SET_DUTY:    handlePwmSetDuty(payload, len);   break;
        case PortPacket::PWM_PORT_SET_FREQ:    handlePwmSetFreq(payload, len);   break;
        case PortPacket::PWM_PORT_READ_SENSE:  handlePwmReadSense(payload, len); break;

        case PortPacket::HBRIDGE_SET_SIGNED:   handleHBridgeSetSigned(payload, len);  break;
        case PortPacket::HBRIDGE_BRAKE:        handleHBridgeBrake(payload, len);      break;
        case PortPacket::HBRIDGE_COAST:        handleHBridgeCoast(payload, len);      break;
        case PortPacket::HBRIDGE_READ_SENSE:   handleHBridgeReadSense(payload, len);  break;

        default:                               return CommandHandleResult::NotMyCommand;
    }
    return CommandHandleResult::Handled;
}

// ── Enumeration ──────────────────────────────────────────────────────

void PortServicePolicy::handlePortListReq() {
    // Wire layout: [numServo:u8] × [idx:u8][capFlags:u8]
    //              [numPwm:u8]   × [idx:u8][senseFlags:u8]
    //              [numHBridge:u8] × [idx:u8][senseFlags:u8]
    uint8_t buf[1 + 16*2 + 1 + 16*2 + 1 + 16*2];
    size_t  off = 0;

    const uint8_t ns = _reg->numServoPorts();
    buf[off++] = ns;
    for (uint8_t i = 0; i < ns; i++) {
        const ServoBinding* b = _reg->servoAt(i);
        if (!b || !b->occupied()) break;
        buf[off++] = i;
        uint8_t flags = ServoPortFlags::EMITS;
        if (b->port->supportsInput()) flags |= ServoPortFlags::SAMPLES;
        buf[off++] = flags;
    }

    const uint8_t np = _reg->numPwmPorts();
    buf[off++] = np;
    for (uint8_t i = 0; i < np; i++) {
        const PwmBinding* b = _reg->pwmAt(i);
        if (!b || !b->occupied()) break;
        buf[off++] = i;
        uint8_t flags = 0;
        if (b->vSense) flags |= PortSenseFlags::VOLTAGE;
        if (b->iSense) flags |= PortSenseFlags::CURRENT;
        if (b->tSense) flags |= PortSenseFlags::TEMPERATURE;
        buf[off++] = flags;
    }

    const uint8_t nh = _reg->numHBridgePorts();
    buf[off++] = nh;
    for (uint8_t i = 0; i < nh; i++) {
        const HBridgeBinding* b = _reg->hbridgeAt(i);
        if (!b || !b->occupied()) break;
        buf[off++] = i;
        uint8_t flags = 0;
        if (b->vSense) flags |= PortSenseFlags::VOLTAGE;
        if (b->iSense) flags |= PortSenseFlags::CURRENT;
        if (b->tSense) flags |= PortSenseFlags::TEMPERATURE;
        buf[off++] = flags;
    }

    _ctx->sendRawPacket(PortPacket::PORT_LIST_RESP, _ctx->currentTag(), buf, off);
}

// ── Servo raw ────────────────────────────────────────────────────────

void PortServicePolicy::handleServoSetUs(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t  idx = p[0];
    const uint16_t us  = SfxWire::getU16LE(&p[1]);
    ServoBinding* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (b->hasRole())         { _ctx->sendNack(PortError::PORT_HAS_ROLE);  return; }
    b->port->writeMicroseconds(us);
    _ctx->sendAck();
}

void PortServicePolicy::handleServoReadUs(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    ServoBinding* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    uint16_t us = 0;
    bool valid  = false;
    if (b->port->supportsInput()) {
        valid = b->port->readMicroseconds(&us);
    } else {
        // Output mode — report last commanded.
        us    = b->port->microseconds();
        valid = true;
    }
    uint8_t out[4];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], us);
    out[3] = valid ? 1 : 0;
    _ctx->sendRawPacket(PortPacket::SERVO_PORT_READ_RESP, _ctx->currentTag(), out, sizeof out);
}

// ── PWM raw ──────────────────────────────────────────────────────────

void PortServicePolicy::handlePwmSetDuty(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t  idx  = p[0];
    const uint16_t duty = SfxWire::getU16LE(&p[1]);
    PwmBinding* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (b->hasRole())         { _ctx->sendNack(PortError::PORT_HAS_ROLE);  return; }
    b->port->setDuty(duty);
    _ctx->sendAck();
}

void PortServicePolicy::handlePwmSetFreq(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t  idx = p[0];
    const uint16_t hz  = SfxWire::getU16LE(&p[1]);
    PwmBinding* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (b->hasRole())         { _ctx->sendNack(PortError::PORT_HAS_ROLE);  return; }
    if (!b->port->setFrequencyHz(hz)) { _ctx->sendNack(PortError::PORT_FREQ_UNSUPPORTED); return; }
    _ctx->sendAck();
}

void PortServicePolicy::handlePwmReadSense(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    PwmBinding* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }

    uint8_t out[1 + 6];
    out[0] = idx;
    SfxWire::putI16LE(&out[1], b->vSense ? b->vSense->voltage_mV()      : 0);
    SfxWire::putI16LE(&out[3], b->iSense ? b->iSense->current_mA()      : 0);
    SfxWire::putI16LE(&out[5], b->tSense ? b->tSense->temperature_cx10() : 0);
    _ctx->sendRawPacket(PortPacket::PWM_PORT_SENSE_RESP, _ctx->currentTag(), out, sizeof out);
}

// ── H-bridge raw ─────────────────────────────────────────────────────

void PortServicePolicy::handleHBridgeSetSigned(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    const int16_t sd  = SfxWire::getI16LE(&p[1]);
    HBridgeBinding* b = _reg->hbridgeAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (b->hasRole())         { _ctx->sendNack(PortError::PORT_HAS_ROLE);  return; }
    b->port->setSigned(sd);
    _ctx->sendAck();
}

void PortServicePolicy::handleHBridgeBrake(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    HBridgeBinding* b = _reg->hbridgeAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (b->hasRole())         { _ctx->sendNack(PortError::PORT_HAS_ROLE);  return; }
    b->port->brake();
    _ctx->sendAck();
}

void PortServicePolicy::handleHBridgeCoast(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    HBridgeBinding* b = _reg->hbridgeAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (b->hasRole())         { _ctx->sendNack(PortError::PORT_HAS_ROLE);  return; }
    b->port->coast();
    _ctx->sendAck();
}

void PortServicePolicy::handleHBridgeReadSense(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    HBridgeBinding* b = _reg->hbridgeAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }

    uint8_t out[1 + 6];
    out[0] = idx;
    SfxWire::putI16LE(&out[1], b->vSense ? b->vSense->voltage_mV()      : 0);
    SfxWire::putI16LE(&out[3], b->iSense ? b->iSense->current_mA()      : 0);
    SfxWire::putI16LE(&out[5], b->tSense ? b->tSense->temperature_cx10() : 0);
    _ctx->sendRawPacket(PortPacket::HBRIDGE_SENSE_RESP, _ctx->currentTag(), out, sizeof out);
}

}  // namespace sfx_core
