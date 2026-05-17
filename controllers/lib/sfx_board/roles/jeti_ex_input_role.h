/*
 * JetiExInputRole — Jeti Duplex EX Bus on an `InputPort`.
 *
 * On `bind()` the role switches the port into JETI_EX mode
 * (`InputPort::configureJetiEx(baud)` — 8N1, half-duplex; baud is
 * 125000 by default, optionally 250000), then attaches a `JetiExBus`
 * peripheral to the port's UART `Stream*`.
 *
 * The role exposes:
 *   - 16 proportional channels (read via `channel_us()` like SBUS)
 *   - Sensor-value registration (`addSensorValue`, `setSensorValue`)
 *   - Config-parameter registration with change callback
 *   - Alarm queueing
 *
 * Telemetry / config responses are auto-driven from `tick()` — the
 * caller doesn't have to think about half-duplex turnaround; the ESP32
 * UART (configured by `EspInputPort::configureJetiEx()` via
 * `UART_MODE_RS485_HALF_DUPLEX`) flips driver direction automatically.
 *
 * Role is ESP32-only (matches `JetiExBus` build gating).
 */

#ifndef SFX_JETI_EX_INPUT_ROLE_H
#define SFX_JETI_EX_INPUT_ROLE_H

#include <Arduino.h>
#include <cstdint>
#include <functional>

#include <platform/sfx_platform.h>
#include <ports/input_port.h>

#if SFX_PLATFORM_ESP32
#  include <jeti_ex/jeti_ex_bus.h>
#  include <jeti_ex/jeti_ex_common.h>
#endif

namespace sfx_core {

class JetiExInputRole {
public:
    /// Async broadcast callback: `(channel_count, valid, rxFrames, rxErrors)`.
    using BroadcastCallback = std::function<void(uint8_t channels,
                                                 bool valid,
                                                 uint32_t rxFrames,
                                                 uint32_t rxErrors)>;

#if SFX_PLATFORM_ESP32
    using ParamChangeCallback = JetiExBus::ParamChangeCallback;
#else
    using ParamChangeCallback = std::function<void(uint8_t, int32_t, int32_t)>;
#endif

    JetiExInputRole() = default;
    explicit JetiExInputRole(sfx_peripherals::InputPort* port) { bind(port); }

    /// Switches port to JETI_EX mode at `baud` (125000 or 250000), then
    /// attaches the decoder.  Returns false on capability mismatch or
    /// peripheral configure failure.
    bool bind(sfx_peripherals::InputPort* port, uint32_t baud = 125000);

    /// Channel value (1-based 1..N) in µs.  Returns 1500 (centre) when
    /// no frame received yet.
    uint16_t channel_us(uint8_t ch1based) const;
    uint8_t  channelCount() const;
    bool     valid()        const;

    uint32_t rxFrameCount()    const;
    uint32_t rxErrorCount()    const;
    uint32_t txResponseCount() const;

    // ── Telemetry sensor registration ────────────────────────────────
    void setSensorInfo(uint16_t manufacturerId, uint16_t deviceId, const char* name);

#if SFX_PLATFORM_ESP32
    bool addSensorValue(uint8_t id, const char* label, const char* unit,
                        JetiEx::ExDataType type, uint8_t decimals = 0);
#endif

    void setSensorValue(uint8_t id, int32_t value);
    uint8_t sensorValueCount() const;

    // ── Config parameter registration ────────────────────────────────
#if SFX_PLATFORM_ESP32
    bool addParam(uint8_t id, const char* label, JetiEx::ParamType type,
                  int32_t minVal, int32_t maxVal, int32_t defaultVal);
#endif

    void    setParamValue(uint8_t id, int32_t value);
    int32_t getParamValue(uint8_t id) const;
    uint8_t paramCount() const;

    void onParamChange(ParamChangeCallback cb);

    // ── Alarm ────────────────────────────────────────────────────────
    void setAlarm(char alarmChar, const char* message);
    void clearAlarm();
    bool alarmActive() const;

    // ── Broadcast / tick ─────────────────────────────────────────────
    void setBroadcastHz(uint8_t hz);
    void onBroadcast(BroadcastCallback cb) { _onBroadcast = std::move(cb); }

    /// Tick — drives decoder I/O (RX parse + auto-respond) and the
    /// optional broadcast timer.
    void tick();

private:
    sfx_peripherals::InputPort* _port = nullptr;
#if SFX_PLATFORM_ESP32
    JetiExBus _decoder;
#endif

    uint8_t  _broadcastHz          = 0;
    uint32_t _broadcastInterval_ms = 0;
    uint32_t _lastBroadcastMs      = 0;

    BroadcastCallback _onBroadcast;
};

}  // namespace sfx_core

#endif  // SFX_JETI_EX_INPUT_ROLE_H
