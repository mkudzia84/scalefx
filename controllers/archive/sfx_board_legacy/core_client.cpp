/*
 * CoreClient — implementation of the typed master-side client.
 *
 * Pattern: each method builds the wire payload, calls into BusClient
 * via `sendCommand` (instant ACK/NACK) or `sendQuery` (typed response
 * decoded by the caller).  Async packets are routed in
 * onModulePacket() to the registered observer fanout.
 *
 * Endianness: all multi-byte integers are little-endian on the wire
 * per CLAUDE.md Rule 4.
 */

#include "core_client.h"

#include <cstring>

namespace sfx_core {

namespace {

inline void putU16LE(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

inline uint16_t getU16LE(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

inline int16_t getI16LE(const uint8_t* p) {
    return (int16_t)getU16LE(p);
}

inline int32_t getI32LE(const uint8_t* p) {
    return (int32_t)((uint32_t)p[0]
                     | ((uint32_t)p[1] << 8)
                     | ((uint32_t)p[2] << 16)
                     | ((uint32_t)p[3] << 24));
}

inline uint32_t getU32LE(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

}  // anonymous namespace

// ── Module-packet routing (async events only) ────────────────────────
//
// Solicited query responses (LED_QUERY_RESP, PWM_QUERY_RESP, ...) are
// captured by BusClient::sendQuery() before this hook fires, so we only
// need to handle slave-initiated async events here (TAG_ASYNC).

void CoreClient::onModulePacket(uint8_t type, uint8_t /*tag*/,
                                const uint8_t* payload, size_t len) {
    switch (type) {
        case ComponentPacket::SERVO_TARGET_REACHED:       decodeServoTargetReached(payload, len); break;
        case ComponentPacket::SERVO_MOTION_UPDATE:        decodeServoMotionUpdate (payload, len); break;
        case ComponentPacket::PWM_STALL:                  decodePwmStall          (payload, len); break;
        case ComponentPacket::LED_QUEUE_DONE:             decodeLedQueueDone      (payload, len); break;
        case ComponentPacket::BATTERY_ALERT:              decodeBatteryAlert      (payload, len); break;
        case ComponentPacket::COMPONENT_STATUS_BROADCAST: decodeStatusBroadcast   (payload, len); break;
        default: break;
    }
}

void CoreClient::decodeServoTargetReached(const uint8_t* p, size_t len) {
    if (len < 3) return;
    const uint8_t  idx = p[0];
    const uint16_t pos = getU16LE(p + 1);
    for (auto& cb : _onTargetReached) cb(idx, pos);
}

void CoreClient::decodeServoMotionUpdate(const uint8_t* p, size_t len) {
    if (len < 7) return;
    const uint8_t  idx    = p[0];
    const uint16_t pos    = getU16LE(p + 1);
    const uint16_t target = getU16LE(p + 3);
    const int16_t  vel    = getI16LE(p + 5);
    for (auto& cb : _onMotionUpdate) cb(idx, pos, target, vel);
}

void CoreClient::decodePwmStall(const uint8_t* p, size_t len) {
    if (len < 5) return;
    const uint8_t  idx     = p[0];
    const uint16_t peak_mA = getU16LE(p + 1);
    const uint16_t dur_ms  = getU16LE(p + 3);
    for (auto& cb : _onPwmStall) cb(idx, peak_mA, dur_ms);
}

void CoreClient::decodeLedQueueDone(const uint8_t* p, size_t len) {
    if (len < 1) return;
    for (auto& cb : _onLedQueueDone) cb(p[0]);
}

void CoreClient::decodeBatteryAlert(const uint8_t* p, size_t len) {
    if (len < 4) return;
    BatteryAlert a{};
    a.level      = p[0];
    a.voltage_mV = getU16LE(p + 1);
    a.cellCount  = p[3];
    for (auto& cb : _onBatteryAlert) cb(a);
}

void CoreClient::decodeBatteryInfoPayload(const uint8_t* p, size_t len, BatteryInfo& out) {
    out = BatteryInfo{};
    if (len < 1) return;
    out.present = (p[0] != 0);
    if (!out.present || len < 14) return;
    out.chemistry         = (BatteryChemistry)p[1];
    out.cellCount         = p[2];
    out.voltage_mV        = getU16LE(p + 3);
    out.cellVoltage_mV    = getU16LE(p + 5);
    out.percentage        = p[7];
    out.flags             = p[8];
    out.profileLow_mV     = getU16LE(p + 9);
    out.profileCritical_mV = getU16LE(p + 11);
}

void CoreClient::decodeBatterySection(const uint8_t* p, size_t len,
                                       size_t& off, BatteryInfo& out) {
    out = BatteryInfo{};
    if (off >= len) return;
    out.present = (p[off++] != 0);
    if (!out.present) return;
    if (off + 8 > len) { out.present = false; return; }
    out.chemistry      = (BatteryChemistry)p[off + 0];
    out.cellCount      = p[off + 1];
    out.voltage_mV     = getU16LE(p + off + 2);
    out.cellVoltage_mV = getU16LE(p + off + 4);
    out.percentage     = p[off + 6];
    out.flags          = p[off + 7];
    off += 8;
}

void CoreClient::decodeStatusBroadcast(const uint8_t* p, size_t len) {
    if (len < 10) return;
    SlaveStatus st{};
    st.boardState    = p[0];
    st.initMode      = p[1];
    st.uptime_ms     = getU32LE(p + 2);
    st.freeRam_bytes = getU32LE(p + 6);
    size_t off = 10;

    // Servos — 9 bytes each
    if (off < len) {
        uint8_t count = p[off++];
        st.servos.reserve(count);
        for (uint8_t i = 0; i < count && off + 9 <= len; i++) {
            ServoStatus s{};
            s.idx               = p[off + 0];
            s.pos_us            = getU16LE(p + off + 1);
            s.target_us         = getU16LE(p + off + 3);
            s.velocity_us_per_s = getI16LE(p + off + 5);
            s.flags             = p[off + 7];
            // p[off + 8] reserved
            st.servos.push_back(s);
            off += 9;
        }
    }

    // PWMs — 11 bytes each: idx, mode, duty_lo, duty_hi, V_lo, V_hi,
    // I_lo, I_hi, stallFlags, peak_lo, peak_hi
    if (off < len) {
        uint8_t count = p[off++];
        st.pwms.reserve(count);
        st.pwmStalls.reserve(count);
        for (uint8_t i = 0; i < count && off + 11 <= len; i++) {
            PwmStatus pw{};
            pw.idx              = p[off + 0];
            pw.mode             = (sfx_peripherals::ComponentKind)p[off + 1];
            pw.duty_thousandths = getU16LE(p + off + 2);
            pw.voltage_mV       = (int32_t)getI16LE(p + off + 4);   // sign-extended
            pw.current_mA       = (int32_t)getI16LE(p + off + 6);
            pw.freq_Hz          = 0;   // not in broadcast — query separately
            st.pwms.push_back(pw);

            PwmStallStatus ps{};
            ps.flags   = p[off + 8];
            ps.peak_mA = getU16LE(p + off + 9);
            st.pwmStalls.push_back(ps);

            off += 11;
        }
    }

    // LEDs — 4 bytes each
    if (off < len) {
        uint8_t count = p[off++];
        st.leds.reserve(count);
        for (uint8_t i = 0; i < count && off + 4 <= len; i++) {
            LedStatus l{};
            l.addr         = p[off + 0];
            l.brightness   = p[off + 1];
            l.queueState   = p[off + 2];
            l.currentEvent = p[off + 3];
            st.leds.push_back(l);
            off += 4;
        }
    }

    // Battery — variable: [present:u8] + (if present) 8 more bytes.
    decodeBatterySection(p, len, off, st.battery);

    for (auto& cb : _onStatusBroadcast) cb(st);
}

// ── Identity / enumeration ───────────────────────────────────────────

CommandResult CoreClient::requestComponentList(
        std::vector<sfx_peripherals::ComponentInfo>& out) {
    SerialPacket resp;
    auto cr = sendQuery(ComponentPacket::COMPONENT_LIST_REQ, nullptr, 0,
                        ComponentPacket::COMPONENT_LIST_RESP, resp);
    if (!cr.success) return cr;
    if (resp.len < 1) return CommandResult::Nack(SerialError::MISSING_PARAMETER);

    const uint8_t count = resp.payload[0];
    if (resp.len < (size_t)(1 + count * sizeof(sfx_peripherals::ComponentInfo))) {
        return CommandResult::Nack(SerialError::MISSING_PARAMETER);
    }
    out.clear();
    out.reserve(count);
    const uint8_t* src = resp.payload + 1;
    for (uint8_t i = 0; i < count; i++) {
        sfx_peripherals::ComponentInfo info;
        memcpy(&info, src + i * sizeof(info), sizeof(info));
        out.push_back(info);
    }
    return CommandResult::Ack();
}

CommandResult CoreClient::getIdentifier(uint8_t& out_boardType,
                                         char* out_name, size_t bufLen) {
    SerialPacket resp;
    auto cr = sendQuery(ComponentPacket::IDENT_GET_REQ, nullptr, 0,
                        ComponentPacket::IDENT_GET_RESP, resp);
    if (!cr.success) return cr;
    if (resp.len < 2) return CommandResult::Nack(SerialError::MISSING_PARAMETER);

    out_boardType  = resp.payload[0];
    const uint8_t l = resp.payload[1];
    if (resp.len < (size_t)(2 + l)) return CommandResult::Nack(SerialError::MISSING_PARAMETER);
    if (out_name && bufLen > 0) {
        const size_t copy = (l + 1u <= bufLen) ? l : (bufLen - 1);
        memcpy(out_name, resp.payload + 2, copy);
        out_name[copy] = 0;
    }
    return CommandResult::Ack();
}

CommandResult CoreClient::setIdentifier(const char* name) {
    if (!name) return CommandResult::Nack(SerialError::INVALID_PARAM);
    const size_t l = strlen(name);
    if (l > 32) return CommandResult::Nack(ComponentError::IDENT_TOO_LONG);
    uint8_t buf[33];
    buf[0] = (uint8_t)l;
    memcpy(buf + 1, name, l);
    return sendCommand(ComponentPacket::IDENT_SET, buf, 1 + l);
}

CommandResult CoreClient::requestStatus(SlaveStatus& out, uint8_t kindsMask) {
    SerialPacket resp;
    uint8_t req[1] = { kindsMask };
    auto cr = sendQuery(ComponentPacket::COMPONENT_STATUS_REQ, req, sizeof req,
                        ComponentPacket::COMPONENT_STATUS_BROADCAST, resp);
    if (!cr.success) return cr;
    if (resp.len < 10) return CommandResult::Nack(SerialError::MISSING_PARAMETER);

    // Decode directly into `out` — single-pass, no observer fanout
    // duplicate (the broadcast decoder fans out for unsolicited
    // packets; sync-poll callers typically don't want that double
    // dispatch).  Logic mirrors decodeStatusBroadcast() — kept inline
    // because the observer fanout vs direct-fill split matters here.
    out = SlaveStatus{};
    out.boardState    = resp.payload[0];
    out.initMode      = resp.payload[1];
    out.uptime_ms     = getU32LE(resp.payload + 2);
    out.freeRam_bytes = getU32LE(resp.payload + 6);
    size_t off = 10;
    const uint8_t* p = resp.payload;
    const size_t   len = resp.len;

    if (off < len) {
        uint8_t count = p[off++];
        out.servos.reserve(count);
        for (uint8_t i = 0; i < count && off + 9 <= len; i++) {
            ServoStatus s{};
            s.idx = p[off + 0];
            s.pos_us = getU16LE(p + off + 1);
            s.target_us = getU16LE(p + off + 3);
            s.velocity_us_per_s = getI16LE(p + off + 5);
            s.flags = p[off + 7];
            out.servos.push_back(s);
            off += 9;
        }
    }
    if (off < len) {
        uint8_t count = p[off++];
        out.pwms.reserve(count);
        out.pwmStalls.reserve(count);
        for (uint8_t i = 0; i < count && off + 11 <= len; i++) {
            PwmStatus pw{};
            pw.idx = p[off + 0];
            pw.mode = (sfx_peripherals::ComponentKind)p[off + 1];
            pw.duty_thousandths = getU16LE(p + off + 2);
            pw.voltage_mV = (int32_t)getI16LE(p + off + 4);
            pw.current_mA = (int32_t)getI16LE(p + off + 6);
            pw.freq_Hz = 0;
            out.pwms.push_back(pw);

            PwmStallStatus ps{};
            ps.flags = p[off + 8];
            ps.peak_mA = getU16LE(p + off + 9);
            out.pwmStalls.push_back(ps);
            off += 11;
        }
    }
    if (off < len) {
        uint8_t count = p[off++];
        out.leds.reserve(count);
        for (uint8_t i = 0; i < count && off + 4 <= len; i++) {
            LedStatus ll{};
            ll.addr         = p[off + 0];
            ll.brightness   = p[off + 1];
            ll.queueState   = p[off + 2];
            ll.currentEvent = p[off + 3];
            out.leds.push_back(ll);
            off += 4;
        }
    }
    decodeBatterySection(p, len, off, out.battery);
    return CommandResult::Ack();
}

CommandResult CoreClient::setStatusRate(uint8_t hz, uint8_t kindsMask) {
    uint8_t buf[2] = { hz, kindsMask };
    return sendCommand(ComponentPacket::COMPONENT_STATUS_RATE, buf, sizeof buf);
}

// ── Battery ──────────────────────────────────────────────────────────

CommandResult CoreClient::requestBatteryInfo(BatteryInfo& out) {
    SerialPacket resp;
    auto cr = sendQuery(ComponentPacket::BATTERY_INFO_REQ, nullptr, 0,
                        ComponentPacket::BATTERY_INFO_RESP, resp);
    if (!cr.success) return cr;
    decodeBatteryInfoPayload(resp.payload, resp.len, out);
    return CommandResult::Ack();
}

CommandResult CoreClient::batteryReconfigure(BatteryChemistry chemistry,
                                              uint8_t          cellCount,
                                              uint16_t         customLow_mV,
                                              uint16_t         customCritical_mV) {
    uint8_t buf[6];
    buf[0] = (uint8_t)chemistry;
    buf[1] = cellCount;
    putU16LE(buf + 2, customLow_mV);
    putU16LE(buf + 4, customCritical_mV);
    return sendCommand(ComponentPacket::BATTERY_RECONFIGURE, buf, sizeof buf);
}

// ── Servo ────────────────────────────────────────────────────────────

CommandResult CoreClient::servoSet(uint8_t idx, uint16_t pulse_us) {
    uint8_t buf[3];
    buf[0] = idx;
    putU16LE(buf + 1, pulse_us);
    return sendCommand(ComponentPacket::SERVO_SET, buf, sizeof buf);
}

CommandResult CoreClient::servoConfig(uint8_t idx, const ServoCalibration& cal) {
    uint8_t buf[13];
    buf[0] = idx;
    putU16LE(buf + 1,  cal.min_us);
    putU16LE(buf + 3,  cal.max_us);
    putU16LE(buf + 5,  cal.center_us);
    putU16LE(buf + 7,  cal.maxSpeed);
    putU16LE(buf + 9,  cal.accel);
    putU16LE(buf + 11, cal.decel);
    return sendCommand(ComponentPacket::SERVO_CONFIG, buf, sizeof buf);
}

CommandResult CoreClient::servoSetMotion(uint8_t idx, uint16_t maxSpeed,
                                          uint16_t accel, uint16_t decel) {
    uint8_t buf[7];
    buf[0] = idx;
    putU16LE(buf + 1, maxSpeed);
    putU16LE(buf + 3, accel);
    putU16LE(buf + 5, decel);
    return sendCommand(ComponentPacket::SERVO_SET_MOTION, buf, sizeof buf);
}

CommandResult CoreClient::servoApplyJerk(uint8_t idx, int16_t offset_us, uint16_t duration_ms) {
    uint8_t buf[5];
    buf[0] = idx;
    putU16LE(buf + 1, (uint16_t)offset_us);
    putU16LE(buf + 3, duration_ms);
    return sendCommand(ComponentPacket::SERVO_APPLY_JERK, buf, sizeof buf);
}

CommandResult CoreClient::servoHold(uint8_t idx, bool hold) {
    uint8_t buf[2] = { idx, (uint8_t)(hold ? 1 : 0) };
    return sendCommand(ComponentPacket::SERVO_HOLD, buf, sizeof buf);
}

CommandResult CoreClient::servoQuery(uint8_t idx, ServoStatus& out) {
    SerialPacket resp;
    uint8_t req[1] = { idx };
    auto cr = sendQuery(ComponentPacket::SERVO_QUERY, req, sizeof req,
                        ComponentPacket::SERVO_QUERY_RESP, resp);
    if (!cr.success) return cr;
    if (resp.len < 8) return CommandResult::Nack(SerialError::MISSING_PARAMETER);
    out.idx               = resp.payload[0];
    out.pos_us            = getU16LE(resp.payload + 1);
    out.target_us         = getU16LE(resp.payload + 3);
    out.velocity_us_per_s = getI16LE(resp.payload + 5);
    out.flags             = resp.payload[7];
    return CommandResult::Ack();
}

CommandResult CoreClient::servoMotionUpdates(bool enable, uint8_t rate_hz) {
    uint8_t buf[2] = { (uint8_t)(enable ? 1 : 0), rate_hz };
    return sendCommand(ComponentPacket::SERVO_MOTION_UPDATES, buf, sizeof buf);
}

// ── PWM ──────────────────────────────────────────────────────────────

CommandResult CoreClient::pwmSetMode(uint8_t idx, sfx_peripherals::ComponentKind mode) {
    uint8_t buf[2] = { idx, (uint8_t)mode };
    return sendCommand(ComponentPacket::PWM_SET_MODE, buf, sizeof buf);
}

CommandResult CoreClient::pwmSetDuty(uint8_t idx, uint16_t duty_thousandths) {
    uint8_t buf[3];
    buf[0] = idx;
    putU16LE(buf + 1, duty_thousandths);
    return sendCommand(ComponentPacket::PWM_SET_DUTY, buf, sizeof buf);
}

CommandResult CoreClient::pwmSetMotor(uint8_t idx, int16_t speed_signed) {
    uint8_t buf[3];
    buf[0] = idx;
    putU16LE(buf + 1, (uint16_t)speed_signed);
    return sendCommand(ComponentPacket::PWM_SET_MOTOR, buf, sizeof buf);
}

CommandResult CoreClient::pwmSetHeater(uint8_t idx, uint16_t value) {
    uint8_t buf[3];
    buf[0] = idx;
    putU16LE(buf + 1, value);
    return sendCommand(ComponentPacket::PWM_SET_HEATER, buf, sizeof buf);
}

CommandResult CoreClient::pwmSetFrequency(uint8_t idx, uint16_t freq_Hz) {
    uint8_t buf[3];
    buf[0] = idx;
    putU16LE(buf + 1, freq_Hz);
    return sendCommand(ComponentPacket::PWM_SET_FREQ, buf, sizeof buf);
}

CommandResult CoreClient::pwmReconfigure(uint8_t idx, const PwmRuntimeConfig& cfg) {
    uint8_t buf[7];
    buf[0] = idx;
    buf[1] = (uint8_t)cfg.mode;
    putU16LE(buf + 2, cfg.freq_Hz);
    buf[4] = cfg.cfgFlags;
    putU16LE(buf + 5, cfg.maxDuty);
    return sendCommand(ComponentPacket::PWM_RECONFIGURE, buf, sizeof buf);
}

CommandResult CoreClient::pwmQuery(uint8_t idx, PwmStatus& out) {
    SerialPacket resp;
    uint8_t req[1] = { idx };
    auto cr = sendQuery(ComponentPacket::PWM_QUERY, req, sizeof req,
                        ComponentPacket::PWM_QUERY_RESP, resp);
    if (!cr.success) return cr;
    if (resp.len < 14) return CommandResult::Nack(SerialError::MISSING_PARAMETER);
    out.idx              = resp.payload[0];
    out.mode             = (sfx_peripherals::ComponentKind)resp.payload[1];
    out.duty_thousandths = getU16LE(resp.payload + 2);
    out.freq_Hz          = getU16LE(resp.payload + 4);
    out.voltage_mV       = getI32LE(resp.payload + 6);
    out.current_mA       = getI32LE(resp.payload + 10);
    return CommandResult::Ack();
}

CommandResult CoreClient::pwmGetConfig(uint8_t idx, PwmConfig& out) {
    SerialPacket resp;
    uint8_t req[1] = { idx };
    auto cr = sendQuery(ComponentPacket::PWM_GET_CONFIG, req, sizeof req,
                        ComponentPacket::PWM_GET_CONFIG_RESP, resp);
    if (!cr.success) return cr;
    if (resp.len < 10) return CommandResult::Nack(SerialError::MISSING_PARAMETER);
    out.idx             = resp.payload[0];
    out.mode            = (sfx_peripherals::ComponentKind)resp.payload[1];
    out.freq_Hz         = getU16LE(resp.payload + 2);
    out.cfgFlags        = resp.payload[4];
    out.maxDuty         = getU16LE(resp.payload + 5);
    out.hwFlags         = resp.payload[7];
    out.voltageSenseIdx = resp.payload[8];
    out.currentSenseIdx = resp.payload[9];
    out.pairedWith      = (resp.len >= 11) ? resp.payload[10] : 0xFF;
    return CommandResult::Ack();
}

CommandResult CoreClient::pwmSetStallGuard(uint8_t idx, uint16_t threshold_mA,
                                            uint8_t debounce_ms, uint8_t flags) {
    uint8_t buf[5];
    buf[0] = idx;
    putU16LE(buf + 1, threshold_mA);
    buf[3] = debounce_ms;
    buf[4] = flags;
    return sendCommand(ComponentPacket::PWM_SET_STALL_GUARD, buf, sizeof buf);
}

CommandResult CoreClient::pwmClearStall(uint8_t idx) {
    uint8_t buf[1] = { idx };
    return sendCommand(ComponentPacket::PWM_CLEAR_STALL, buf, sizeof buf);
}

// ── LED ──────────────────────────────────────────────────────────────

CommandResult CoreClient::ledSetBrightness(uint8_t addr, uint8_t brightness) {
    uint8_t buf[2] = { addr, brightness };
    return sendCommand(ComponentPacket::LED_SET_BRIGHTNESS, buf, sizeof buf);
}

CommandResult CoreClient::ledLoadQueue(uint8_t addr, uint8_t flags,
                                          const LedEvent* events, size_t count) {
    if (count > 64) return CommandResult::Nack(ComponentError::QUEUE_TOO_LARGE);
    // 3-byte header + 8 bytes per event.  Stack-allocate an
    // upper-bound buffer rather than malloc.
    uint8_t buf[3 + 64 * 8];
    buf[0] = addr;
    buf[1] = flags;
    buf[2] = (uint8_t)count;
    size_t off = 3;
    for (size_t i = 0; i < count; i++) {
        const LedEvent& e = events[i];
        buf[off + 0] = e.type;
        putU16LE(buf + off + 1, e.p1);
        putU16LE(buf + off + 3, e.p2);
        buf[off + 5] = e.p3;
        buf[off + 6] = e.p4;
        buf[off + 7] = e.p5;
        off += 8;
    }
    return sendCommand(ComponentPacket::LED_QUEUE_LOAD, buf, off);
}

CommandResult CoreClient::ledStartQueue(uint8_t addr) {
    uint8_t buf[1] = { addr };
    return sendCommand(ComponentPacket::LED_QUEUE_START, buf, sizeof buf);
}

CommandResult CoreClient::ledStopQueue(uint8_t addr) {
    uint8_t buf[1] = { addr };
    return sendCommand(ComponentPacket::LED_QUEUE_STOP, buf, sizeof buf);
}

CommandResult CoreClient::ledRestartQueue(uint8_t addr) {
    uint8_t buf[1] = { addr };
    return sendCommand(ComponentPacket::LED_QUEUE_RESTART, buf, sizeof buf);
}

CommandResult CoreClient::ledResetChannel(uint8_t addr) {
    uint8_t buf[1] = { addr };
    return sendCommand(ComponentPacket::LED_RESET_CHANNEL, buf, sizeof buf);
}

CommandResult CoreClient::ledEnableChannel(uint8_t addr, bool enabled) {
    uint8_t buf[2] = { addr, (uint8_t)(enabled ? 1 : 0) };
    return sendCommand(ComponentPacket::LED_ENABLE_CHANNEL, buf, sizeof buf);
}

CommandResult CoreClient::ledSetMasterBrightness(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint8_t buf[1] = { pct };
    return sendCommand(ComponentPacket::LED_SET_MASTER_BRIGHTNESS, buf, sizeof buf);
}

CommandResult CoreClient::ledQuery(uint8_t addr, LedStatus& out) {
    SerialPacket resp;
    uint8_t req[1] = { addr };
    auto cr = sendQuery(ComponentPacket::LED_QUERY, req, sizeof req,
                        ComponentPacket::LED_QUERY_RESP, resp);
    if (!cr.success) return cr;
    if (resp.len < 4) return CommandResult::Nack(SerialError::MISSING_PARAMETER);
    out.addr         = resp.payload[0];
    out.brightness   = resp.payload[1];
    out.queueState   = resp.payload[2];
    out.currentEvent = resp.payload[3];
    return CommandResult::Ack();
}

}  // namespace sfx_core
