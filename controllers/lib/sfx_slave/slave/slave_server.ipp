/*
 * SlaveServer<TServos, TPwms, TLeds, TBattery>::* — implementation.
 *
 * Dispatches the entire 0x10..0x3F SlavePacket range:
 *   0x10..0x14   identity / enumeration
 *   0x18..0x1C   servo (incl. SERVO_TARGET_REACHED async out)
 *   0x20..0x29   PWM (incl. PWM_RECONFIGURE / PWM_GET_CONFIG)
 *   0x28..0x2E   LED (incl. LED_PROGRAM_DONE async out)
 *
 * Each handler:
 *   - validates payload length up front
 *   - calls into the bound collection
 *   - emits the appropriate ACK / NACK / response packet
 *
 * Async events (SERVO_TARGET_REACHED, LED_PROGRAM_DONE) are emitted
 * outside this dispatch path — driven by the collection update()
 * callbacks wired in wireAsyncEvents().
 */

#ifndef SFX_SLAVE_SERVER_IPP
#define SFX_SLAVE_SERVER_IPP

#include "slave_server.h"
#include <serial/slave/slave.h>
#include <serial/slave/component_kind.h>
#include <platform/diag_log.h>             // DiagLog — emits LOG_MESSAGE / DIAG_HISTORY

#include <collections/servo_collection.ipp>
#include <collections/pwm_collection.ipp>
#include <collections/led_collection.ipp>

namespace sfx_slave {

using namespace sfx_peripherals;   // collections, sense policies, motor primitives

// ── Helpers ─────────────────────────────────────────────────────────

namespace detail {
    // Little-endian readers — payloads are LE per CLAUDE.md Rule 4.
    inline uint16_t rdU16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
    inline int16_t  rdI16(const uint8_t* p) { return (int16_t)rdU16(p); }
}

// ── Module-error message lookup ─────────────────────────────────────

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
const char* SlaveServer<TServos, TPwms, TLeds, TBattery>::getModuleErrorMessage(uint8_t code) {
    switch (code) {
        case SlaveError::INVALID_INDEX:        return "invalid component index";
        case SlaveError::WRONG_COMPONENT_KIND: return "wrong component kind for command";
        case SlaveError::MODE_NOT_SUPPORTED:   return "mode not supported by channel";
        case SlaveError::SENSING_UNAVAILABLE:  return "sensing not available on channel";
        case SlaveError::IDENT_TOO_LONG:       return "identifier too long (max 32 bytes)";
        case SlaveError::IDENT_PERSIST_FAILED: return "identifier persistence failed";
        case SlaveError::IDENT_INVALID_CHARS:  return "identifier has invalid characters";
        case SlaveError::NOT_INITIALISED:      return "slave not initialised — send INIT first";
        case SlaveError::PROGRAM_TOO_LARGE:    return "LED program exceeds slot capacity";
        case SlaveError::INVALID_PROGRAM_ID:   return "invalid LED program id";
    }
    return BusServer::getModuleErrorMessage(code);
}

// ── Top-level dispatch ──────────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
CommandHandleResult
SlaveServer<TServos, TPwms, TLeds, TBattery>::handleModulePacket(uint8_t type,
                                                       const uint8_t* payload,
                                                       size_t len) {
    noteMasterTraffic();

    // Identity / enumeration commands are valid even before INIT.
    switch (type) {
        case SlavePacket::COMPONENT_LIST_REQ:
            return handleComponentList({type, payload, len}) ? CommandHandleResult::Handled
                                                              : CommandHandleResult::Error;
        case SlavePacket::IDENT_GET_REQ:
            return handleIdentGet      ({type, payload, len}) ? CommandHandleResult::Handled
                                                              : CommandHandleResult::Error;
        case SlavePacket::IDENT_SET:
            return handleIdentSet      ({type, payload, len}) ? CommandHandleResult::Handled
                                                              : CommandHandleResult::Error;
        case SlavePacket::BATTERY_INFO_REQ:
            return handleBatteryInfo       ({type, payload, len}) ? CommandHandleResult::Handled
                                                                  : CommandHandleResult::Error;
        case SlavePacket::BATTERY_RECONFIGURE:
            return handleBatteryReconfigure({type, payload, len}) ? CommandHandleResult::Handled
                                                                  : CommandHandleResult::Error;
    }

    if (!_attached) {
        sendNack(SlaveError::NOT_INITIALISED);
        return CommandHandleResult::Handled;
    }

    // Dispatch ranges follow the post-cleanup allocation:
    //   0x10..0x2F  servo
    //   0x30..0x4F  PWM
    //   0x50..0x7F  LED
    if (type >= 0x10 && type <= 0x2F) {
        return handleServo({type, payload, len}) ? CommandHandleResult::Handled
                                                  : CommandHandleResult::Error;
    }
    if (type >= 0x30 && type <= 0x4F) {
        return handlePwm({type, payload, len}) ? CommandHandleResult::Handled
                                                : CommandHandleResult::Error;
    }
    if (type >= 0x50 && type <= 0x7F) {
        return handleLed({type, payload, len}) ? CommandHandleResult::Handled
                                               : CommandHandleResult::Error;
    }
    if (type == SlavePacket::SLAVE_STATUS_RATE) {
        if (len < 1) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return CommandHandleResult::Handled; }
        _statusBroadcastRate_hz = payload[0];                  // 0 = disabled
        _statusKindsMask        = (len >= 2) ? payload[1] : 0; // 0 = all kinds
        sendAck();
        return CommandHandleResult::Handled;
    }
    if (type == SlavePacket::SLAVE_STATUS_REQ) {
        // Synchronous version of the broadcast — same payload, master's tag.
        const uint8_t kinds = (len >= 1) ? payload[0] : 0;
        return handleStatusReq(kinds) ? CommandHandleResult::Handled
                                      : CommandHandleResult::Error;
    }

    return CommandHandleResult::NotMyCommand;
}

// ── Identity / enumeration ──────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
bool SlaveServer<TServos, TPwms, TLeds, TBattery>::handleComponentList(const SerialPacket&) {
    // Build COMPONENT_LIST_RESP payload: [count:u8][ComponentInfo×N].
    // Order: servos first, then PWM, then LEDs.
    constexpr size_t MAX_COMPONENTS = 64;
    uint8_t buf[1 + MAX_COMPONENTS * sizeof(ComponentInfo)];
    size_t  off = 0;

    auto append = [&](const ComponentInfo& info) {
        if (off + sizeof(ComponentInfo) > sizeof(buf)) return;
        memcpy(buf + 1 + off, &info, sizeof(ComponentInfo));
        off += sizeof(ComponentInfo);
    };

    uint8_t count = 0;
    if (_servos) {
        for (size_t i = 0; i < TServos::COUNT; i++, count++) append(TServos::describe(i));
    }
    if (_pwms) {
        for (size_t i = 0; i < TPwms::COUNT; i++, count++) append(_pwms->describe(i));
    }
    if (_leds) {
        for (size_t i = 0; i < TLeds::COUNT; i++, count++) append(_leds->describe(i));
    }
    buf[0] = count;

    sendRawPacket(SlavePacket::COMPONENT_LIST_RESP, currentTag(), buf, 1 + off);
    return true;
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
bool SlaveServer<TServos, TPwms, TLeds, TBattery>::handleIdentGet(const SerialPacket&) {
    if (!_ident) { sendNack(SlaveError::IDENT_PERSIST_FAILED); return true; }
    uint8_t buf[2 + sfx_slave::BoardIdentifier::MAX_LEN + 1];
    // [boardType:u8][len:u8][utf8...]  — boardType comes from the
    // BoardIdentifier (set once at firmware setup() via
    // boardIdent.setBoardType(SlaveType::…)).  Boards that don't set
    // it report 0 (Unknown), which the master interprets as a
    // dev/unidentified board and falls back to component fingerprint
    // discovery via COMPONENT_LIST_REQ.
    buf[0] = _ident->boardType();
    size_t l = _ident->length();
    buf[1] = (uint8_t)l;
    if (l > 0) memcpy(buf + 2, _ident->get(), l);
    sendRawPacket(SlavePacket::IDENT_GET_RESP, currentTag(), buf, 2 + l);
    return true;
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
bool SlaveServer<TServos, TPwms, TLeds, TBattery>::handleIdentSet(const SerialPacket& pkt) {
    if (!_ident) { sendNack(SlaveError::IDENT_PERSIST_FAILED); return true; }
    if (pkt.len < 1) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
    uint8_t l = pkt.payload[0];
    if (l > sfx_slave::BoardIdentifier::MAX_LEN) {
        sendNack(SlaveError::IDENT_TOO_LONG);
        return true;
    }
    if (pkt.len < (size_t)(1 + l)) {
        sendNack(SerialError::INVALID_PAYLOAD_LENGTH);
        return true;
    }
    char tmp[sfx_slave::BoardIdentifier::MAX_LEN + 1] = {0};
    memcpy(tmp, pkt.payload + 1, l);
    if (!_ident->set(tmp)) {
        DiagLog::instance().warn("slave: IDENT_SET rejected — invalid chars in '%s'", tmp);
        sendNack(SlaveError::IDENT_INVALID_CHARS);
        return true;
    }
    if (_identStorage.write && !_ident->save(_identStorage.write)) {
        DiagLog::instance().error("slave: IDENT_SET persistence failed for '%s'", tmp);
        sendNack(SlaveError::IDENT_PERSIST_FAILED);
        return true;
    }
    DiagLog::instance().info("slave: identifier set to '%s'", tmp);
    sendAck();
    return true;
}

// ── Servo dispatch ──────────────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
bool SlaveServer<TServos, TPwms, TLeds, TBattery>::handleServo(const SerialPacket& pkt) {
    if (!_servos) { sendNack(SlaveError::INVALID_INDEX); return true; }
    if (pkt.len < 1) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
    uint8_t idx = pkt.payload[0];

    switch (pkt.type) {
        case SlavePacket::SERVO_SET: {
            if (pkt.len < 3) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t pos = detail::rdU16(pkt.payload + 1);
            if (!_servos->setPosition_us(idx, pos)) {
                sendNack(SlaveError::INVALID_INDEX);
                return true;
            }
            sendAck();
            return true;
        }
        case SlavePacket::SERVO_CONFIG: {
            if (pkt.len < 13) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t mn  = detail::rdU16(pkt.payload + 1);
            uint16_t mx  = detail::rdU16(pkt.payload + 3);
            uint16_t cen = detail::rdU16(pkt.payload + 5);
            uint16_t spd = detail::rdU16(pkt.payload + 7);
            uint16_t acc = detail::rdU16(pkt.payload + 9);
            uint16_t dec = detail::rdU16(pkt.payload + 11);
            if (!_servos->setRange(idx, mn, mx, cen) ||
                !_servos->setMotion(idx, spd, acc, dec)) {
                sendNack(SlaveError::INVALID_INDEX);
                return true;
            }
            sendAck();
            return true;
        }
        case SlavePacket::SERVO_QUERY: {
            uint16_t pos = 0, tgt = 0;
            int16_t  vel = 0;
            if (!_servos->query(idx, pos, tgt, vel)) {
                sendNack(SlaveError::INVALID_INDEX);
                return true;
            }
            uint8_t resp[8];
            resp[0] = idx;
            resp[1] = (uint8_t) pos;        resp[2] = (uint8_t)(pos >> 8);
            resp[3] = (uint8_t) tgt;        resp[4] = (uint8_t)(tgt >> 8);
            resp[5] = (uint8_t) vel;        resp[6] = (uint8_t)(vel >> 8);
            resp[7] = 0;                    // flags reserved
            sendRawPacket(SlavePacket::SERVO_QUERY_RESP, currentTag(), resp, sizeof resp);
            return true;
        }
        case SlavePacket::SERVO_APPLY_JERK: {
            if (pkt.len < 5) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            int16_t  offset = detail::rdI16(pkt.payload + 1);
            uint16_t dur    = detail::rdU16(pkt.payload + 3);
            if (!_servos->applyJerk(idx, offset, dur)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::SERVO_SET_MOTION: {
            if (pkt.len < 7) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t spd = detail::rdU16(pkt.payload + 1);
            uint16_t acc = detail::rdU16(pkt.payload + 3);
            uint16_t dec = detail::rdU16(pkt.payload + 5);
            if (!_servos->setMotion(idx, spd, acc, dec)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::SERVO_HOLD: {
            if (pkt.len < 2) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            if (!_servos->setHold(idx, pkt.payload[1] != 0)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::SERVO_MOTION_UPDATES: {
            // [enable:u8][rate_hz:u8] — note: payload[0] is `enable`,
            // not a channel index, so the dispatch's idx-extract is
            // ignored on this path.
            if (pkt.len < 1) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            const bool    en   = pkt.payload[0] != 0;
            const uint8_t rate = (pkt.len >= 2) ? pkt.payload[1] : 0;
            _servos->setMotionUpdatesEnabled(en, rate);
            sendAck(); return true;
        }
    }
    return false;
}

// ── PWM dispatch ────────────────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
bool SlaveServer<TServos, TPwms, TLeds, TBattery>::handlePwm(const SerialPacket& pkt) {
    if (!_pwms) { sendNack(SlaveError::INVALID_INDEX); return true; }
    if (pkt.len < 1) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
    uint8_t idx = pkt.payload[0];

    switch (pkt.type) {
        case SlavePacket::PWM_SET_MODE: {
            if (pkt.len < 2) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            ComponentKind kind = (ComponentKind)pkt.payload[1];
            if (!_pwms->setMode(idx, kind)) { sendNack(SlaveError::MODE_NOT_SUPPORTED); return true; }
            sendAck(); return true;
        }
        case SlavePacket::PWM_SET_DUTY: {
            if (pkt.len < 3) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t duty = detail::rdU16(pkt.payload + 1);
            if (!_pwms->setDuty(idx, duty)) { sendNack(SlaveError::WRONG_COMPONENT_KIND); return true; }
            sendAck(); return true;
        }
        case SlavePacket::PWM_SET_MOTOR: {
            if (pkt.len < 3) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            int16_t speed = detail::rdI16(pkt.payload + 1);
            if (!_pwms->setMotor(idx, speed)) { sendNack(SlaveError::WRONG_COMPONENT_KIND); return true; }
            sendAck(); return true;
        }
        case SlavePacket::PWM_SET_HEATER: {
            if (pkt.len < 3) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t v = detail::rdU16(pkt.payload + 1);
            if (!_pwms->setHeater(idx, v)) { sendNack(SlaveError::WRONG_COMPONENT_KIND); return true; }
            sendAck(); return true;
        }
        case SlavePacket::PWM_SET_FREQ: {
            if (pkt.len < 3) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t f = detail::rdU16(pkt.payload + 1);
            if (!_pwms->setFrequency(idx, f)) { sendNack(SlaveError::INVALID_INDEX); return true; }
            sendAck(); return true;
        }
        case SlavePacket::PWM_RECONFIGURE: {
            if (pkt.len < 7) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            PwmRuntimeConfig cfg{};
            cfg.mode     = (ComponentKind)pkt.payload[1];
            cfg.freq_Hz  = detail::rdU16(pkt.payload + 2);
            cfg.cfgFlags = pkt.payload[4];
            cfg.maxDuty  = detail::rdU16(pkt.payload + 5);
            if (!_pwms->reconfigure(idx, cfg)) { sendNack(SlaveError::MODE_NOT_SUPPORTED); return true; }
            sendAck(); return true;
        }
        case SlavePacket::PWM_QUERY: {
            ComponentKind mode;
            uint16_t duty = 0, freq = 0;
            int32_t  v_mV = 0, c_mA = 0;
            if (!_pwms->query(idx, mode, duty, freq, v_mV, c_mA)) {
                sendNack(SlaveError::INVALID_INDEX);
                return true;
            }
            uint8_t resp[14];
            resp[0]  = idx;
            resp[1]  = (uint8_t)mode;
            resp[2]  = (uint8_t) duty;       resp[3]  = (uint8_t)(duty >> 8);
            resp[4]  = (uint8_t) freq;       resp[5]  = (uint8_t)(freq >> 8);
            resp[6]  = (uint8_t) v_mV;       resp[7]  = (uint8_t)(v_mV >> 8);
            resp[8]  = (uint8_t)(v_mV >> 16);resp[9]  = (uint8_t)(v_mV >> 24);
            resp[10] = (uint8_t) c_mA;       resp[11] = (uint8_t)(c_mA >> 8);
            resp[12] = (uint8_t)(c_mA >> 16);resp[13] = (uint8_t)(c_mA >> 24);
            sendRawPacket(SlavePacket::PWM_QUERY_RESP, currentTag(), resp, sizeof resp);
            return true;
        }
        case SlavePacket::PWM_SET_STALL_GUARD: {
            if (pkt.len < 5) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t threshold = detail::rdU16(pkt.payload + 1);
            uint8_t  debounce  = pkt.payload[3];
            uint8_t  flags     = pkt.payload[4];
            if (!_pwms->setStallGuard(idx, threshold, debounce, flags)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::PWM_CLEAR_STALL: {
            if (!_pwms->clearStall(idx)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::PWM_GET_CONFIG: {
            PwmRuntimeConfig cfg{};
            if (!_pwms->getRuntimeConfig(idx, cfg)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            uint8_t resp[10];
            resp[0] = idx;
            resp[1] = (uint8_t)cfg.mode;
            resp[2] = (uint8_t) cfg.freq_Hz;  resp[3] = (uint8_t)(cfg.freq_Hz >> 8);
            resp[4] = cfg.cfgFlags;
            resp[5] = (uint8_t) cfg.maxDuty;  resp[6] = (uint8_t)(cfg.maxDuty >> 8);
            resp[7] = _pwms->hwFlags(idx);
            resp[8] = 0xFF;   // vSense placeholder — wire when PwmSpec exposes
            resp[9] = 0xFF;   // cSense placeholder
            sendRawPacket(SlavePacket::PWM_GET_CONFIG_RESP, currentTag(), resp, sizeof resp);
            return true;
        }
    }
    return false;
}

// ── LED dispatch ────────────────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
bool SlaveServer<TServos, TPwms, TLeds, TBattery>::handleLed(const SerialPacket& pkt) {
    if (!_leds) { sendNack(SlaveError::INVALID_INDEX); return true; }
    if (pkt.len < 1) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
    uint8_t addr = pkt.payload[0];

    switch (pkt.type) {
        case SlavePacket::LED_SET_BRIGHTNESS: {
            if (pkt.len < 2) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            if (!_leds->setBrightness(addr, pkt.payload[1])) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::LED_PROGRAM_LOAD: {
            if (pkt.len < 3) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint8_t progId = pkt.payload[1];
            uint8_t count  = pkt.payload[2];
            if (pkt.len < (size_t)(3 + count * sizeof(LedEvent))) {
                sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true;
            }
            const LedEvent* events = reinterpret_cast<const LedEvent*>(pkt.payload + 3);
            if (!_leds->loadProgram(addr, progId, events, count)) {
                sendNack(SlaveError::PROGRAM_TOO_LARGE); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::LED_PROGRAM_RUN: {
            if (pkt.len < 3) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            if (!_leds->runProgram(addr, pkt.payload[1], pkt.payload[2])) {
                sendNack(SlaveError::INVALID_PROGRAM_ID); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::LED_PROGRAM_STOP: {
            if (!_leds->stopProgram(addr)) { sendNack(SlaveError::INVALID_INDEX); return true; }
            sendAck(); return true;
        }
        case SlavePacket::LED_QUERY: {
            uint8_t bri = 0, pid = 0, st = 0;
            if (!_leds->query(addr, bri, pid, st)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            uint8_t resp[4] = { addr, bri, pid, st };
            sendRawPacket(SlavePacket::LED_QUERY_RESP, currentTag(), resp, sizeof resp);
            return true;
        }
        case SlavePacket::LED_PROGRAM_RESTART: {
            if (!_leds->restartProgram(addr)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::LED_RESET_CHANNEL: {
            if (!_leds->resetChannel(addr)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::LED_ENABLE_CHANNEL: {
            if (pkt.len < 2) { sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            if (!_leds->enableChannel(addr, pkt.payload[1] != 0)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::LED_SET_MASTER_BRIGHTNESS: {
            // Note: this command's first byte IS the percent, not a
            // channel address — re-use of the dispatch pkt.payload[0]
            // works because we read it as `addr` above.
            if (!_leds->setMasterBrightness(addr)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            sendAck(); return true;
        }
        case SlavePacket::LED_SEQ_STATUS_REQ: {
            LightFxSeqStatus st{};
            if (!_leds->seqStatus(addr, st)) {
                sendNack(SlaveError::INVALID_INDEX); return true;
            }
            // LightFxSeqStatus is a POD struct in serial/lightfx/lightfx.h —
            // wire it verbatim.  An [addr:u8] prefix lets the master
            // correlate when multiple status queries fly back-to-back.
            uint8_t resp[1 + sizeof(LightFxSeqStatus)];
            resp[0] = addr;
            memcpy(resp + 1, &st, sizeof st);
            sendRawPacket(SlavePacket::LED_SEQ_STATUS_RESP, currentTag(),
                          resp, sizeof resp);
            return true;
        }
    }
    return false;
}

// ── Async event emitters ────────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
void SlaveServer<TServos, TPwms, TLeds, TBattery>::emitServoTargetReached(uint8_t idx, uint16_t pos_us) {
    uint8_t buf[3] = { idx, (uint8_t)pos_us, (uint8_t)(pos_us >> 8) };
    sendRawPacket(SlavePacket::SERVO_TARGET_REACHED, CoreProtocol::TAG_ASYNC, buf, sizeof buf);
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
void SlaveServer<TServos, TPwms, TLeds, TBattery>::emitServoMotionUpdate(
        uint8_t idx, uint16_t pos, uint16_t target, int16_t vel) {
    uint8_t buf[7];
    buf[0] = idx;
    buf[1] = (uint8_t) pos;     buf[2] = (uint8_t)(pos    >> 8);
    buf[3] = (uint8_t) target;  buf[4] = (uint8_t)(target >> 8);
    buf[5] = (uint8_t) vel;     buf[6] = (uint8_t)(vel    >> 8);
    sendRawPacket(SlavePacket::SERVO_MOTION_UPDATE, CoreProtocol::TAG_ASYNC, buf, sizeof buf);
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
void SlaveServer<TServos, TPwms, TLeds, TBattery>::emitLedProgramDone(uint8_t addr, uint8_t progId) {
    uint8_t buf[2] = { addr, progId };
    sendRawPacket(SlavePacket::LED_PROGRAM_DONE, CoreProtocol::TAG_ASYNC, buf, sizeof buf);
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
void SlaveServer<TServos, TPwms, TLeds, TBattery>::emitPwmStall(uint8_t idx, uint16_t peak_mA, uint16_t duration_ms) {
    // Stall is rare and informative — log at warn so the master sees a
    // history entry alongside the async PWM_STALL packet.
    DiagLog::instance().warn("pwm[%u]: stall guard tripped — peak=%u mA after %u ms",
                             (unsigned)idx, (unsigned)peak_mA, (unsigned)duration_ms);
    uint8_t buf[5];
    buf[0] = idx;
    buf[1] = (uint8_t) peak_mA;      buf[2] = (uint8_t)(peak_mA >> 8);
    buf[3] = (uint8_t) duration_ms;  buf[4] = (uint8_t)(duration_ms >> 8);
    sendRawPacket(SlavePacket::PWM_STALL, CoreProtocol::TAG_ASYNC, buf, sizeof buf);
}

// ── Unified status broadcast ────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
void SlaveServer<TServos, TPwms, TLeds, TBattery>::checkStatusBroadcast() {
    if (_statusBroadcastRate_hz == 0) return;
    uint32_t period_ms = 1000u / _statusBroadcastRate_hz;
    if ((uint32_t)(millis() - _lastStatusBroadcast_ms) < period_ms) return;
    _lastStatusBroadcast_ms = millis();
    emitStatusBroadcast();
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
size_t SlaveServer<TServos, TPwms, TLeds, TBattery>::buildStatusPayload(
        uint8_t* buf, size_t bufSize, uint8_t kindsMask) const {
    // Worst-case wire bound: 10B header + 32 servos × 9B + 32 PWMs × 11B + 32 LEDs × 4B
    // = 10 + 288 + 352 + 128 = 778B.  Caller's bufSize must be at least this for a
    // fully-loaded slave; the standard 256B / 512B COBS cap covers typical 6/8/8 boards.
    size_t off = 0;
    if (off + 10 > bufSize) return 0;

    // ── Header ─────────────────────────────────────────────────────
    buf[off++] = _attached ? BoardState::SLAVE : BoardState::IDLE;
    buf[off++] = _initMode;
    uint32_t up = millis();
    buf[off++] = (uint8_t) up;       buf[off++] = (uint8_t)(up >> 8);
    buf[off++] = (uint8_t)(up >> 16);buf[off++] = (uint8_t)(up >> 24);
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;   // freeRam reserved

    const bool wantAll     = (kindsMask == SlavePacket::StatusKinds::ALL);
    const bool wantServo   = wantAll || (kindsMask & SlavePacket::StatusKinds::SERVO);
    const bool wantPwm     = wantAll || (kindsMask & SlavePacket::StatusKinds::PWM);
    const bool wantLed     = wantAll || (kindsMask & SlavePacket::StatusKinds::LED);
    const bool wantBattery = wantAll || (kindsMask & SlavePacket::StatusKinds::BATTERY);
    const bool headerOnly  = (kindsMask & SlavePacket::StatusKinds::HEADER_ONLY) != 0;

    // ── Servos — entry: [port_id][pos:u16][target:u16][vel:i16][flags:u8] = 9 bytes
    if (off >= bufSize) return off;
    uint8_t* servoCount = &buf[off++];
    *servoCount = 0;
    if (_servos && wantServo && !headerOnly) {
        for (size_t i = 0; i < TServos::COUNT && off + 9 <= bufSize; i++) {
            uint16_t pos = 0, tgt = 0; int16_t vel = 0;
            _servos->query((uint8_t)i, pos, tgt, vel);
            buf[off++] = SlavePacket::PortId::make(SlavePacket::PortId::Servo, (uint8_t)i);
            buf[off++] = (uint8_t) pos; buf[off++] = (uint8_t)(pos >> 8);
            buf[off++] = (uint8_t) tgt; buf[off++] = (uint8_t)(tgt >> 8);
            buf[off++] = (uint8_t) vel; buf[off++] = (uint8_t)(vel >> 8);
            buf[off++] = 0;   // flags reserved
            (*servoCount)++;
        }
    }

    // ── PWMs — entry: [port_id][mode][duty:u16][V_mV:i16][I_mA:i16][stallFlags][peak_mA:u16] = 11 bytes
    if (off >= bufSize) return off;
    uint8_t* pwmCount = &buf[off++];
    *pwmCount = 0;
    if (_pwms && wantPwm && !headerOnly) {
        for (size_t i = 0; i < TPwms::COUNT && off + 11 <= bufSize; i++) {
            ComponentKind mode; uint16_t duty = 0, freq = 0;
            int32_t v_mV = 0, c_mA = 0;
            _pwms->query((uint8_t)i, mode, duty, freq, v_mV, c_mA);
            int16_t v_clip = (v_mV >  32767) ?  32767 : (v_mV < -32768 ? -32768 : (int16_t)v_mV);
            int16_t c_clip = (c_mA >  32767) ?  32767 : (c_mA < -32768 ? -32768 : (int16_t)c_mA);
            buf[off++] = SlavePacket::PortId::make(SlavePacket::PortId::Pwm, (uint8_t)i);
            buf[off++] = (uint8_t)mode;
            buf[off++] = (uint8_t) duty;   buf[off++] = (uint8_t)(duty >> 8);
            buf[off++] = (uint8_t) v_clip; buf[off++] = (uint8_t)(v_clip >> 8);
            buf[off++] = (uint8_t) c_clip; buf[off++] = (uint8_t)(c_clip >> 8);
            // Stall guard mirror — exposes whether the channel is
            // currently latched + the last peak observed since the
            // master cleared it.  Helps the master render motor health.
            uint8_t  stallFlags = 0;
            uint16_t peak_mA    = 0;
            _pwms->getStallStatus((uint8_t)i, stallFlags, peak_mA);
            buf[off++] = stallFlags;
            buf[off++] = (uint8_t) peak_mA;
            buf[off++] = (uint8_t)(peak_mA >> 8);
            (*pwmCount)++;
        }
    }

    // ── LEDs — entry: [port_id][brightness][progState][progId] = 4 bytes
    if (off >= bufSize) return off;
    uint8_t* ledCount = &buf[off++];
    *ledCount = 0;
    if (_leds && wantLed && !headerOnly) {
        for (size_t i = 0; i < TLeds::COUNT && off + 4 <= bufSize; i++) {
            uint8_t addr = SlavePacket::LedAddr::dedicated((uint8_t)i);
            uint8_t bri = 0, pid = 0, st = 0;
            _leds->query(addr, bri, pid, st);
            buf[off++] = SlavePacket::PortId::make(SlavePacket::PortId::LedDed, (uint8_t)i);
            buf[off++] = bri;
            buf[off++] = st;
            buf[off++] = pid;
            (*ledCount)++;
        }
    }

    // ── Battery — variable: [present:u8] (+ chemistry/cells/v/cv/pct/flags if present)
    if (wantBattery && !headerOnly) {
        off += appendBatterySection(buf + off, bufSize - off);
    } else {
        // Section is filtered out — emit a single `present=0` byte so
        // every status payload has the same shape (master parsers can
        // always read the trailing battery byte unconditionally).
        if (off < bufSize) buf[off++] = 0;
    }
    return off;
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
void SlaveServer<TServos, TPwms, TLeds, TBattery>::emitStatusBroadcast() {
    uint8_t buf[768];
    size_t  n = buildStatusPayload(buf, sizeof buf, _statusKindsMask);
    sendRawPacket(SlavePacket::SLAVE_STATUS_BROADCAST,
                  CoreProtocol::TAG_ASYNC, buf, n);
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
bool SlaveServer<TServos, TPwms, TLeds, TBattery>::handleStatusReq(uint8_t kindsMask) {
    uint8_t buf[768];
    size_t  n = buildStatusPayload(buf, sizeof buf, kindsMask);
    sendRawPacket(SlavePacket::SLAVE_STATUS_BROADCAST,
                  currentTag(), buf, n);
    return true;
}

// ── Battery dispatch + alert emission + status section ─────────────

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
bool SlaveServer<TServos, TPwms, TLeds, TBattery>::handleBatteryInfo(const SerialPacket&) {
    // Wire format:
    //   [present:u8][chemistry:u8][cellCount:u8][voltage_mV:u16LE]
    //   [cellVoltage_mV:u16LE][percentage:u8][flags:u8]
    //   [profileLow_mV:u16LE][profileCritical_mV:u16LE]
    //
    // Boards without a battery (TBattery == NoBattery, or _battery == nullptr)
    // return `present = 0` and stop — the master is expected to gate
    // further calls on this byte (or on the BATTERY capability bit).
    uint8_t buf[14];
    size_t  off = 0;

    if constexpr (std::is_same_v<TBattery, NoBattery>) {
        buf[off++] = 0;   // present = false
    } else {
        const bool present = (_battery != nullptr) && _battery->isPresent();
        buf[off++] = present ? 1 : 0;
        if (!present) {
            // Stop after the present byte — Studio / CLI handle the
            // "battery monitor wired but no pack" case via this short
            // form rather than reading dummy fields.
        } else {
            const uint16_t v_mV   = _battery->voltage_mV();
            const uint16_t cv_mV  = _battery->cellVoltage_mV();
            const uint8_t  pct    = _battery->percentage();
            const auto&    profile = BatteryProfiles::forChemistry(_battery->chemistry());

            uint8_t flags = SlavePacket::BatteryFlags::PRESENT;
            if (_battery->isLowTriggered())      flags |= SlavePacket::BatteryFlags::LOW_TRIGGERED;
            if (_battery->isCriticalTriggered()) flags |= SlavePacket::BatteryFlags::CRITICAL_TRIGGERED;
            if (_battery->isCellCountManual())   flags |= SlavePacket::BatteryFlags::MANUAL_CELL_COUNT;
            if (_battery->isUsbPowered())        flags |= SlavePacket::BatteryFlags::USB_POWERED;

            buf[off++] = (uint8_t)_battery->chemistry();
            buf[off++] = _battery->cellCount();
            buf[off++] = (uint8_t) v_mV;  buf[off++] = (uint8_t)(v_mV  >> 8);
            buf[off++] = (uint8_t) cv_mV; buf[off++] = (uint8_t)(cv_mV >> 8);
            buf[off++] = pct;
            buf[off++] = flags;
            buf[off++] = (uint8_t) profile.low_mV;      buf[off++] = (uint8_t)(profile.low_mV      >> 8);
            buf[off++] = (uint8_t) profile.critical_mV; buf[off++] = (uint8_t)(profile.critical_mV >> 8);
        }
    }

    sendRawPacket(SlavePacket::BATTERY_INFO_RESP, currentTag(), buf, off);
    return true;
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
bool SlaveServer<TServos, TPwms, TLeds, TBattery>::handleBatteryReconfigure(const SerialPacket& pkt) {
    // [chemistry:u8][cellCount:u8][customLow_mV:u16LE][customCritical_mV:u16LE]
    if (pkt.len < 6) {
        sendNack(SerialError::MISSING_PARAMETER);
        return true;
    }
    const uint8_t  chemistry  = pkt.payload[0];
    const uint8_t  cellCount  = pkt.payload[1];
    const uint16_t low_mV     = detail::rdU16(&pkt.payload[2]);
    const uint16_t crit_mV    = detail::rdU16(&pkt.payload[4]);

    if (chemistry > (uint8_t)BatteryChemistry::NIMH) {
        sendNack(SlaveError::BATTERY_INVALID_CHEMISTRY);
        return true;
    }

    if constexpr (std::is_same_v<TBattery, NoBattery>) {
        sendNack(SlaveError::BATTERY_NOT_PRESENT);
        return true;
    } else {
        if (!_battery) {
            sendNack(SlaveError::BATTERY_NOT_PRESENT);
            return true;
        }
        _battery->setChemistry((BatteryChemistry)chemistry);
        _battery->setCellCount(cellCount);
        if (low_mV  != 0) _battery->setLowThreshold_mV(low_mV);
        if (crit_mV != 0) _battery->setCriticalThreshold_mV(crit_mV);
        DiagLog::instance().info("battery: reconfigured chem=%u cells=%u low=%u crit=%u (mV/cell)",
                                 (unsigned)chemistry, (unsigned)cellCount,
                                 (unsigned)low_mV, (unsigned)crit_mV);
        sendAck();
        return true;
    }
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
void SlaveServer<TServos, TPwms, TLeds, TBattery>::emitBatteryAlert(uint8_t  level,
                                                                   uint16_t voltage_mV,
                                                                   uint8_t  cellCount) {
    // Log the transition.  Hysteresis in BatteryStateMachine ensures we
    // don't spam — each level transition is a single edge per crossing.
    const char* levelName =
        (level == SlavePacket::BatteryAlertLevel::CRITICAL) ? "CRITICAL" :
        (level == SlavePacket::BatteryAlertLevel::LOW)      ? "LOW"      : "OK";
    if (level == SlavePacket::BatteryAlertLevel::CRITICAL) {
        DiagLog::instance().error("battery: %s — %u mV across %u cell(s)",
                                  levelName, (unsigned)voltage_mV, (unsigned)cellCount);
    } else if (level == SlavePacket::BatteryAlertLevel::LOW) {
        DiagLog::instance().warn ("battery: %s — %u mV across %u cell(s)",
                                  levelName, (unsigned)voltage_mV, (unsigned)cellCount);
    } else {
        DiagLog::instance().info ("battery: re-armed (OK) at %u mV / %u cell(s)",
                                  (unsigned)voltage_mV, (unsigned)cellCount);
    }

    uint8_t buf[4];
    buf[0] = level;
    buf[1] = (uint8_t) voltage_mV;
    buf[2] = (uint8_t)(voltage_mV >> 8);
    buf[3] = cellCount;
    sendRawPacket(SlavePacket::BATTERY_ALERT, CoreProtocol::TAG_ASYNC, buf, sizeof buf);
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
size_t SlaveServer<TServos, TPwms, TLeds, TBattery>::appendBatterySection(uint8_t* buf,
                                                                          size_t   bufSize) const {
    if (bufSize < 1) return 0;
    size_t off = 0;

    if constexpr (std::is_same_v<TBattery, NoBattery>) {
        buf[off++] = 0;   // present = false; no further bytes
        return off;
    } else {
        const bool present = (_battery != nullptr) && _battery->isPresent();
        buf[off++] = present ? 1 : 0;
        if (!present || off + 7 > bufSize) return off;

        uint8_t flags = SlavePacket::BatteryFlags::PRESENT;
        if (_battery->isLowTriggered())      flags |= SlavePacket::BatteryFlags::LOW_TRIGGERED;
        if (_battery->isCriticalTriggered()) flags |= SlavePacket::BatteryFlags::CRITICAL_TRIGGERED;
        if (_battery->isCellCountManual())   flags |= SlavePacket::BatteryFlags::MANUAL_CELL_COUNT;
        if (_battery->isUsbPowered())        flags |= SlavePacket::BatteryFlags::USB_POWERED;

        const uint16_t v_mV  = _battery->voltage_mV();
        const uint16_t cv_mV = _battery->cellVoltage_mV();

        buf[off++] = (uint8_t)_battery->chemistry();
        buf[off++] = _battery->cellCount();
        buf[off++] = (uint8_t) v_mV;  buf[off++] = (uint8_t)(v_mV  >> 8);
        buf[off++] = (uint8_t) cv_mV; buf[off++] = (uint8_t)(cv_mV >> 8);
        buf[off++] = _battery->percentage();
        buf[off++] = flags;
        return off;
    }
}

// ── Keepalive watchdog ──────────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
void SlaveServer<TServos, TPwms, TLeds, TBattery>::noteMasterTraffic() {
    _lastKeepalive_ms = millis();
}

template <typename TServos, typename TPwms, typename TLeds, typename TBattery>
void SlaveServer<TServos, TPwms, TLeds, TBattery>::checkKeepaliveTimeout() {
    // Only enforce in SLAVE mode — DIRECT is the bench-test path.
    if (_initMode != BoardMode::SLAVE) return;
    if (!_attached) return;     // already in safe state — don't log every loop tick
    if ((uint32_t)(millis() - _lastKeepalive_ms) < _keepaliveTimeout_ms) return;
    DiagLog::instance().warn("slave: keepalive timeout (%lu ms since last master traffic) — entering safe state",
                             (unsigned long)(millis() - _lastKeepalive_ms));
    enterSafeState();
}

}  // namespace sfx_slave

#endif  // SFX_SLAVE_SERVER_IPP
