/*
 * jeti_expander.h — JetiExpander
 *
 * Jeti EX Bus telemetry EXPANDER (the spec's expander topology, page 1).  A
 * board-unique singleton (Rule 14) that bridges two half-duplex Jeti links and
 * runs on a dedicated Core-0 FreeRTOS task (above the Arduino loopTask) so the
 * time-critical ~4 ms response slots are never blocked by the soft main loop —
 * and never touch the hard-real-time audio on Core 1.
 *
 *   IN_1 (Rx side, we are SLAVE)  : answer the receiver's telemetry polls with
 *                                   the MERGED multi-device set from the hub
 *                                   (half-duplex TX), and decode RC channels.
 *   IN_2 (ESC side, we are MASTER): mirror the receiver's master frames out to
 *                                   the downstream device ("copy the input
 *                                   signal") so it replies; decode its EX
 *                                   telemetry into the hub PASS-THROUGH with
 *                                   its own USN/LSN/name.
 *
 * The JetiTelemetryHub is the shared merge point.  Devices keep their identity,
 * so the radio shows the downstream device under its own name next to "HubFx".
 *
 * ESP32-only (FreeRTOS task + half-duplex GPIO-matrix TX).
 */

#ifndef SFX_JETI_EXPANDER_H
#define SFX_JETI_EXPANDER_H

#include <platform/sfx_platform.h>
#if SFX_PLATFORM_ESP32

#include <Arduino.h>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ports/input_port.h>
#include <serial/diag_log.h>
#include "jeti_ex_bus.h"
#include "jeti_ex_telemetry_monitor.h"
#include "jeti_telemetry_hub.h"

namespace JetiEx {

class JetiExpander {
public:
    static JetiExpander& instance() {
        static JetiExpander inst;            // C++11 thread-safe static local
        return inst;
    }

    /// Start the expander.  `rxPort` is the Rx-facing link (IN_1), `escPort`
    /// the downstream link (IN_2).  Registers the HubFX-own device (usn/lsn/
    /// name) as a local hub device, wires the responder + forward hooks, and
    /// spawns the Core-0 task.  Idempotent.
    bool begin(sfx_peripherals::InputPort* rxPort,
               sfx_peripherals::InputPort* escPort,
               uint16_t hubUsn, uint16_t hubLsn, const char* hubName,
               uint32_t baud = 125000) {
        if (_task) return true;                       // already running
        if (!rxPort) return false;
        _rxPort  = rxPort;
        if (!rxPort->configureJetiEx(baud)) return false;
        Stream* rxStream = rxPort->uartStream();
        if (!rxStream) return false;

        // Downstream (ESC) link is optional — without it we still serve the
        // HubFX-own device, just with nothing to forward/merge.
        _escPort = escPort;
        if (_escPort && _escPort->configureJetiEx(baud)) {
            _escStream = _escPort->uartStream();
            if (_escStream) _escMon.begin(_escStream);
            else            _escPort = nullptr;
        } else {
            _escPort = nullptr;
        }

        if (!_rxBus.begin(rxStream)) return false;
        _rxBus.setTxPort(rxPort);                     // half-duplex TX on IN_1
        _rxBus.onTelemetryRequest([this](uint8_t pkt){ serveTelemetry(pkt); });
        _rxBus.onRawFrame([this](const uint8_t* f, uint8_t l){ forwardToEsc(f, l); });

        // Register the HubFX-own device (local → never expires) + its built-in
        // sensors, so it shows on the radio even with no downstream ESC.
        {
            auto& hub = JetiTelemetryHub::instance();
            JetiTelemetryHub::ScopedLock lk(hub);
            _localDev = hub.upsertDevice(hubUsn, hubLsn, hubName, /*local=*/true, 0);
            if (_localDev != 0xFF) {
                hub.setSensor(_localDev, kUptimeId,  ExDataType::Int22, 0, 0, 0);
                hub.setLabel (_localDev, kUptimeId,  "Uptime", "s");
                hub.setSensor(_localDev, kFreeRamId, ExDataType::Int22, 0, 0, 0);
                hub.setLabel (_localDev, kFreeRamId, "FreeRAM", "kB");
            }
        }

        _stop = false;
        const BaseType_t ok = xTaskCreatePinnedToCore(
            &JetiExpander::taskEntry, "jetiExp", 4096, this,
            /*priority=*/3,             // above loopTask (1)
            &_task, /*core=*/0);        // Core 0 — audio owns Core 1
        if (ok != pdPASS) { _task = nullptr; return false; }
        SFX_LOG_INFO("[jexp] started on Core 0 (rx=IN_1, esc=IN_2, baud=%lu)",
                     (unsigned long)baud);
        return true;
    }

    void end() {
        if (_task) {
            _stop = true;
            // Let the loop observe _stop and self-delete; then forget it.
            for (int i = 0; i < 50 && _task; ++i) SFX_DELAY_MS(2);
            _task = nullptr;
        }
        _rxBus.end();
        _escMon.end();
        if (_rxPort)  _rxPort->disable();
        if (_escPort) _escPort->disable();
        _rxPort = _escPort = nullptr;
        _escStream = nullptr;
    }

    bool running() const { return _task != nullptr; }

    // ── HubFX-own telemetry (the local device) ───────────────────────
    void setLocalSensor(uint8_t id, const char* label, const char* unit,
                        ExDataType type, uint8_t decimals, int32_t value, uint32_t nowMs) {
        if (_localDev == 0xFF) return;
        auto& hub = JetiTelemetryHub::instance();
        JetiTelemetryHub::ScopedLock lk(hub);
        hub.setSensor(_localDev, id, type, decimals, value, nowMs);
        if (label) hub.setLabel(_localDev, id, label, unit);
    }
    void setLocalValue(uint8_t id, int32_t value, uint32_t nowMs) {
        if (_localDev == 0xFF) return;
        auto& hub = JetiTelemetryHub::instance();
        JetiTelemetryHub::ScopedLock lk(hub);
        // Preserve type/decimals — re-set via the existing sensor entry.
        const auto* d = hub.device(_localDev);
        if (!d) return;
        for (uint8_t i = 0; i < d->sensorCount; ++i)
            if (d->sensors[i].id == id) {
                hub.setSensor(_localDev, id, d->sensors[i].type,
                              d->sensors[i].decimals, value, nowMs);
                return;
            }
    }

    // ── Channels (read by effects via the input dispatcher) ──────────
    uint16_t channel_us(uint8_t ch1based) const { return _rxBus.channel_us(ch1based); }
    uint8_t  channelCount() const { return _rxBus.channelCount(); }
    bool     valid()        const { return _rxBus.isValid(); }

    // ── Diagnostics ──────────────────────────────────────────────────
    uint32_t rxBytes()   const { return _rxBus.rxByteCount(); }
    uint32_t rxFrames()  const { return _rxBus.rxFrameCount(); }
    uint32_t rxErrors()  const { return _rxBus.rxErrorCount(); }
    uint32_t txResp()    const { return _rxBus.txResponseCount(); }
    uint32_t escBytes()  const { return _escMon.rxByteCount(); }
    uint32_t escFrames() const { return _escMon.telemetryFrames(); }
    uint32_t escErrors() const { return _escMon.errorCount(); }
    uint8_t  deviceCount() const { return JetiTelemetryHub::instance().deviceCount(); }
    uint8_t  sensorCount() const { return JetiTelemetryHub::instance().activeSensorCount(); }

private:
    JetiExpander() = default;
    JetiExpander(const JetiExpander&) = delete;
    JetiExpander& operator=(const JetiExpander&) = delete;

    static void taskEntry(void* arg) { static_cast<JetiExpander*>(arg)->taskLoop(); }

    void taskLoop() {
        uint32_t lastExpire = 0, lastLog = 0, lastBuiltin = 0;
        for (;;) {
            if (_stop) break;
            const uint32_t now = millis();
            // IN_2 first: drain the ESC's telemetry into the hub before IN_1
            // forwards (and harmlessly re-parses) the next echoed master frame.
            _escMon.update(now);
            // IN_1: parse master frames → respond (hook) + forward (hook).
            _rxBus.update();

            if (now - lastBuiltin >= 1000 && _localDev != 0xFF) {
                lastBuiltin = now;
                auto& hub = JetiTelemetryHub::instance();
                JetiTelemetryHub::ScopedLock lk(hub);
                hub.setSensor(_localDev, kUptimeId,  ExDataType::Int22, 0,
                              (int32_t)(now / 1000), now);
                hub.setSensor(_localDev, kFreeRamId, ExDataType::Int22, 0,
                              (int32_t)(ESP.getFreeHeap() / 1024), now);
            }
            if (now - lastExpire >= 1000) {
                lastExpire = now;
                auto& hub = JetiTelemetryHub::instance();
                JetiTelemetryHub::ScopedLock lk(hub);
                hub.expireStale(now, 2000);   // disconnected ESC drops out
            }
#if SFX_INSTRUMENTATION
            if (now - lastLog >= 1000) {
                lastLog = now;
                SFX_LOG_INFO("[jexp] rxF=%lu rxE=%lu tx=%lu | escF=%lu escE=%lu | dev=%u",
                             (unsigned long)_rxBus.rxFrameCount(),
                             (unsigned long)_rxBus.rxErrorCount(),
                             (unsigned long)_rxBus.txResponseCount(),
                             (unsigned long)_escMon.telemetryFrames(),
                             (unsigned long)_escMon.errorCount(),
                             (unsigned)JetiTelemetryHub::instance().deviceCount());
            }
#else
            (void)lastLog;
#endif
            vTaskDelay(1);                     // ~1 ms; yields Core 0 to the loop
        }
        _task = nullptr;
        vTaskDelete(nullptr);
    }

    // Telemetry hook (runs in-task during _rxBus.update()): build the next
    // multi-device frame from the hub (under lock), then half-duplex TX it.
    void serveTelemetry(uint8_t pktId) {
        auto& hub = JetiTelemetryHub::instance();
        uint8_t buf[40];
        uint8_t len = 0;
        {
            JetiTelemetryHub::ScopedLock lk(hub);
            _seq++;
            if ((_seq % 5) == 0) len = buildText(hub, buf);     // names + labels
            else                 len = buildData(hub, buf);     // values
            if (!len)            len = buildText(hub, buf);      // fallback
        }
        if (len) _rxBus.sendTelemetry(pktId, buf, len);
    }

    // Raw-frame hook: mirror an Rx master frame to the ESC (half-duplex).  No
    // echo drain — the monitor harmlessly skips the echoed (non-telemetry)
    // master frame and resyncs on the ESC's 0x3B response.
    //
    // NOTE: this forwards the Rx's frames Rx->ESC, but the reverse CONFIG-relay
    // (ESC config/menu responses -> Rx) is NOT IMPLEMENTED YET.  The monitor
    // only extracts the ESC's telemetry (0x3A); its config/menu replies are
    // dropped.  Two-way DeviceExplorer config (synchronous Rx<->ESC proxy in
    // the response slot, proprietary packet format) is a deferred feature.
    void forwardToEsc(const uint8_t* frame, uint8_t len) {
        if (!_escPort || !_escStream) return;
        _escPort->txEnable();
        _escStream->write(frame, len);
        _escStream->flush();
        _escPort->txDisable();
    }

    // ── Multi-device rotation (cursors advanced under the hub lock) ───
    uint8_t buildData(JetiTelemetryHub& hub, uint8_t* buf) {
        uint8_t devN = hub.deviceCount();
        if (!devN) return 0;
        const uint16_t maxSlots = (uint16_t)devN * JetiTelemetryHub::kMaxSensorsPerDevice;
        for (uint16_t i = 0; i < maxSlots; ++i) {
            const auto* d = hub.device(_dataDev);
            bool hit = (d && d->active && _dataSen < d->sensorCount &&
                        d->sensors[_dataSen].active);
            uint8_t dev = _dataDev, sen = _dataSen;
            // advance
            const uint8_t cnt = d ? d->sensorCount : 0;
            if (++_dataSen >= cnt) { _dataSen = 0; _dataDev = (uint8_t)((_dataDev + 1) % devN); }
            if (hit) {
                const auto& s = d->sensors[sen];
                return buildExDataBlock(buf, d->usn, d->lsn, s.id, s.type, s.decimals, s.value);
            }
            (void)dev;
        }
        return 0;
    }

    uint8_t buildText(JetiTelemetryHub& hub, uint8_t* buf) {
        uint8_t devN = hub.deviceCount();
        if (!devN) return 0;
        const uint16_t maxSlots = (uint16_t)devN * (JetiTelemetryHub::kMaxSensorsPerDevice + 1);
        for (uint16_t i = 0; i < maxSlots; ++i) {
            const auto* d = hub.device(_textDev);
            bool isName  = (d && d->active && _textSen < 0);
            bool isLabel = (d && d->active && _textSen >= 0 &&
                            _textSen < (int16_t)d->sensorCount &&
                            d->sensors[_textSen].active);
            uint8_t dev = _textDev; int16_t sen = _textSen;
            // advance
            const int16_t cnt = d ? (int16_t)d->sensorCount : 0;
            if (++_textSen >= cnt) { _textSen = -1; _textDev = (uint8_t)((_textDev + 1) % devN); }
            if (isName)  return buildExTextBlock(buf, d->usn, d->lsn, 0, d->name, nullptr);
            if (isLabel) { const auto& s = d->sensors[sen];
                           return buildExTextBlock(buf, d->usn, d->lsn, s.id, s.label, s.unit); }
            (void)dev;
        }
        return 0;
    }

    sfx_peripherals::InputPort* _rxPort  = nullptr;   // IN_1, Rx side
    sfx_peripherals::InputPort* _escPort = nullptr;   // IN_2, ESC side
    Stream*                     _escStream = nullptr;
    JetiExBus                   _rxBus;
    JetiExTelemetryMonitor      _escMon;

    static constexpr uint8_t kUptimeId  = 1;   // built-in HubFX-own sensors
    static constexpr uint8_t kFreeRamId = 2;
    uint8_t  _localDev = 0xFF;            // hub index of the HubFX-own device

    // Rotation cursors (data + text walk independently across the hub).
    uint16_t _seq     = 0;
    uint8_t  _dataDev = 0, _dataSen = 0;
    uint8_t  _textDev = 0;
    int16_t  _textSen = -1;               // -1 = device name, else sensor index

    TaskHandle_t   _task = nullptr;
    volatile bool  _stop = false;
};

}  // namespace JetiEx

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_JETI_EXPANDER_H
