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
        if (_rxTask) return true;                     // already running
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
        _rxBus.setTxPort(rxPort);                     // half-duplex TX on IN_1 (reply)
        _rxBus.onTelemetryRequest([this](uint8_t pkt){ serveTelemetry(pkt); });
        // No per-frame forward hook — the ESC is polled by its OWN task, on its
        // own schedule (two independent tasks, no cross-task coordination).

        // Register the HubFX-own device (local → never expires) + its built-in
        // sensors, so it shows on the radio even with no downstream ESC.
        {
            auto& hub = JetiTelemetryHub::instance();
            JetiTelemetryHub::ScopedLock lk(hub);
            _localDev = hub.upsertDevice(hubUsn, hubLsn, hubName, /*local=*/true, 0);
            if (_localDev != 0xFF) {
                hub.setSensor(_localDev, kUptimeId, ExDataType::Int22, 0, 0, 0);
                hub.setLabel (_localDev, kUptimeId, "Uptime", "s");
            }
        }

        _stop = false;
        // TWO independent Core-0 tasks, no cross-task signaling: the Rx task
        // (IN_1: channels + reply) at HIGHER priority than the ESC task (IN_2:
        // monitor + independent poll).  TEST build — the ESC poll runs even with
        // nothing attached, to measure whether splitting the tasks avoids the
        // IN_2->IN_1 crosstalk dropouts.
        BaseType_t ok = xTaskCreatePinnedToCore(
            &JetiExpander::rxTaskEntry, "jetiRx", 4096, this,
            /*priority=*/3, &_rxTask, /*core=*/0);
        if (ok != pdPASS) { _rxTask = nullptr; return false; }
        if (_escPort) {
            ok = xTaskCreatePinnedToCore(
                &JetiExpander::escTaskEntry, "jetiEsc", 4096, this,
                /*priority=*/2, &_escTask, /*core=*/0);
            if (ok != pdPASS) _escTask = nullptr;
        }
        SFX_LOG_INFO("[jexp] started: rxTask(IN_1)+escTask(IN_2)%s poll=%s baud=%lu",
                     (_escTask ? "" : " [no-esc]"),
                     (kForwardToEsc ? "on" : "off"), (unsigned long)baud);
        return true;
    }

    void end() {
        if (_rxTask || _escTask) {
            _stop = true;
            // Let both loops observe _stop and self-delete; then forget them.
            for (int i = 0; i < 50 && (_rxTask || _escTask); ++i) SFX_DELAY_MS(2);
            _rxTask = _escTask = nullptr;
        }
        _rxBus.end();
        _escMon.end();
        if (_rxPort)  _rxPort->disable();
        if (_escPort) _escPort->disable();
        _rxPort = _escPort = nullptr;
        _escStream = nullptr;
    }

    bool running() const { return _rxTask != nullptr; }

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

    static void rxTaskEntry(void* arg)  { static_cast<JetiExpander*>(arg)->rxTaskLoop(); }
    static void escTaskEntry(void* arg) { static_cast<JetiExpander*>(arg)->escTaskLoop(); }

    // IN_1 task: channels + telemetry reply (the reply hook fires inside update).
    void rxTaskLoop() {
        uint32_t lastBuiltin = 0, lastLog = 0;
        for (;;) {
            if (_stop) break;
            const uint32_t now = millis();
            _rxBus.update();

            if (now - lastBuiltin >= 1000 && _localDev != 0xFF) {
                lastBuiltin = now;
                auto& hub = JetiTelemetryHub::instance();
                JetiTelemetryHub::ScopedLock lk(hub);
                hub.setSensor(_localDev, kUptimeId, ExDataType::Int22, 0,
                              (int32_t)(now / 1000), now);
            }
#if SFX_INSTRUMENTATION
            if (now - lastLog >= 1000) {
                lastLog = now;
                SFX_LOG_INFO("[jexp] rxF=%lu rxE=%lu tx=%lu | escF=%lu escE=%lu poll=%lu | dev=%u",
                             (unsigned long)_rxBus.rxFrameCount(),
                             (unsigned long)_rxBus.rxErrorCount(),
                             (unsigned long)_rxBus.txResponseCount(),
                             (unsigned long)_escMon.telemetryFrames(),
                             (unsigned long)_escMon.errorCount(),
                             (unsigned long)_pollCount,
                             (unsigned)JetiTelemetryHub::instance().deviceCount());
            }
#else
            (void)lastLog;
#endif
            vTaskDelay(1);
        }
        _rxTask = nullptr;
        vTaskDelete(nullptr);
    }

    // IN_2 task: drain the ESC's telemetry into the hub + poll the ESC on its
    // OWN schedule (no coordination with the Rx task).
    void escTaskLoop() {
        uint32_t lastPoll = 0, lastExpire = 0;
        for (;;) {
            if (_stop) break;
            const uint32_t now = millis();
            _escMon.update(now);
            if (kForwardToEsc && now - lastPoll >= kEscPollIntervalMs) {
                lastPoll = now;
                sendEscPoll();
            }
            if (now - lastExpire >= 1000) {
                lastExpire = now;
                auto& hub = JetiTelemetryHub::instance();
                JetiTelemetryHub::ScopedLock lk(hub);
                hub.expireStale(now, 2000);   // disconnected ESC drops out
            }
            vTaskDelay(1);
        }
        _escTask = nullptr;
        vTaskDelete(nullptr);
    }

    // Telemetry hook (runs in-task during _rxBus.update()): build the next
    // multi-device frame from the hub (under lock), then half-duplex TX it.
    void serveTelemetry(uint8_t pktId) {
        // Rate-limit replies (~30 Hz).  Each reply is a half-duplex TX turnaround
        // on IN_1; replying to EVERY poll leaves too little channel-RX headroom,
        // and the odd late reply overruns the slot and corrupts a frame (RC
        // signal gaps).  The radio tolerates skipped polls (it just asks again);
        // telemetry still updates smoothly.
        const uint32_t now = millis();
        if (now - _lastRespMs < kRespIntervalMs) return;
        _lastRespMs = now;

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

    // Poll the downstream ESC: emit a master TELEMETRY-REQUEST on IN_2 (half-
    // duplex), generated locally (no copy of the Rx's frame — the ESC replies to
    // any request).  Frame: [0x3D][0x01][len=8][pktId][0x3A][0x00][crc16LE].
    // The ESC's reply lands on IN_2 and is decoded by _escMon on the next tick.
    // NOTE: the reverse CONFIG-relay (ESC menu replies -> Rx) is NOT IMPLEMENTED.
    void sendEscPoll() {
        if (!_escPort || !_escStream) return;
        uint8_t f[8];
        f[0] = START_ADDR1;          // 0x3D — master, response allowed
        f[1] = 0x01;
        f[2] = 8;                    // total length incl. header + CRC
        f[3] = _escPktId++;
        f[4] = DATA_TELEMETRY;       // 0x3A — telemetry request
        f[5] = 0;                    // 0-length data block = request
        const uint16_t c = crc16_ccitt(f, 6);
        f[6] = (uint8_t)(c & 0xFF);
        f[7] = (uint8_t)(c >> 8);
        _escPort->txEnable();
        _escStream->write(f, sizeof f);
        _escStream->flush();
        _escPort->txDisable();
        ++_pollCount;
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

    // TEST build: ESC poll ON, driven by its own IN_2 task (no coordination with
    // the Rx task) — to measure whether two independent tasks avoid the IN_2->
    // IN_1 crosstalk dropouts.  (Previously the per-frame forward crosstalked
    // GPIO6->GPIO5 and dropped RC frames.)
    static constexpr bool     kForwardToEsc      = true;
    static constexpr uint8_t  kUptimeId          = 1;    // built-in HubFX-own sensor
    static constexpr uint32_t kEscPollIntervalMs = 100;  // ESC poll rate (~10 Hz)
    static constexpr uint32_t kRespIntervalMs    = 50;   // Rx reply rate cap (~20 Hz)
    uint32_t _lastRespMs = 0;            // last Rx-reply time (rate limit)
    uint32_t _pollCount  = 0;            // ESC polls sent (diag)
    uint8_t  _escPktId   = 0;            // ESC poll packet-id counter
    uint8_t  _localDev   = 0xFF;         // hub index of the HubFX-own device

    // Rotation cursors (data + text walk independently across the hub).
    uint16_t _seq     = 0;
    uint8_t  _dataDev = 0, _dataSen = 0;
    uint8_t  _textDev = 0;
    int16_t  _textSen = -1;               // -1 = device name, else sensor index

    TaskHandle_t   _rxTask  = nullptr;   // IN_1 task (channels + reply)
    TaskHandle_t   _escTask = nullptr;   // IN_2 task (monitor + poll)
    volatile bool  _stop = false;
};

}  // namespace JetiEx

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_JETI_EXPANDER_H
