/*
 * RoleEventEmitter implementation — async role telemetry serialisation.
 *
 * Bodies moved verbatim from the former RoleServicePolicy emit* helpers; the
 * only change is that fireLocalAsync() + _ctx are now the emitter's own
 * members rather than the policy's.
 */

#include "role_event_emitter.h"
#include "board_server.h"            // BoardServerBase — sendRawPacket
#include <serial/roles.h>           // RolePacket
#include <serial/wire.h>            // SfxWire

namespace sfx_core {

void RoleEventEmitter::emitRoleAttached(uint8_t portKind, uint8_t portIdx, uint8_t roleKind) {
    uint8_t buf[3] = { portKind, portIdx, roleKind };
    _ctx->sendRawPacket(RolePacket::ROLE_ATTACHED, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::ROLE_ATTACHED, buf, sizeof buf);
}

void RoleEventEmitter::emitRoleDetached(uint8_t portKind, uint8_t portIdx) {
    uint8_t buf[2] = { portKind, portIdx };
    _ctx->sendRawPacket(RolePacket::ROLE_DETACHED, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::ROLE_DETACHED, buf, sizeof buf);
}

void RoleEventEmitter::emitLedQueueDone(uint8_t portIdx) {
    uint8_t buf[1] = { portIdx };
    _ctx->sendRawPacket(RolePacket::LED_QUEUE_DONE, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::LED_QUEUE_DONE, buf, sizeof buf);
}

void RoleEventEmitter::emitServoTargetReached(uint8_t portIdx, uint16_t pos_us) {
    uint8_t buf[3];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], pos_us);
    // Wire = host live-view only (Rule 53): a continuously-tracking servo (gun
    // yaw/pitch) reaches "target" many times/sec and floods the wire + diag log.
    // Gate it on a listening host.  The LOCAL dispatch ALWAYS fires — the landing-
    // light service depends on it to know a servo finished deploying/retracting.
    if (_ctx && _ctx->hostVerboseActive())
        _ctx->sendRawPacket(RolePacket::SERVO_TARGET_REACHED, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::SERVO_TARGET_REACHED, buf, sizeof buf);
}

void RoleEventEmitter::emitServoMotionDone(uint8_t portIdx) {
    uint8_t buf[1] = { portIdx };
    // Wire = host live-view only (Rule 53); the LOCAL dispatch ALWAYS fires — the
    // gear door-sequencer chains the next door on this event.
    if (_ctx && _ctx->hostVerboseActive())
        _ctx->sendRawPacket(RolePacket::SERVO_MOTION_DONE, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::SERVO_MOTION_DONE, buf, sizeof buf);
}

void RoleEventEmitter::emitMotorStallEvent(uint8_t portIdx, uint16_t peak_mA, uint16_t duration_ms) {
    uint8_t buf[5];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], peak_mA);
    SfxWire::putU16LE(&buf[3], duration_ms);
    _ctx->sendRawPacket(RolePacket::MOTOR_STALL_EVENT, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::MOTOR_STALL_EVENT, buf, sizeof buf);
}

void RoleEventEmitter::emitBiMotorStallEvent(uint8_t portIdx, uint16_t peak_mA, uint16_t duration_ms) {
    uint8_t buf[5];
    buf[0] = portIdx;
    SfxWire::putU16LE(&buf[1], peak_mA);
    SfxWire::putU16LE(&buf[3], duration_ms);
    _ctx->sendRawPacket(RolePacket::BIMOTOR_STALL_EVENT, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::BIMOTOR_STALL_EVENT, buf, sizeof buf);
}

void RoleEventEmitter::emitBiMotorEndstopResult(uint8_t portIdx, uint8_t outcome,
                                                uint16_t travel_ms, uint16_t peak_mA,
                                                uint8_t position) {
    // Rule 11 extension — trailing position byte.  Older masters parse the
    // first 6 bytes and ignore the tail; newer masters see the resulting
    // endstop label (Strategy A) without a follow-up STATUS.
    uint8_t buf[7];
    buf[0] = portIdx;
    buf[1] = outcome;
    SfxWire::putU16LE(&buf[2], travel_ms);
    SfxWire::putU16LE(&buf[4], peak_mA);
    buf[6] = position;
    _ctx->sendRawPacket(RolePacket::BIMOTOR_ENDSTOP_RESULT, SfxWire::TAG_ASYNC, buf, sizeof buf);
    fireLocalAsync(RolePacket::BIMOTOR_ENDSTOP_RESULT, buf, sizeof buf);
}

void RoleEventEmitter::emitPpmFrameBroadcast(uint8_t portIdx, const RcPwmInputRole& role) {
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

void RoleEventEmitter::emitSbusFrameBroadcast(uint8_t portIdx, const SbusInputRole& role) {
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

void RoleEventEmitter::emitJetiExFrameBroadcast(uint8_t portIdx, const JetiExInputRole& role) {
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
