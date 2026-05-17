/*
 * ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::* — implementation.
 *
 * Dispatches the entire 0x10..0x3F ComponentPacket range:
 *   0x10..0x14   identity / enumeration
 *   0x18..0x1C   servo (incl. SERVO_TARGET_REACHED async out)
 *   0x20..0x29   PWM (incl. PWM_RECONFIGURE / PWM_GET_CONFIG)
 *   0x28..0x2E   LED (incl. LED_QUEUE_DONE async out)
 *
 * Each handler:
 *   - validates payload length up front
 *   - calls into the bound collection
 *   - emits the appropriate ACK / NACK / response packet
 *
 * Async events (SERVO_TARGET_REACHED, LED_QUEUE_DONE) are emitted
 * outside this dispatch path — driven by the collection update()
 * callbacks wired in wireAsyncEvents().
 */

#ifndef SFX_COMPONENT_SERVICE_IPP
#define SFX_COMPONENT_SERVICE_IPP

#include "component_service.h"
#include <serial/components/components.h>
#include <serial/components/component_kind.h>
#include <platform/diag_log.h>             // DiagLog — emits LOG_MESSAGE / DIAG_HISTORY

#include <collections/servo_collection.ipp>
#include <collections/pwm_collection.ipp>


namespace sfx_core {

using namespace sfx_peripherals;   // collections, sense policies, motor primitives

// ── Helpers ─────────────────────────────────────────────────────────

namespace detail {
    // Little-endian readers — payloads are LE per CLAUDE.md Rule 4.
    inline uint16_t rdU16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
    inline int16_t  rdI16(const uint8_t* p) { return (int16_t)rdU16(p); }
}

// ── Module-error message lookup ─────────────────────────────────────

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
const char* ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::getErrorMessage(uint8_t code) const {
    switch (code) {
        case ComponentError::INVALID_INDEX:        return "invalid component index";
        case ComponentError::WRONG_COMPONENT_KIND: return "wrong component kind for command";
        case ComponentError::MODE_NOT_SUPPORTED:   return "mode not supported by channel";
        case ComponentError::SENSING_UNAVAILABLE:  return "sensing not available on channel";
        case ComponentError::IDENT_TOO_LONG:       return "identifier too long (max 32 bytes)";
        case ComponentError::IDENT_PERSIST_FAILED: return "identifier persistence failed";
        case ComponentError::IDENT_INVALID_CHARS:  return "identifier has invalid characters";
        case ComponentError::NOT_INITIALISED:      return "expander not initialised — send INIT first";
        case ComponentError::QUEUE_TOO_LARGE:       return "LED queue exceeds slot capacity";
        case ComponentError::BATCH_NOT_FOUND:        return "batch id not loaded";
        case ComponentError::BATCH_TOO_LARGE:        return "batch payload exceeds slot capacity";
        case ComponentError::BATCH_INVALID_COMMAND:  return "batch sub-command type not batchable";
        case ComponentError::BATCH_VALIDATION_FAILED:return "batch sub-command validation failed";
    }
    return nullptr;   // not our code — BoardServer falls back to generic SerialError or the next policy
}

// ── Top-level dispatch ──────────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
CommandHandleResult
ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::handle(uint8_t type,
                                                       const uint8_t* payload,
                                                       size_t len) {
    noteMasterTraffic();

    // Identity / enumeration commands are valid even before INIT.
    switch (type) {
        case ComponentPacket::COMPONENT_LIST_REQ:
            return handleComponentList({type, payload, len}) ? CommandHandleResult::Handled
                                                              : CommandHandleResult::Error;
        case ComponentPacket::IDENT_GET_REQ:
            return handleIdentGet      ({type, payload, len}) ? CommandHandleResult::Handled
                                                              : CommandHandleResult::Error;
        case ComponentPacket::IDENT_SET:
            return handleIdentSet      ({type, payload, len}) ? CommandHandleResult::Handled
                                                              : CommandHandleResult::Error;
        case ComponentPacket::BATTERY_INFO_REQ:
            return handleBatteryInfo       ({type, payload, len}) ? CommandHandleResult::Handled
                                                                  : CommandHandleResult::Error;
        case ComponentPacket::BATTERY_RECONFIGURE:
            return handleBatteryReconfigure({type, payload, len}) ? CommandHandleResult::Handled
                                                                  : CommandHandleResult::Error;
    }

    if (!_attached) {
        _ctx->sendNack(ComponentError::NOT_INITIALISED);
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
    if (type == ComponentPacket::COMPONENT_STATUS_RATE) {
        if (len < 1) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return CommandHandleResult::Handled; }
        _statusBroadcastRate_hz = payload[0];                  // 0 = disabled
        _statusKindsMask        = (len >= 2) ? payload[1] : 0; // 0 = all kinds
        _ctx->sendAck();
        return CommandHandleResult::Handled;
    }
    if (type == ComponentPacket::COMPONENT_STATUS_REQ) {
        // Synchronous version of the broadcast — same payload, master's tag.
        const uint8_t kinds = (len >= 1) ? payload[0] : 0;
        return handleStatusReq(kinds) ? CommandHandleResult::Handled
                                      : CommandHandleResult::Error;
    }

    return CommandHandleResult::NotMyCommand;
}

// ── Identity / enumeration ──────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
bool ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::handleComponentList(const SerialPacket&) {
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
    // Dedicated LED pool — every channel is always-present.
    if (_ledsDed) {
        for (size_t i = 0; i < TLedsDed::COUNT; i++, count++) {
            append(ComponentInfo{
                .index    = (uint8_t)i,
                .kind     = sfx_peripherals::ComponentKind::LedDigital,
                .flags    = sfx_peripherals::LedFlags::SUPPORTS_QUEUE,
                .reserved = 0,
            });
        }
    }
    // PWM-borrowed LED pool — only channels currently in PwmLed mode
    // surface here.  Channels in other modes are advertised in the
    // PWM section above with their actual ComponentKind.
    if constexpr (!std::is_same_v<TLedsBor, NoBorrowedLeds>) {
        if (_ledsBor && _pwms) {
            for (size_t i = 0; i < TLedsBor::COUNT; i++) {
                if (_pwms->currentMode((uint8_t)i) != sfx_peripherals::ComponentKind::PwmLed) continue;
                append(ComponentInfo{
                    .index    = (uint8_t)(0x80 | (i & 0x7F)),  // bit 7 → PWM-borrowed
                    .kind     = sfx_peripherals::ComponentKind::PwmLed,
                    .flags    = sfx_peripherals::LedFlags::SUPPORTS_QUEUE,
                    .reserved = 0,
                });
                count++;
            }
        }
    }
    buf[0] = count;

    _ctx->sendRawPacket(ComponentPacket::COMPONENT_LIST_RESP, _ctx->currentTag(), buf, 1 + off);
    return true;
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
bool ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::handleIdentGet(const SerialPacket&) {
    if (!_ident) { _ctx->sendNack(ComponentError::IDENT_PERSIST_FAILED); return true; }
    uint8_t buf[2 + sfx_core::BoardIdentifier::MAX_LEN + 1];
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
    _ctx->sendRawPacket(ComponentPacket::IDENT_GET_RESP, _ctx->currentTag(), buf, 2 + l);
    return true;
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
bool ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::handleIdentSet(const SerialPacket& pkt) {
    if (!_ident) { _ctx->sendNack(ComponentError::IDENT_PERSIST_FAILED); return true; }
    if (pkt.len < 1) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
    uint8_t l = pkt.payload[0];
    if (l > sfx_core::BoardIdentifier::MAX_LEN) {
        _ctx->sendNack(ComponentError::IDENT_TOO_LONG);
        return true;
    }
    if (pkt.len < (size_t)(1 + l)) {
        _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH);
        return true;
    }
    char tmp[sfx_core::BoardIdentifier::MAX_LEN + 1] = {0};
    memcpy(tmp, pkt.payload + 1, l);
    if (!_ident->set(tmp)) {
        DiagLog::instance().warn("slave: IDENT_SET rejected — invalid chars in '%s'", tmp);
        _ctx->sendNack(ComponentError::IDENT_INVALID_CHARS);
        return true;
    }
    if (_identStorage.write && !_ident->save(_identStorage.write)) {
        DiagLog::instance().error("slave: IDENT_SET persistence failed for '%s'", tmp);
        _ctx->sendNack(ComponentError::IDENT_PERSIST_FAILED);
        return true;
    }
    DiagLog::instance().info("slave: identifier set to '%s'", tmp);
    _ctx->sendAck();
    return true;
}

// ── Servo dispatch ──────────────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
bool ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::handleServo(const SerialPacket& pkt) {
    if (!_servos) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
    if (pkt.len < 1) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
    uint8_t idx = pkt.payload[0];

    switch (pkt.type) {
        case ComponentPacket::SERVO_SET: {
            if (pkt.len < 3) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t pos = detail::rdU16(pkt.payload + 1);
            if (!_servos->setPosition_us(idx, pos)) {
                _ctx->sendNack(ComponentError::INVALID_INDEX);
                return true;
            }
            _ctx->sendAck();
            return true;
        }
        case ComponentPacket::SERVO_CONFIG: {
            if (pkt.len < 13) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t mn  = detail::rdU16(pkt.payload + 1);
            uint16_t mx  = detail::rdU16(pkt.payload + 3);
            uint16_t cen = detail::rdU16(pkt.payload + 5);
            uint16_t spd = detail::rdU16(pkt.payload + 7);
            uint16_t acc = detail::rdU16(pkt.payload + 9);
            uint16_t dec = detail::rdU16(pkt.payload + 11);
            if (!_servos->setRange(idx, mn, mx, cen) ||
                !_servos->setMotion(idx, spd, acc, dec)) {
                _ctx->sendNack(ComponentError::INVALID_INDEX);
                return true;
            }
            _ctx->sendAck();
            return true;
        }
        case ComponentPacket::SERVO_QUERY: {
            uint16_t pos = 0, tgt = 0;
            int16_t  vel = 0;
            if (!_servos->query(idx, pos, tgt, vel)) {
                _ctx->sendNack(ComponentError::INVALID_INDEX);
                return true;
            }
            uint8_t resp[8];
            resp[0] = idx;
            resp[1] = (uint8_t) pos;        resp[2] = (uint8_t)(pos >> 8);
            resp[3] = (uint8_t) tgt;        resp[4] = (uint8_t)(tgt >> 8);
            resp[5] = (uint8_t) vel;        resp[6] = (uint8_t)(vel >> 8);
            resp[7] = 0;                    // flags reserved
            _ctx->sendRawPacket(ComponentPacket::SERVO_QUERY_RESP, _ctx->currentTag(), resp, sizeof resp);
            return true;
        }
        case ComponentPacket::SERVO_APPLY_JERK: {
            if (pkt.len < 5) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            int16_t  offset = detail::rdI16(pkt.payload + 1);
            uint16_t dur    = detail::rdU16(pkt.payload + 3);
            if (!_servos->applyJerk(idx, offset, dur)) {
                _ctx->sendNack(ComponentError::INVALID_INDEX); return true;
            }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::SERVO_SET_MOTION: {
            if (pkt.len < 7) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t spd = detail::rdU16(pkt.payload + 1);
            uint16_t acc = detail::rdU16(pkt.payload + 3);
            uint16_t dec = detail::rdU16(pkt.payload + 5);
            if (!_servos->setMotion(idx, spd, acc, dec)) {
                _ctx->sendNack(ComponentError::INVALID_INDEX); return true;
            }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::SERVO_HOLD: {
            if (pkt.len < 2) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            if (!_servos->setHold(idx, pkt.payload[1] != 0)) {
                _ctx->sendNack(ComponentError::INVALID_INDEX); return true;
            }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::SERVO_MOTION_UPDATES: {
            // [enable:u8][rate_hz:u8] — note: payload[0] is `enable`,
            // not a channel index, so the dispatch's idx-extract is
            // ignored on this path.
            if (pkt.len < 1) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            const bool    en   = pkt.payload[0] != 0;
            const uint8_t rate = (pkt.len >= 2) ? pkt.payload[1] : 0;
            _servos->setMotionUpdatesEnabled(en, rate);
            _ctx->sendAck(); return true;
        }
    }
    return false;
}

// ── PWM dispatch ────────────────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
bool ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::handlePwm(const SerialPacket& pkt) {
    if (!_pwms) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
    if (pkt.len < 1) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
    uint8_t idx = pkt.payload[0];

    switch (pkt.type) {
        case ComponentPacket::PWM_SET_MODE: {
            if (pkt.len < 2) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            ComponentKind kind = (ComponentKind)pkt.payload[1];
            if (!_pwms->setMode(idx, kind)) { _ctx->sendNack(ComponentError::MODE_NOT_SUPPORTED); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::PWM_SET_DUTY: {
            if (pkt.len < 3) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t duty = detail::rdU16(pkt.payload + 1);
            if (!_pwms->setDuty(idx, duty)) { _ctx->sendNack(ComponentError::WRONG_COMPONENT_KIND); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::PWM_SET_MOTOR: {
            if (pkt.len < 3) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            int16_t speed = detail::rdI16(pkt.payload + 1);
            if (!_pwms->setMotor(idx, speed)) { _ctx->sendNack(ComponentError::WRONG_COMPONENT_KIND); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::PWM_SET_HEATER: {
            if (pkt.len < 3) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t v = detail::rdU16(pkt.payload + 1);
            if (!_pwms->setHeater(idx, v)) { _ctx->sendNack(ComponentError::WRONG_COMPONENT_KIND); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::PWM_SET_FREQ: {
            if (pkt.len < 3) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t f = detail::rdU16(pkt.payload + 1);
            if (!_pwms->setFrequency(idx, f)) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::PWM_RECONFIGURE: {
            if (pkt.len < 7) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            PwmRuntimeConfig cfg{};
            cfg.mode     = (ComponentKind)pkt.payload[1];
            cfg.freq_Hz  = detail::rdU16(pkt.payload + 2);
            cfg.cfgFlags = pkt.payload[4];
            cfg.maxDuty  = detail::rdU16(pkt.payload + 5);
            if (!_pwms->reconfigure(idx, cfg)) { _ctx->sendNack(ComponentError::MODE_NOT_SUPPORTED); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::PWM_QUERY: {
            ComponentKind mode;
            uint16_t duty = 0, freq = 0;
            int32_t  v_mV = 0, c_mA = 0;
            if (!_pwms->query(idx, mode, duty, freq, v_mV, c_mA)) {
                _ctx->sendNack(ComponentError::INVALID_INDEX);
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
            _ctx->sendRawPacket(ComponentPacket::PWM_QUERY_RESP, _ctx->currentTag(), resp, sizeof resp);
            return true;
        }
        case ComponentPacket::PWM_SET_STALL_GUARD: {
            if (pkt.len < 5) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            uint16_t threshold = detail::rdU16(pkt.payload + 1);
            uint8_t  debounce  = pkt.payload[3];
            uint8_t  flags     = pkt.payload[4];
            if (!_pwms->setStallGuard(idx, threshold, debounce, flags)) {
                _ctx->sendNack(ComponentError::INVALID_INDEX); return true;
            }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::PWM_CLEAR_STALL: {
            if (!_pwms->clearStall(idx)) {
                _ctx->sendNack(ComponentError::INVALID_INDEX); return true;
            }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::PWM_GET_CONFIG: {
            PwmRuntimeConfig cfg{};
            if (!_pwms->getRuntimeConfig(idx, cfg)) {
                _ctx->sendNack(ComponentError::INVALID_INDEX); return true;
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
            _ctx->sendRawPacket(ComponentPacket::PWM_GET_CONFIG_RESP, _ctx->currentTag(), resp, sizeof resp);
            return true;
        }
    }
    return false;
}

// ── LED dispatch ────────────────────────────────────────────────────

// ── LED dispatch ───────────────────────────────────────────────────
//
// Address byte bit 7 selects the pool: 0 = dedicated, 1 = PWM-borrowed.
// `resolveLedAddr` bounds-checks and returns the (pool, idx) pair.
// All operations dispatch to whichever LedRuntime owns that pool — both
// are the same template instantiated against different PwmOutput
// backends (NativeGpio/PCA9685 for dedicated, PwmDutyAdapter for
// borrowed), so the dispatch is symmetric.
//
// LED_SET_MASTER_BRIGHTNESS is the one exception — it has no address
// byte (payload is [percent:u8]).  Both pools receive the new master
// brightness so dedicated and borrowed channels scale uniformly.

namespace detail {
    // Local helper — does the routing for ops that look like
    // `bool op(uint8_t idx)`.  Returns the function-style result.
    template <typename Fn>
    inline bool dispatchLed(bool borrowed, uint8_t idx,
                            Fn dedFn, Fn borFn) {
        return borrowed ? borFn(idx) : dedFn(idx);
    }
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
bool ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::handleLed(const SerialPacket& pkt) {
    if (pkt.len < 1) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }

    // LED_SET_MASTER_BRIGHTNESS has no address byte — handle first.
    if (pkt.type == ComponentPacket::LED_SET_MASTER_BRIGHTNESS) {
        uint8_t pct = pkt.payload[0];
        if (_ledsDed) _ledsDed->setMasterBrightness(pct);
        if constexpr (!std::is_same_v<TLedsBor, NoBorrowedLeds>) {
            if (_ledsBor) _ledsBor->setMasterBrightness(pct);
        }
        _ctx->sendAck();
        return true;
    }

    uint8_t addr = pkt.payload[0];
    bool    borrowed;
    uint8_t idx;
    if (!resolveLedAddr(addr, borrowed, idx)) {
        _ctx->sendNack(ComponentError::INVALID_INDEX); return true;
    }
    if (borrowed) {
        if constexpr (std::is_same_v<TLedsBor, NoBorrowedLeds>) {
            _ctx->sendNack(ComponentError::INVALID_INDEX); return true;
        } else {
            // Verify the PWM channel is currently in PwmLed mode —
            // otherwise the master is addressing a borrowed slot
            // whose PWM channel has been switched to a different role.
            if (_pwms && _pwms->currentMode(idx) != sfx_peripherals::ComponentKind::PwmLed) {
                _ctx->sendNack(ComponentError::WRONG_COMPONENT_KIND); return true;
            }
        }
    }

    auto invoke = [&](auto&& fnDed, auto&& fnBor) -> bool {
        if (borrowed) {
            if constexpr (!std::is_same_v<TLedsBor, NoBorrowedLeds>) {
                if (_ledsBor) return fnBor(*_ledsBor);
            }
            return false;
        }
        if (_ledsDed) return fnDed(*_ledsDed);
        return false;
    };

    switch (pkt.type) {
        case ComponentPacket::LED_SET_BRIGHTNESS: {
            if (pkt.len < 2) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            const uint8_t bri = pkt.payload[1];
            bool ok = invoke(
                [&](auto& r) { return r.setBrightness(idx, bri); },
                [&](auto& r) { return r.setBrightness(idx, bri); });
            if (!ok) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::LED_QUEUE_LOAD: {
            if (pkt.len < 3) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            const uint8_t flags = pkt.payload[1];        // LedQueueFlags::*
            const uint8_t count = pkt.payload[2];
            using ComponentPacket::LedEvent;
            if (pkt.len < (size_t)(3 + count * sizeof(LedEvent))) {
                _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true;
            }
            const LedEvent* events = reinterpret_cast<const LedEvent*>(pkt.payload + 3);
            bool ok = invoke(
                [&](auto& r) { return r.loadQueue(idx, flags, events, count); },
                [&](auto& r) { return r.loadQueue(idx, flags, events, count); });
            if (!ok) { _ctx->sendNack(ComponentError::QUEUE_TOO_LARGE); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::LED_QUEUE_START: {
            bool ok = invoke(
                [&](auto& r) { return r.startQueue(idx); },
                [&](auto& r) { return r.startQueue(idx); });
            if (!ok) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::LED_QUEUE_STOP: {
            bool ok = invoke(
                [&](auto& r) { return r.stopQueue(idx); },
                [&](auto& r) { return r.stopQueue(idx); });
            if (!ok) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::LED_QUERY: {
            ComponentPacket::LedChannelStatus st{};
            bool ok = invoke(
                [&](auto& r) { return r.query(idx, st); },
                [&](auto& r) { return r.query(idx, st); });
            if (!ok) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
            // Re-wrap the address byte so the master sees the same
            // form it sent (LedRuntime::query writes the plain index).
            uint8_t resp[4] = { addr, st.brightness, st.queueState, st.currentEvent };
            _ctx->sendRawPacket(ComponentPacket::LED_QUERY_RESP, _ctx->currentTag(), resp, sizeof resp);
            return true;
        }
        case ComponentPacket::LED_QUEUE_RESTART: {
            bool ok = invoke(
                [&](auto& r) { return r.restartQueue(idx); },
                [&](auto& r) { return r.restartQueue(idx); });
            if (!ok) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::LED_RESET_CHANNEL: {
            bool ok = invoke(
                [&](auto& r) { return r.resetChannel(idx); },
                [&](auto& r) { return r.resetChannel(idx); });
            if (!ok) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::LED_ENABLE_CHANNEL: {
            if (pkt.len < 2) { _ctx->sendNack(SerialError::INVALID_PAYLOAD_LENGTH); return true; }
            const bool en = (pkt.payload[1] != 0);
            bool ok = invoke(
                [&](auto& r) { return r.enableChannel(idx, en); },
                [&](auto& r) { return r.enableChannel(idx, en); });
            if (!ok) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
            _ctx->sendAck(); return true;
        }
        case ComponentPacket::LED_QUEUE_STATUS_REQ: {
            ComponentPacket::LedQueueStatus st{};
            bool ok = invoke(
                [&](auto& r) { return r.queueStatus(idx, st); },
                [&](auto& r) { return r.queueStatus(idx, st); });
            if (!ok) { _ctx->sendNack(ComponentError::INVALID_INDEX); return true; }
            // Wire format: prefixed addr byte (master sent form), then
            // the LedQueueStatus struct verbatim.
            uint8_t resp[1 + sizeof(ComponentPacket::LedQueueStatus)];
            resp[0] = addr;
            memcpy(resp + 1, &st, sizeof st);
            _ctx->sendRawPacket(ComponentPacket::LED_QUEUE_STATUS_RESP, _ctx->currentTag(),
                          resp, sizeof resp);
            return true;
        }
    }
    return false;
}

// ── Async event emitters ────────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
void ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::emitServoTargetReached(uint8_t idx, uint16_t pos_us) {
    uint8_t buf[3] = { idx, (uint8_t)pos_us, (uint8_t)(pos_us >> 8) };
    _ctx->sendRawPacket(ComponentPacket::SERVO_TARGET_REACHED, CoreProtocol::TAG_ASYNC, buf, sizeof buf);
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
void ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::emitServoMotionUpdate(
        uint8_t idx, uint16_t pos, uint16_t target, int16_t vel) {
    uint8_t buf[7];
    buf[0] = idx;
    buf[1] = (uint8_t) pos;     buf[2] = (uint8_t)(pos    >> 8);
    buf[3] = (uint8_t) target;  buf[4] = (uint8_t)(target >> 8);
    buf[5] = (uint8_t) vel;     buf[6] = (uint8_t)(vel    >> 8);
    _ctx->sendRawPacket(ComponentPacket::SERVO_MOTION_UPDATE, CoreProtocol::TAG_ASYNC, buf, sizeof buf);
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
void ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::emitLedQueueDone(uint8_t addr) {
    uint8_t buf[1] = { addr };
    _ctx->sendRawPacket(ComponentPacket::LED_QUEUE_DONE, CoreProtocol::TAG_ASYNC, buf, sizeof buf);
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
void ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::emitPwmStall(uint8_t idx, uint16_t peak_mA, uint16_t duration_ms) {
    // Stall is rare and informative — log at warn so the master sees a
    // history entry alongside the async PWM_STALL packet.
    DiagLog::instance().warn("pwm[%u]: stall guard tripped — peak=%u mA after %u ms",
                             (unsigned)idx, (unsigned)peak_mA, (unsigned)duration_ms);
    uint8_t buf[5];
    buf[0] = idx;
    buf[1] = (uint8_t) peak_mA;      buf[2] = (uint8_t)(peak_mA >> 8);
    buf[3] = (uint8_t) duration_ms;  buf[4] = (uint8_t)(duration_ms >> 8);
    _ctx->sendRawPacket(ComponentPacket::PWM_STALL, CoreProtocol::TAG_ASYNC, buf, sizeof buf);
}

// ── Unified status broadcast ────────────────────────────────────────

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
void ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::checkStatusBroadcast() {
    if (_statusBroadcastRate_hz == 0) return;
    uint32_t period_ms = 1000u / _statusBroadcastRate_hz;
    if ((uint32_t)(millis() - _lastStatusBroadcast_ms) < period_ms) return;
    _lastStatusBroadcast_ms = millis();
    emitStatusBroadcast();
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
size_t ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::buildStatusPayload(
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

    const bool wantAll     = (kindsMask == ComponentPacket::StatusKinds::ALL);
    const bool wantServo   = wantAll || (kindsMask & ComponentPacket::StatusKinds::SERVO);
    const bool wantPwm     = wantAll || (kindsMask & ComponentPacket::StatusKinds::PWM);
    const bool wantLed     = wantAll || (kindsMask & ComponentPacket::StatusKinds::LED);
    const bool wantBattery = wantAll || (kindsMask & ComponentPacket::StatusKinds::BATTERY);
    const bool headerOnly  = (kindsMask & ComponentPacket::StatusKinds::HEADER_ONLY) != 0;

    // ── Servos — entry: [port_id][pos:u16][target:u16][vel:i16][flags:u8] = 9 bytes
    if (off >= bufSize) return off;
    uint8_t* servoCount = &buf[off++];
    *servoCount = 0;
    if (_servos && wantServo && !headerOnly) {
        for (size_t i = 0; i < TServos::COUNT && off + 9 <= bufSize; i++) {
            uint16_t pos = 0, tgt = 0; int16_t vel = 0;
            _servos->query((uint8_t)i, pos, tgt, vel);
            buf[off++] = ComponentPacket::PortId::make(ComponentPacket::PortId::Servo, (uint8_t)i);
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
            buf[off++] = ComponentPacket::PortId::make(ComponentPacket::PortId::Pwm, (uint8_t)i);
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

    // ── LEDs — entry: [port_id][brightness][queueState][currentEvent] = 4 bytes
    //
    // Both pools are walked: dedicated channels always present, then
    // PWM-borrowed channels that are currently in PwmLed mode (others
    // appear in the PWM section above with their actual ComponentKind).
    if (off >= bufSize) return off;
    uint8_t* ledCount = &buf[off++];
    *ledCount = 0;
    if (wantLed && !headerOnly) {
        // Dedicated pool
        if (_ledsDed) {
            for (size_t i = 0; i < TLedsDed::COUNT && off + 4 <= bufSize; i++) {
                ComponentPacket::LedChannelStatus st{};
                _ledsDed->query((uint8_t)i, st);
                buf[off++] = ComponentPacket::PortId::make(ComponentPacket::PortId::LedDed, (uint8_t)i);
                buf[off++] = st.brightness;
                buf[off++] = st.queueState;
                buf[off++] = st.currentEvent;
                (*ledCount)++;
            }
        }
        // PWM-borrowed pool — only includes channels currently in PwmLed mode.
        if constexpr (!std::is_same_v<TLedsBor, NoBorrowedLeds>) {
            if (_ledsBor && _pwms) {
                for (size_t i = 0; i < TLedsBor::COUNT && off + 4 <= bufSize; i++) {
                    if (_pwms->currentMode((uint8_t)i) != sfx_peripherals::ComponentKind::PwmLed) continue;
                    ComponentPacket::LedChannelStatus st{};
                    _ledsBor->query((uint8_t)i, st);
                    buf[off++] = ComponentPacket::PortId::make(ComponentPacket::PortId::LedPwm, (uint8_t)i);
                    buf[off++] = st.brightness;
                    buf[off++] = st.queueState;
                    buf[off++] = st.currentEvent;
                    (*ledCount)++;
                }
            }
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

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
void ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::emitStatusBroadcast() {
    uint8_t buf[768];
    size_t  n = buildStatusPayload(buf, sizeof buf, _statusKindsMask);
    _ctx->sendRawPacket(ComponentPacket::COMPONENT_STATUS_BROADCAST,
                  CoreProtocol::TAG_ASYNC, buf, n);
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
bool ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::handleStatusReq(uint8_t kindsMask) {
    uint8_t buf[768];
    size_t  n = buildStatusPayload(buf, sizeof buf, kindsMask);
    _ctx->sendRawPacket(ComponentPacket::COMPONENT_STATUS_BROADCAST,
                  _ctx->currentTag(), buf, n);
    return true;
}

// ── Battery dispatch + alert emission + status section ─────────────

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
bool ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::handleBatteryInfo(const SerialPacket&) {
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

            uint8_t flags = ComponentPacket::BatteryFlags::PRESENT;
            if (_battery->isLowTriggered())      flags |= ComponentPacket::BatteryFlags::LOW_TRIGGERED;
            if (_battery->isCriticalTriggered()) flags |= ComponentPacket::BatteryFlags::CRITICAL_TRIGGERED;
            if (_battery->isCellCountManual())   flags |= ComponentPacket::BatteryFlags::MANUAL_CELL_COUNT;
            if (_battery->isUsbPowered())        flags |= ComponentPacket::BatteryFlags::USB_POWERED;

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

    _ctx->sendRawPacket(ComponentPacket::BATTERY_INFO_RESP, _ctx->currentTag(), buf, off);
    return true;
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
bool ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::handleBatteryReconfigure(const SerialPacket& pkt) {
    // [chemistry:u8][cellCount:u8][customLow_mV:u16LE][customCritical_mV:u16LE]
    if (pkt.len < 6) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return true;
    }
    const uint8_t  chemistry  = pkt.payload[0];
    const uint8_t  cellCount  = pkt.payload[1];
    const uint16_t low_mV     = detail::rdU16(&pkt.payload[2]);
    const uint16_t crit_mV    = detail::rdU16(&pkt.payload[4]);

    if (chemistry > (uint8_t)BatteryChemistry::NIMH) {
        _ctx->sendNack(ComponentError::BATTERY_INVALID_CHEMISTRY);
        return true;
    }

    if constexpr (std::is_same_v<TBattery, NoBattery>) {
        _ctx->sendNack(ComponentError::BATTERY_NOT_PRESENT);
        return true;
    } else {
        if (!_battery) {
            _ctx->sendNack(ComponentError::BATTERY_NOT_PRESENT);
            return true;
        }
        _battery->setChemistry((BatteryChemistry)chemistry);
        _battery->setCellCount(cellCount);
        if (low_mV  != 0) _battery->setLowThreshold_mV(low_mV);
        if (crit_mV != 0) _battery->setCriticalThreshold_mV(crit_mV);
        DiagLog::instance().info("battery: reconfigured chem=%u cells=%u low=%u crit=%u (mV/cell)",
                                 (unsigned)chemistry, (unsigned)cellCount,
                                 (unsigned)low_mV, (unsigned)crit_mV);
        _ctx->sendAck();
        return true;
    }
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
void ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::emitBatteryAlert(uint8_t  level,
                                                                   uint16_t voltage_mV,
                                                                   uint8_t  cellCount) {
    // Log the transition.  Hysteresis in BatteryStateMachine ensures we
    // don't spam — each level transition is a single edge per crossing.
    const char* levelName =
        (level == ComponentPacket::BatteryAlertLevel::CRITICAL) ? "CRITICAL" :
        (level == ComponentPacket::BatteryAlertLevel::LOW)      ? "LOW"      : "OK";
    if (level == ComponentPacket::BatteryAlertLevel::CRITICAL) {
        DiagLog::instance().error("battery: %s — %u mV across %u cell(s)",
                                  levelName, (unsigned)voltage_mV, (unsigned)cellCount);
    } else if (level == ComponentPacket::BatteryAlertLevel::LOW) {
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
    _ctx->sendRawPacket(ComponentPacket::BATTERY_ALERT, CoreProtocol::TAG_ASYNC, buf, sizeof buf);
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
size_t ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::appendBatterySection(uint8_t* buf,
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

        uint8_t flags = ComponentPacket::BatteryFlags::PRESENT;
        if (_battery->isLowTriggered())      flags |= ComponentPacket::BatteryFlags::LOW_TRIGGERED;
        if (_battery->isCriticalTriggered()) flags |= ComponentPacket::BatteryFlags::CRITICAL_TRIGGERED;
        if (_battery->isCellCountManual())   flags |= ComponentPacket::BatteryFlags::MANUAL_CELL_COUNT;
        if (_battery->isUsbPowered())        flags |= ComponentPacket::BatteryFlags::USB_POWERED;

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

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
void ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::noteMasterTraffic() {
    _lastKeepalive_ms = millis();
}

template <typename TServos, typename TPwms, typename TLedsDed, typename TLedsBor, typename TBattery>
void ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>::checkKeepaliveTimeout() {
    // Only enforce in SLAVE mode — DIRECT is the bench-test path.
    if (_initMode != BoardMode::SLAVE) return;
    if (!_attached) return;     // already in safe state — don't log every loop tick
    if ((uint32_t)(millis() - _lastKeepalive_ms) < _keepaliveTimeout_ms) return;
    DiagLog::instance().warn("slave: keepalive timeout (%lu ms since last master traffic) — entering safe state",
                             (unsigned long)(millis() - _lastKeepalive_ms));
    enterSafeState();
}

}  // namespace sfx_core

#endif  // SFX_COMPONENT_SERVICE_IPP
