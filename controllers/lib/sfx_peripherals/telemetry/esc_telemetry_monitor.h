/*
 * esc_telemetry_monitor.h — passive ESC telemetry monitor → TelemetryHub.
 *
 *   One monitor drains an RX-only UART stream carrying a manufacturer ESC
 *   telemetry broadcast, runs the selected decoder POLICY over it, and
 *   publishes a normalized sensor set into the shared TelemetryHub — from
 *   where it flows to the Jeti radio (via the JetiExpander responder) AND
 *   Studio's Telemetry panel, exactly like a downstream Jeti EX device.
 *
 *   STRUCTURE (mirrors the BatteryPolicy pattern in sfx_peripherals/power):
 *   each manufacturer protocol is its own decoder class in esc/ satisfying
 *   the `EscTelemetryDecoder` concept (esc/esc_decoder.h);
 *   `EscTelemetryMonitorT<TDecoders...>` composes them into one
 *   runtime-selected monitor (std::variant — exactly one decoder alive).
 *   Adding a protocol = write esc/<vendor>_decoder.h + append it to the
 *   `EscTelemetryMonitor` alias; the monitor, role, wire config and UI pick
 *   it up from the pack (no switch statements to extend).
 *
 *   All protocols are UNSOLICITED broadcasts — the ESC transmits push-pull
 *   on its own; the master side is a bare RX pin, no pull-up, no polling
 *   (and CRITICALLY no TX pad attached — see EspInputPort::configureUartRaw).
 *
 *   Fault propagation: a decoder MAY expose a fault table (kFaultMask +
 *   faultMessage) — the monitor publishes a per-device MESSAGE into the
 *   TelemetryHub on every fault-bits CHANGE; the Jeti responder forwards it
 *   to the radio as an EX Message packet (warning/error popup) and Studio
 *   surfaces it on the Telemetry tab.  Decoders without a table fall back
 *   to a generic "ESC FAULT 0x…" warning.
 */

#ifndef SFX_ESC_TELEMETRY_MONITOR_H
#define SFX_ESC_TELEMETRY_MONITOR_H

#include <cstdint>
#include <cstdio>
#include <variant>

#include <platform/sfx_stream.h>
#include <telemetry/telemetry_hub.h>
#include <telemetry/esc/esc_decoder.h>
#include <telemetry/esc/kontronik_decoder.h>
#include <telemetry/esc/scorpion_decoder.h>
#include <telemetry/esc/hobbywing_v4_decoder.h>
#include <telemetry/esc/hobbywing_v5_decoder.h>

namespace sfx_peripherals {

// ============================================================================
// EscTelemetryMonitorT — the composed monitor
// ============================================================================
template <EscTelemetryDecoder... TDecoders>
class EscTelemetryMonitorT {
public:
    /// UART params for a wire protocol id — folded out of the decoder pack
    /// (Rule 33: recovered from the carrier types, no parallel table).
    static bool uartParamsFor(uint8_t protocol, EscUartParams& out) {
        bool found = false;
        (void)(((uint8_t)TDecoders::kProtocol == protocol
                ? (out = EscUartParams{TDecoders::kBaud, TDecoders::kParityEven}, found = true)
                : false) || ...);
        return found;
    }

    /// Display name for a wire protocol id ("ESC" when unknown).
    static const char* protocolName(uint8_t protocol) {
        const char* name = "ESC";
        (void)(((uint8_t)TDecoders::kProtocol == protocol
                ? (name = TDecoders::kName, true) : false) || ...);
        return name;
    }

    /// @return false when no decoder in the pack claims `protocol`.
    bool begin(sfx::Stream* s, uint8_t protocol) {
        _s = s;
        _frames = _errors = _sensorPushes = 0;
        _rxBytes = 0;
        _snapLen = 0;
        _hubDev = 0xFF;
        _lastPushMs = 0;
        _lastFaults = 0;
        _proto = protocol;
        _d = EscTelemData{};
        bool ok = false;
        (void)(((uint8_t)TDecoders::kProtocol == protocol
                ? (_dec.template emplace<TDecoders>(), ok = true) : false) || ...);
        if (!ok) { _dec.template emplace<std::monostate>(); return false; }
        std::visit([](auto& dec) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(dec)>, std::monostate>) dec.reset();
        }, _dec);
        std::snprintf(_d.deviceName, sizeof(_d.deviceName), "%s", protocolName(protocol));
        return true;
    }

    void end() {
        // Expire our hub device promptly so the radio/Studio drop stale rows.
        _s = nullptr;
        _hubDev = 0xFF;
        _dec.template emplace<std::monostate>();
    }

    /// RPM divider ×100 applied at publish (pole pairs × gear ratio) — the
    /// Kontronik stream carries ELECTRICAL rpm; other protocols motor rpm.
    /// 100 = 1.00 (as transmitted).  0 is coerced to 100.
    void setRpmDividerX100(uint16_t x100) { _rpmDivX100 = x100 ? x100 : 100; }
    uint16_t rpmDividerX100() const { return _rpmDivX100; }

    /// Drain + parse.  Cheap: byte-level sync hunt, bounded per pass.
    void update(uint32_t nowMs) {
        if (!_s) return;
        int guard = 256;                     // bound one pass (Rule: no unbounded drains)
        while (guard-- > 0 && _s->available() > 0) {
            const int c = _s->read();
            if (c < 0) break;
            _rxBytes++;
            if (_snapLen < sizeof(_snap)) _snap[_snapLen++] = (uint8_t)c;
            const EscFeed ev = std::visit([&](auto& dec) -> EscFeed {
                if constexpr (std::is_same_v<std::decay_t<decltype(dec)>, std::monostate>) return EscFeed::None;
                else return dec.feed((uint8_t)c, _d);
            }, _dec);
            switch (ev) {
                case EscFeed::Live:  _frames++; publish(nowMs); break;
                case EscFeed::Info:  _frames++; break;
                case EscFeed::Error: _errors++; break;
                case EscFeed::None:  break;
            }
        }
    }

    // Diagnostics
    uint32_t rxBytes() const { return _rxBytes; }
    /// Copy-and-reset the first-bytes snapshot of the current window (bench
    /// diagnostics — lets the instrumentation log show RAW line bytes).
    uint8_t takeSnap(uint8_t* out, uint8_t cap) {
        const uint8_t n = (_snapLen < cap) ? _snapLen : cap;
        for (uint8_t i = 0; i < n; ++i) out[i] = _snap[i];
        _snapLen = 0;
        return n;
    }
    uint32_t frames()  const { return _frames; }
    uint32_t errors()  const { return _errors; }
    uint32_t pushes()  const { return _sensorPushes; }
    const EscTelemData& data() const { return _d; }

private:
    // ── Hub publication (radio + Studio, throttled) ───────────────────
    void publish(uint32_t nowMs) {
        if (nowMs - _lastPushMs < kPushIntervalMs) return;
        _lastPushMs = nowMs;
        _sensorPushes++;

        auto& hub = sfx_telemetry::TelemetryHub::instance();
        sfx_telemetry::TelemetryHub::ScopedLock lk(hub);
        using sfx_telemetry::SensorKind;
        // Synthetic identity: usn tags the ESC family, lsn the protocol —
        // unique against real Jeti devices on the same radio.  Re-upsert every
        // push: refreshes the device's lastMs AND its NAME, which upgrades
        // from the protocol default to the real identity once the info frame
        // decodes (Kontronik KODI → "KOLIBRI").
        const bool first = (_hubDev == 0xFF);
        _hubDev = hub.upsertDevice((uint16_t)(0xE5C0u | (uint16_t)_proto),
                                   0x0001, _d.deviceName, /*local=*/false, nowMs);
        if (_hubDev == 0xFF) return;
        if (first) {
            hub.setSensor(_hubDev, 1, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 1, "RPM", "rpm");
            hub.setSensor(_hubDev, 2, SensorKind::Int, 2, 0, nowMs); hub.setLabel(_hubDev, 2, "Voltage", "V");
            hub.setSensor(_hubDev, 3, SensorKind::Int, 1, 0, nowMs); hub.setLabel(_hubDev, 3, "Current", "A");
            hub.setSensor(_hubDev, 4, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 4, "Used", "mAh");
            hub.setSensor(_hubDev, 5, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 5, "Throttle", "%");
            hub.setSensor(_hubDev, 6, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 6, "Temp ESC", "\xB0""C");
            hub.setSensor(_hubDev, 7, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 7, "Temp BEC", "\xB0""C");
            hub.setSensor(_hubDev, 8, SensorKind::Int, 1, 0, nowMs); hub.setLabel(_hubDev, 8, "BEC U", "V");
            hub.setSensor(_hubDev, 9, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 9, "Faults", "");
        }
        // RPM divider: transmitted (electrical/motor) rpm → shaft/head rpm.
        const uint32_t rpmOut = (uint32_t)(((uint64_t)_d.rpm * 100u) / _rpmDivX100);

        // Fault reporting: mask decoder-declared benign bits, then publish
        // the masked value as the Faults sensor AND — on CHANGE — a device
        // MESSAGE (text + class) the Jeti responder forwards to the radio.
        uint32_t masked = _d.faults;
        char     msg[24] = {};
        uint8_t  cls = 0;
        std::visit([&](auto& dec) {
            using D = std::decay_t<decltype(dec)>;
            if constexpr (!std::is_same_v<D, std::monostate>) {
                if constexpr (requires { D::kFaultMask; }) masked &= D::kFaultMask;
                if (masked) {
                    if constexpr (requires { D::faultMessage(0u, msg, sizeof msg); }) {
                        cls = D::faultMessage(masked, msg, sizeof msg);
                    } else {
                        std::snprintf(msg, sizeof msg, "ESC FAULT %04lX", (unsigned long)masked);
                        cls = 2;   // generic warning
                    }
                }
            }
        }, _dec);
        if (masked != _lastFaults) {
            _lastFaults = masked;
            hub.setMessage(_hubDev, cls, masked ? msg : "", nowMs);
        }

        hub.setSensor(_hubDev, 1, SensorKind::Int, 0, (int32_t)rpmOut, nowMs);
        hub.setSensor(_hubDev, 2, SensorKind::Int, 2, (int32_t)(_d.voltage_mV / 10), nowMs);
        hub.setSensor(_hubDev, 3, SensorKind::Int, 1, _d.current_cA / 10, nowMs);
        hub.setSensor(_hubDev, 4, SensorKind::Int, 0, (int32_t)_d.capacity_mAh, nowMs);
        hub.setSensor(_hubDev, 5, SensorKind::Int, 0, _d.throttlePct, nowMs);
        hub.setSensor(_hubDev, 6, SensorKind::Int, 0, _d.tempEsc_C, nowMs);
        hub.setSensor(_hubDev, 7, SensorKind::Int, 0, _d.tempBec_C, nowMs);
        hub.setSensor(_hubDev, 8, SensorKind::Int, 1, (int32_t)(_d.becVoltage_mV / 100), nowMs);
        hub.setSensor(_hubDev, 9, SensorKind::Int, 0, (int32_t)masked, nowMs);
    }

    static constexpr uint32_t kPushIntervalMs = 500;   // hub/radio refresh (2 Hz)

    sfx::Stream* _s = nullptr;
    uint8_t      _proto = 0xFF;
    std::variant<std::monostate, TDecoders...> _dec;
    uint32_t     _frames = 0, _errors = 0, _sensorPushes = 0;
    uint32_t     _rxBytes = 0;
    uint8_t      _snap[16];
    uint8_t      _snapLen = 0;
    uint32_t     _lastPushMs = 0;
    uint32_t     _lastFaults = 0;
    uint16_t     _rpmDivX100 = 100;
    uint8_t      _hubDev = 0xFF;
    EscTelemData _d;
};

/// The shipped protocol set — extend by appending a decoder type.
using EscTelemetryMonitor = EscTelemetryMonitorT<KontronikDecoder,
                                                 ScorpionDecoder,
                                                 HobbywingV4Decoder,
                                                 HobbywingV5Decoder>;

}  // namespace sfx_peripherals

#endif  // SFX_ESC_TELEMETRY_MONITOR_H
