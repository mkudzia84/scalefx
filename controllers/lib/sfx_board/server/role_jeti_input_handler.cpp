/*
 * JetiInputHandler implementation — bodies moved verbatim from the former
 * InputRoleHandler Jeti methods; the IN_1/IN_2 expander pairing stays
 * platform-gated (`#if SFX_PLATFORM_ESP32`).
 */

#include "role_jeti_input_handler.h"
#include "role_event_emitter.h"
#include "board_server.h"
#include <serial/roles.h>          // RolePacket
#include <serial/wire.h>           // SfxWire
#include <serial/core/core.h>      // SerialError / PortError / RoleError

#if SFX_PLATFORM_ESP32
#  include <jeti_ex/jeti_expander.h>   // board-unique JetiExpander (Core-0 task)
#endif

#include <variant>

namespace sfx_core {

bool JetiInputHandler::attachInput(InputBinding& b, uint8_t portIdx,
                                   const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<JetiExInputRole>();
    // Optional config: [broadcastHz:u8][baudHi:u8][baudLo:u8][downstream:u8][respond:u8]
    //   baud encoded as kbaud (125 / 250); 0 = use default 125 000.
    //   downstream (byte 3): RESERVED / ignored — the IN_2 ESC monitor is now
    //     AUTODETECT (always on; the presence machine handles whether a device
    //     is there).  Kept in the layout for wire compat so byte 4 stays put.
    //   respond  (Rule 11, byte 4): enable TWO-WAY telemetry (half-duplex reply).
    //     DEFAULTS ON when the byte is absent; set byte 4 = 0 to force listen-only.
    uint32_t baud = 125000;
    if (cfgLen >= 3) {
        const uint16_t kbaud = ((uint16_t)cfg[1] << 8) | cfg[2];
        if (kbaud == 250) baud = 250000;
        else if (kbaud == 125 || kbaud == 0) baud = 125000;
        else baud = (uint32_t)kbaud * 1000;
    }
    const bool respond = (cfgLen >= 5) ? (cfg[4] != 0) : true;   // default ON
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
                                           /*respondTelemetry=*/respond);

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

bool JetiInputHandler::attachTelemetry(InputBinding& b, uint8_t portIdx,
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

void JetiInputHandler::handleGetFrameReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->inputAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<JetiExInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }

    const uint8_t count = r->channelCount();
    // [idx][count][valid][rxFrames:4][rxErrors:4][channels:u16×count]
    //   + TWO-WAY tail (Rule 11 append, after the channels so old clients ignore
    //     it): [txResp:4][pollsSeen:4][echoShort:4][slotOverruns:4][maxTxDurUs:2]
    //     [responding:1]
    uint8_t out[3 + 4 + 4 + 24*2 + 4 + 4 + 4 + 4 + 2 + 1];
    size_t off = 0;
    out[off++] = idx;
    out[off++] = count;
    out[off++] = r->valid() ? 1 : 0;
    SfxWire::putU32LE(&out[off], r->rxFrameCount()); off += 4;
    SfxWire::putU32LE(&out[off], r->rxErrorCount()); off += 4;
    const uint8_t nch = count > 24 ? 24 : count;   // Jeti EX ≤ 24 ch
    for (uint8_t i = 0; i < nch; i++) {
        SfxWire::putU16LE(&out[off], r->channel_us((uint8_t)(i + 1)));
        off += 2;
    }
    // Two-way (half-duplex reply) instrumentation tail.
    SfxWire::putU32LE(&out[off], r->txResponseCount());     off += 4;
    SfxWire::putU32LE(&out[off], r->pollsSeen());           off += 4;
    SfxWire::putU32LE(&out[off], r->echoShort());           off += 4;
    SfxWire::putU32LE(&out[off], r->slotOverruns());        off += 4;
    SfxWire::putU16LE(&out[off], (uint16_t)r->maxTxDurUs()); off += 2;
    out[off++] = r->responding() ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::JETIEX_FRAME_RESP, _ctx->currentTag(), out, off);
}

void JetiInputHandler::handleSetBroadcastHz(const uint8_t* p, size_t len) {
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
