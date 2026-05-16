/*
 * SlaveClient — implementation of the typed master-side client.
 *
 * Pattern: each method builds the wire payload, calls into BusClient
 * via `sendCommand` (instant ACK/NACK) or `sendQuery` (typed response
 * decoded by the caller).  Async packets are routed in
 * onModulePacket() to the registered observer fanout.
 *
 * Endianness: all multi-byte integers are little-endian on the wire
 * per CLAUDE.md Rule 4.
 */

#include "slave_client.h"

#include <cstring>

namespace sfx_slave {

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

// ── Module-packet routing (async + query response handling) ──────────

bool SlaveClient::onModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
    // Async events first — slave fires these unsolicited with TAG_ASYNC.
    switch (type) {
        case SlavePacket::SERVO_TARGET_REACHED:   decodeServoTargetReached(payload, len); return true;
        case SlavePacket::SERVO_MOTION_UPDATE:    decodeServoMotionUpdate (payload, len); return true;
        case SlavePacket::PWM_STALL:              decodePwmStall          (payload, len); return true;
        case SlavePacket::LED_PROGRAM_DONE:       decodeLedProgramDone    (payload, len); return true;
        case SlavePacket::BATTERY_ALERT:          decodeBatteryAlert      (payload, len); return true;
        case SlavePacket::SLAVE_STATUS_BROADCAST: decodeStatusBroadcast   (payload, len); return true;
    }
    // Query responses fall through to BusClient's tag-correlation path.
    return BusClient::onModulePacket(type, payload, len);
}

void SlaveClient::decodeServoTargetReached(const uint8_t* p, size_t len) {
    if (len < 3) return;
    const uint8_t  idx = p[0];
    const uint16_t pos = getU16LE(p + 1);
    for (auto& cb : _onTargetReached) cb(idx, pos);
}

void SlaveClient::decodeServoMotionUpdate(const uint8_t* p, size_t len) {
    if (len < 7) return;
    const uint8_t  idx    = p[0];
    const uint16_t pos    = getU16LE(p + 1);
    const uint16_t target = getU16LE(p + 3);
    const int16_t  vel    = getI16LE(p + 5);
    for (auto& cb : _onMotionUpdate) cb(idx, pos, target, vel);
}

void SlaveClient::decodePwmStall(const uint8_t* p, size_t len) {
    if (len < 5) return;
    const uint8_t  idx     = p[0];
    const uint16_t peak_mA = getU16LE(p + 1);
    const uint16_t dur_ms  = getU16LE(p + 3);
    for (auto& cb : _onPwmStall) cb(idx, peak_mA, dur_ms);
}

void SlaveClient::decodeLedProgramDone(const uint8_t* p, size_t len) {
    if (len < 2) return;
    for (auto& cb : _onLedProgramDone) cb(p[0], p[1]);
}

void SlaveClient::decodeBatteryAlert(const uint8_t* p, size_t len) {
    if (len < 4) return;
    BatteryAlert a{};
    a.level      = p[0];
    a.voltage_mV = getU16LE(p + 1);
    a.cellCount  = p[3];
    for (auto& cb : _onBatteryAlert) cb(a);
}

void SlaveClient::decodeBatteryInfoPayload(const uint8_t* p, size_t len, BatteryInfo& out) {
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

void SlaveClient::decodeBatterySection(const uint8_t* p, size_t len,
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

void SlaveClient::decodeStatusBroadcast(const uint8_t* p, size_t len) {
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
            l.addr       = p[off + 0];
            l.brightness = p[off + 1];
            l.progState  = p[off + 2];
            l.progId     = p[off + 3];
            st.leds.push_back(l);
            off += 4;
        }
    }

    // Battery — variable: [present:u8] + (if present) 8 more bytes.
    decodeBatterySection(p, len, off, st.battery);

    for (auto& cb : _onStatusBroadcast) cb(st);
}

// ── Identity / enumeration ───────────────────────────────────────────

CommandResult SlaveClient::requestComponentList(
        std::vector<sfx_peripherals::ComponentInfo>& out) {
    SerialPacket resp;
    auto cr = sendQuery(SlavePacket::COMPONENT_LIST_REQ, nullptr, 0,
                        SlavePacket::COMPONENT_LIST_RESP, resp);
    if (!cr.ok()) return cr;
    if (resp.len < 1) return CommandResult::Error(SerialError::INVALID_PAYLOAD_LENGTH);

    const uint8_t count = resp.payload[0];
    if (resp.len < (size_t)(1 + count * sizeof(sfx_peripherals::ComponentInfo))) {
        return CommandResult::Error(SerialError::INVALID_PAYLOAD_LENGTH);
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

CommandResult SlaveClient::getIdentifier(uint8_t& out_boardType,
                                         char* out_name, size_t bufLen) {
    SerialPacket resp;
    auto cr = sendQuery(SlavePacket::IDENT_GET_REQ, nullptr, 0,
                        SlavePacket::IDENT_GET_RESP, resp);
    if (!cr.ok()) return cr;
    if (resp.len < 2) return CommandResult::Error(SerialError::INVALID_PAYLOAD_LENGTH);

    out_boardType  = resp.payload[0];
    const uint8_t l = resp.payload[1];
    if (resp.len < (size_t)(2 + l)) return CommandResult::Error(SerialError::INVALID_PAYLOAD_LENGTH);
    if (out_name && bufLen > 0) {
        const size_t copy = (l + 1u <= bufLen) ? l : (bufLen - 1);
        memcpy(out_name, resp.payload + 2, copy);
        out_name[copy] = 0;
    }
    return CommandResult::Ack();
}

CommandResult SlaveClient::setIdentifier(const char* name) {
    if (!name) return CommandResult::Error(SerialError::INVALID_PAYLOAD);
    const size_t l = strlen(name);
    if (l > 32) return CommandResult::Error(SlaveError::IDENT_TOO_LONG);
    uint8_t buf[33];
    buf[0] = (uint8_t)l;
    memcpy(buf + 1, name, l);
    return sendCommand(SlavePacket::IDENT_SET, buf, 1 + l);
}

CommandResult SlaveClient::requestStatus(SlaveStatus& out, uint8_t kindsMask) {
    SerialPacket resp;
    uint8_t req[1] = { kindsMask };
    auto cr = sendQuery(SlavePacket::SLAVE_STATUS_REQ, req, sizeof req,
                        SlavePacket::SLAVE_STATUS_BROADCAST, resp);
    if (!cr.ok()) return cr;
    if (resp.len < 10) return CommandResult::Error(SerialError::INVALID_PAYLOAD_LENGTH);

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
            ll.addr       = p[off + 0];
            ll.brightness = p[off + 1];
            ll.progState  = p[off + 2];
            ll.progId     = p[off + 3];
            out.leds.push_back(ll);
            off += 4;
        }
    }
    decodeBatterySection(p, len, off, out.battery);
    return CommandResult::Ack();
}

CommandResult SlaveClient::setStatusRate(uint8_t hz, uint8_t kindsMask) {
    uint8_t buf[2] = { hz, kindsMask };
    return sendCommand(SlavePacket::SLAVE_STATUS_RATE, buf, sizeof buf);
}

// ── Battery ──────────────────────────────────────────────────────────

CommandResult SlaveClient::requestBatteryInfo(BatteryInfo& out) {
    SerialPacket resp;
    auto cr = sendQuery(SlavePacket::BATTERY_INFO_REQ, nullptr, 0,
                        SlavePacket::BATTERY_INFO_RESP, resp);
    if (!cr.ok()) return cr;
    decodeBatteryInfoPayload(resp.payload, resp.len, out);
    return CommandResult::Ack();
}

CommandResult SlaveClient::batteryReconfigure(BatteryChemistry chemistry,
                                              uint8_t          cellCount,
                                              uint16_t         customLow_mV,
                                              uint16_t         customCritical_mV) {
    uint8_t buf[6];
    buf[0] = (uint8_t)chemistry;
    buf[1] = cellCount;
    putU16LE(buf + 2, customLow_mV);
    putU16LE(buf + 4, customCritical_mV);
    return sendCommand(SlavePacket::BATTERY_RECONFIGURE, buf, sizeof buf);
}

// ── Servo ────────────────────────────────────────────────────────────

CommandResult SlaveClient::servoSet(uint8_t idx, uint16_t pulse_us) {
    uint8_t buf[3];
    buf[0] = idx;
    putU16LE(buf + 1, pulse_us);
    return sendCommand(SlavePacket::SERVO_SET, buf, sizeof buf);
}

CommandResult SlaveClient::servoConfig(uint8_t idx, const ServoCalibration& cal) {
    uint8_t buf[13];
    buf[0] = idx;
    putU16LE(buf + 1,  cal.min_us);
    putU16LE(buf + 3,  cal.max_us);
    putU16LE(buf + 5,  cal.center_us);
    putU16LE(buf + 7,  cal.maxSpeed);
    putU16LE(buf + 9,  cal.accel);
    putU16LE(buf + 11, cal.decel);
    return sendCommand(SlavePacket::SERVO_CONFIG, buf, sizeof buf);
}

CommandResult SlaveClient::servoSetMotion(uint8_t idx, uint16_t maxSpeed,
                                          uint16_t accel, uint16_t decel) {
    uint8_t buf[7];
    buf[0] = idx;
    putU16LE(buf + 1, maxSpeed);
    putU16LE(buf + 3, accel);
    putU16LE(buf + 5, decel);
    return sendCommand(SlavePacket::SERVO_SET_MOTION, buf, sizeof buf);
}

CommandResult SlaveClient::servoApplyJerk(uint8_t idx, int16_t offset_us, uint16_t duration_ms) {
    uint8_t buf[5];
    buf[0] = idx;
    putU16LE(buf + 1, (uint16_t)offset_us);
    putU16LE(buf + 3, duration_ms);
    return sendCommand(SlavePacket::SERVO_APPLY_JERK, buf, sizeof buf);
}

CommandResult SlaveClient::servoHold(uint8_t idx, bool hold) {
    uint8_t buf[2] = { idx, (uint8_t)(hold ? 1 : 0) };
    return sendCommand(SlavePacket::SERVO_HOLD, buf, sizeof buf);
}

CommandResult SlaveClient::servoQuery(uint8_t idx, ServoStatus& out) {
    SerialPacket resp;
    uint8_t req[1] = { idx };
    auto cr = sendQuery(SlavePacket::SERVO_QUERY, req, sizeof req,
                        SlavePacket::SERVO_QUERY_RESP, resp);
    if (!cr.ok()) return cr;
    if (resp.len < 8) return CommandResult::Error(SerialError::INVALID_PAYLOAD_LENGTH);
    out.idx               = resp.payload[0];
    out.pos_us            = getU16LE(resp.payload + 1);
    out.target_us         = getU16LE(resp.payload + 3);
    out.velocity_us_per_s = getI16LE(resp.payload + 5);
    out.flags             = resp.payload[7];
    return CommandResult::Ack();
}

CommandResult SlaveClient::servoMotionUpdates(bool enable, uint8_t rate_hz) {
    uint8_t buf[2] = { (uint8_t)(enable ? 1 : 0), rate_hz };
    return sendCommand(SlavePacket::SERVO_MOTION_UPDATES, buf, sizeof buf);
}

// ── PWM ──────────────────────────────────────────────────────────────

CommandResult SlaveClient::pwmSetMode(uint8_t idx, sfx_peripherals::ComponentKind mode) {
    uint8_t buf[2] = { idx, (uint8_t)mode };
    return sendCommand(SlavePacket::PWM_SET_MODE, buf, sizeof buf);
}

CommandResult SlaveClient::pwmSetDuty(uint8_t idx, uint16_t duty_thousandths) {
    uint8_t buf[3];
    buf[0] = idx;
    putU16LE(buf + 1, duty_thousandths);
    return sendCommand(SlavePacket::PWM_SET_DUTY, buf, sizeof buf);
}

CommandResult SlaveClient::pwmSetMotor(uint8_t idx, int16_t speed_signed) {
    uint8_t buf[3];
    buf[0] = idx;
    putU16LE(buf + 1, (uint16_t)speed_signed);
    return sendCommand(SlavePacket::PWM_SET_MOTOR, buf, sizeof buf);
}

CommandResult SlaveClient::pwmSetHeater(uint8_t idx, uint16_t value) {
    uint8_t buf[3];
    buf[0] = idx;
    putU16LE(buf + 1, value);
    return sendCommand(SlavePacket::PWM_SET_HEATER, buf, sizeof buf);
}

CommandResult SlaveClient::pwmSetFrequency(uint8_t idx, uint16_t freq_Hz) {
    uint8_t buf[3];
    buf[0] = idx;
    putU16LE(buf + 1, freq_Hz);
    return sendCommand(SlavePacket::PWM_SET_FREQ, buf, sizeof buf);
}

CommandResult SlaveClient::pwmReconfigure(uint8_t idx, const PwmRuntimeConfig& cfg) {
    uint8_t buf[7];
    buf[0] = idx;
    buf[1] = (uint8_t)cfg.mode;
    putU16LE(buf + 2, cfg.freq_Hz);
    buf[4] = cfg.cfgFlags;
    putU16LE(buf + 5, cfg.maxDuty);
    return sendCommand(SlavePacket::PWM_RECONFIGURE, buf, sizeof buf);
}

CommandResult SlaveClient::pwmQuery(uint8_t idx, PwmStatus& out) {
    SerialPacket resp;
    uint8_t req[1] = { idx };
    auto cr = sendQuery(SlavePacket::PWM_QUERY, req, sizeof req,
                        SlavePacket::PWM_QUERY_RESP, resp);
    if (!cr.ok()) return cr;
    if (resp.len < 14) return CommandResult::Error(SerialError::INVALID_PAYLOAD_LENGTH);
    out.idx              = resp.payload[0];
    out.mode             = (sfx_peripherals::ComponentKind)resp.payload[1];
    out.duty_thousandths = getU16LE(resp.payload + 2);
    out.freq_Hz          = getU16LE(resp.payload + 4);
    out.voltage_mV       = getI32LE(resp.payload + 6);
    out.current_mA       = getI32LE(resp.payload + 10);
    return CommandResult::Ack();
}

CommandResult SlaveClient::pwmGetConfig(uint8_t idx, PwmConfig& out) {
    SerialPacket resp;
    uint8_t req[1] = { idx };
    auto cr = sendQuery(SlavePacket::PWM_GET_CONFIG, req, sizeof req,
                        SlavePacket::PWM_GET_CONFIG_RESP, resp);
    if (!cr.ok()) return cr;
    if (resp.len < 10) return CommandResult::Error(SerialError::INVALID_PAYLOAD_LENGTH);
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

CommandResult SlaveClient::pwmSetStallGuard(uint8_t idx, uint16_t threshold_mA,
                                            uint8_t debounce_ms, uint8_t flags) {
    uint8_t buf[5];
    buf[0] = idx;
    putU16LE(buf + 1, threshold_mA);
    buf[3] = debounce_ms;
    buf[4] = flags;
    return sendCommand(SlavePacket::PWM_SET_STALL_GUARD, buf, sizeof buf);
}

CommandResult SlaveClient::pwmClearStall(uint8_t idx) {
    uint8_t buf[1] = { idx };
    return sendCommand(SlavePacket::PWM_CLEAR_STALL, buf, sizeof buf);
}

// ── LED ──────────────────────────────────────────────────────────────

CommandResult SlaveClient::ledSetBrightness(uint8_t addr, uint8_t brightness) {
    uint8_t buf[2] = { addr, brightness };
    return sendCommand(SlavePacket::LED_SET_BRIGHTNESS, buf, sizeof buf);
}

CommandResult SlaveClient::ledLoadProgram(uint8_t addr, uint8_t progId,
                                          const LedEvent* events, size_t count) {
    if (count > 64) return CommandResult::Error(SlaveError::PROGRAM_TOO_LARGE);
    // 3-byte header + 8 bytes per event.  Stack-allocate an
    // upper-bound buffer rather than malloc.
    uint8_t buf[3 + 64 * 8];
    buf[0] = addr;
    buf[1] = progId;
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
    return sendCommand(SlavePacket::LED_PROGRAM_LOAD, buf, off);
}

CommandResult SlaveClient::ledRunProgram(uint8_t addr, uint8_t progId, uint8_t flags) {
    uint8_t buf[3] = { addr, progId, flags };
    return sendCommand(SlavePacket::LED_PROGRAM_RUN, buf, sizeof buf);
}

CommandResult SlaveClient::ledStopProgram(uint8_t addr) {
    uint8_t buf[1] = { addr };
    return sendCommand(SlavePacket::LED_PROGRAM_STOP, buf, sizeof buf);
}

CommandResult SlaveClient::ledRestartProgram(uint8_t addr) {
    uint8_t buf[1] = { addr };
    return sendCommand(SlavePacket::LED_PROGRAM_RESTART, buf, sizeof buf);
}

CommandResult SlaveClient::ledResetChannel(uint8_t addr) {
    uint8_t buf[1] = { addr };
    return sendCommand(SlavePacket::LED_RESET_CHANNEL, buf, sizeof buf);
}

CommandResult SlaveClient::ledEnableChannel(uint8_t addr, bool enabled) {
    uint8_t buf[2] = { addr, (uint8_t)(enabled ? 1 : 0) };
    return sendCommand(SlavePacket::LED_ENABLE_CHANNEL, buf, sizeof buf);
}

CommandResult SlaveClient::ledSetMasterBrightness(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint8_t buf[1] = { pct };
    return sendCommand(SlavePacket::LED_SET_MASTER_BRIGHTNESS, buf, sizeof buf);
}

CommandResult SlaveClient::ledQuery(uint8_t addr, LedStatus& out) {
    SerialPacket resp;
    uint8_t req[1] = { addr };
    auto cr = sendQuery(SlavePacket::LED_QUERY, req, sizeof req,
                        SlavePacket::LED_QUERY_RESP, resp);
    if (!cr.ok()) return cr;
    if (resp.len < 4) return CommandResult::Error(SerialError::INVALID_PAYLOAD_LENGTH);
    out.addr       = resp.payload[0];
    out.brightness = resp.payload[1];
    out.progId     = resp.payload[2];
    out.progState  = resp.payload[3];
    return CommandResult::Ack();
}

}  // namespace sfx_slave
