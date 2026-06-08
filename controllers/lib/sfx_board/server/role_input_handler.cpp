/*
 * InputRoleHandler implementation — bodies moved verbatim from the former
 * RoleServicePolicy input handlers; broadcast callbacks now go through the
 * bound RoleEventEmitter.  The Jeti IN_1/IN_2 expander pairing stays
 * platform-gated (`#if SFX_PLATFORM_ESP32`).
 */

#include "role_input_handler.h"
#include "role_event_emitter.h"
#include "board_server.h"
#include <serial/roles.h>          // RolePacket
#include <serial/wire.h>           // SfxWire
#include <serial/core/core.h>      // SerialError / PortError / RoleError
#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32
#  include <jeti_ex/jeti_expander.h>   // board-unique JetiExpander (Core-0 task)
#endif

#include <variant>

namespace sfx_core {

// ── Attach: build role + wire broadcast callbacks ───────────────────────

bool InputRoleHandler::attachRcPwm(InputBinding& b, uint8_t portIdx,
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
            _emit->emitPpmFrameBroadcast(portIdx, *r);
        }
    });
    return true;
}

bool InputRoleHandler::attachSbus(InputBinding& b, uint8_t portIdx,
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
            _emit->emitSbusFrameBroadcast(portIdx, *r);
        }
    });
    return true;
}

bool InputRoleHandler::attachJetiEx(InputBinding& b, uint8_t portIdx,
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
            _emit->emitJetiExFrameBroadcast(portIdx, *r);
        }
    });
    return true;
}

bool InputRoleHandler::attachJetiExTelemetry(InputBinding& b, uint8_t portIdx,
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

// ── RC PWM input role commands ──────────────────────────────────────────

void InputRoleHandler::handleRcInGetValueReq(const uint8_t* p, size_t len) {
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

void InputRoleHandler::handleRcInSetBroadcastHz(const uint8_t* p, size_t len) {
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

// ── SBUS input role commands ────────────────────────────────────────────

void InputRoleHandler::handleSbusGetFrameReq(const uint8_t* p, size_t len) {
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

void InputRoleHandler::handleSbusSetBroadcastHz(const uint8_t* p, size_t len) {
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

// ── Jeti EX input role commands ─────────────────────────────────────────

void InputRoleHandler::handleJetiExGetFrameReq(const uint8_t* p, size_t len) {
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

void InputRoleHandler::handleJetiExSetBroadcastHz(const uint8_t* p, size_t len) {
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

}  // namespace sfx_core
