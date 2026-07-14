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
 * The TelemetryHub is the shared merge point.  Devices keep their identity,
 * so the radio shows the downstream device under its own name next to "HubFx".
 *
 * ESP32-only (FreeRTOS task + half-duplex GPIO-matrix TX).
 */

#ifndef SFX_JETI_EXPANDER_H
#define SFX_JETI_EXPANDER_H

#include <platform/sfx_platform.h>
#if SFX_PLATFORM_ESP32

#include <platform/sfx_stream.h>   // sfx::Stream (was <Arduino.h>)
#include <freertos/FreeRTOS.h>     // dedicated IN_1 servicing task
#include <freertos/task.h>
#include <atomic>                  // deferred link-loss restart request (Rule 15)
#include <cstdint>
#include <cstdio>                  // snprintf (saved hub name for restart)
#include <cstring>                 // memcpy (ESC poll-template capture)
#include <functional>

#include <ports/input_port.h>
#include <serial/diag_log.h>
#include "jeti_ex_bus.h"
#include "jeti_ex_telemetry_monitor.h"
#include <telemetry/telemetry_hub.h>

namespace JetiEx {

using sfx_telemetry::TelemetryHub;
using sfx_telemetry::SensorKind;

#if SFX_INSTRUMENTATION
/// Pass-through sfx::Stream that snapshots the bytes read through it —
/// bench diagnostics for the downstream (IN_2) link: lets the expander
/// hex-dump exactly what arrives (echo vs slave replies vs noise) without
/// disturbing the monitor's decode.  Instrumentation builds only.
class EscTeeStream final : public sfx::Stream {
public:
    void attach(sfx::Stream* inner) { _inner = inner; _len = 0; }
    int  available() override { return _inner ? _inner->available() : 0; }
    int  read() override {
        if (!_inner) return -1;
        const int c = _inner->read();
        if (c >= 0 && _len < sizeof(_snap)) _snap[_len++] = (uint8_t)c;
        return c;
    }
    size_t readBytes(uint8_t* buf, size_t n) override {
        if (!_inner) return 0;
        const size_t got = _inner->readBytes(buf, n);
        for (size_t i = 0; i < got && _len < sizeof(_snap); ++i) _snap[_len++] = buf[i];
        return got;
    }
    int    peek() override { return _inner ? _inner->peek() : -1; }
    size_t write(uint8_t b) override { return _inner ? _inner->write(b) : 0; }
    size_t write(const uint8_t* b, size_t n) override { return _inner ? _inner->write(b, n) : 0; }
    void   flush() override { if (_inner) _inner->flush(); }
    void   flushRx() override { if (_inner) _inner->flushRx(); }
    explicit operator bool() const override { return _inner && (bool)*_inner; }

    /// Drain the snapshot into a hex string (and reset).  Returns bytes taken.
    size_t takeHex(char* out, size_t outLen) {
        const size_t n = _len;
        size_t o = 0;
        for (size_t i = 0; i < n && o + 3 < outLen; ++i)
            o += (size_t)snprintf(out + o, outLen - o, "%02X", _snap[i]);
        out[o] = '\0';
        _len = 0;
        return n;
    }
private:
    sfx::Stream* _inner = nullptr;
    uint8_t _snap[48] = {};
    size_t  _len = 0;
};
#endif  // SFX_INSTRUMENTATION

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
               uint32_t baud = 125000,
               bool respondTelemetry = false) {
        if (_running) return true;                    // already running
        if (!rxPort) return false;
        _rxPort  = rxPort;
        _baud    = baud;                              // kept for UART re-init on loss
        // Save the full begin() parameter set so the deferred link-loss
        // restart (maybeResetRx -> tickMainLoop) can re-run begin() verbatim.
        _escPortOrig = escPort;
        _hubUsn = hubUsn;
        _hubLsn = hubLsn;
        std::snprintf(_hubName, sizeof(_hubName), "%s", hubName ? hubName : "HubFx");
        if (!rxPort->configureJetiEx(baud)) return false;
        sfx::Stream* rxStream = rxPort->uartStream();
        if (!rxStream) return false;

        // Downstream (ESC) link on IN_2 — AUTODETECT, no config flag (was the
        // opt-in `downstream:`).  We always configure IN_2 and let the presence
        // machine decide if a device is there: it SLOW-probes (200 ms) only by
        // replaying a captured REAL Rx poll (maybePollEsc fires nothing until
        // IN_1 has captured one — i.e. only while the radio is actually driving
        // IN_1), and the monitor drain is BOUNDED, so a disconnected/floating
        // IN_2 (held idle by its pull-up) just reads "absent" — no stall, no
        // crosstalk floods.  A device that replies is promoted to ACTIVE and
        // shows on the radio automatically.  Channel RX on IN_1 is unaffected.
        _escPort   = nullptr;
        _escStream = nullptr;
        if (escPort && escPort->configureJetiEx(baud)) {
            _escStream = escPort->uartStream();
            if (_escStream) {
                _escPort = escPort;
#if SFX_INSTRUMENTATION
                _escTee.attach(_escStream);
                _escMon.begin(&_escTee);   // monitor reads through the tee (hex diag)
#else
                _escMon.begin(_escStream);
#endif
            }
        }

        if (!_rxBus.begin(rxStream)) return false;
        // Fresh link monitors — the new bus counters start at zero, and stale
        // baselines from a previous run would read as a wrap (spurious NOISY)
        // and keep deadSecs climbing across a restart.
        _rxWatch  = LinkMonitor{};
        _escWatch = LinkMonitor{};
        _lastRxResetAtDead = 0;
        _respond = respondTelemetry;
        // Two-way telemetry (half-duplex reply) — RUNTIME-gated via _respond.
        // History: disabled 2026-05-30 because the ~3 ms TX reply on the shared
        // IN_1 line sagged the rail under engine/heater load → corrupted channel
        // RX (rxErr flood, ----).  The single-wire line now carries a 10 kΩ
        // pull-up (spec topology) to hold the idle level; this branch re-enables
        // the reply for bench validation, watched by the [jexp] TX instrumentation
        // (echoShort / slotOverruns / rxErr).  Default OFF (listen-only) when the
        // attach config doesn't request it.
        if (_respond) {
            _rxBus.setTxPort(rxPort);                 // half-duplex TX on IN_1 (reply)
            _rxBus.onTelemetryRequest([this](uint8_t pkt){ serveTelemetry(pkt); });
        }
        // Phase 2 — downstream ESC master link (IN_2).  We do NOT mirror every
        // Rx frame out IN_2 (its TX crosstalks GPIO6->GPIO5 onto the IN_1 channel
        // RX and dropped the signal).  Instead we CAPTURE the Rx's latest
        // telemetry-request frame as a poll template and replay it on IN_2 ONLY
        // inside IN_1's guaranteed-quiet post-reply window (maybePollEsc, called
        // from serveTelemetry), at the autodetect cadence.  Capturing is harmless
        // (no TX) so it's always armed when the downstream link is up.
        if (_escPort) {
            _rxBus.onRawFrame([this](const uint8_t* f, uint8_t l){ captureEscPoll(f, l); });
        }

        // Register the HubFX-own device (local → never expires) + its built-in
        // sensors, so it shows on the radio even with no downstream ESC.
        {
            auto& hub = TelemetryHub::instance();
            TelemetryHub::ScopedLock lk(hub);
            _localDev = hub.upsertDevice(hubUsn, hubLsn, hubName, /*local=*/true, 0);
            if (_localDev != 0xFF) {
                hub.setSensor(_localDev, kUptimeId, SensorKind::Int, 0, 0, 0);
                hub.setLabel (_localDev, kUptimeId, "Uptime", "s");
            }
        }

        // Run in the MAIN LOOP, driven by JetiExInputRole::tick() → update() —
        // NOT a separate task.  With telemetry disabled the only work is RC
        // channel decode (not time-critical), and a prio-3 Core-0 task preempted
        // the main loop and starved the storage/upload pipeline (file.list +
        // segment-ACK timeouts).  Cooperative main-loop decode is the pre-refactor
        // design and is naturally paused by the upload-exclusivity gate.  Revisit
        // a task only if telemetry (the ~4 ms reply slot) is re-enabled.
        _running     = true;
        _lastBuiltin = _lastExpire = 0;
        const uint32_t now0 = SFX_MILLIS();
        _rxWatch.reset(now0);
        _escWatch.reset(now0);

        // Telemetry reply is time-critical: the master reserves a ~4 ms slot and
        // our reply is ~2 ms, so it must START within ~1.3 ms of the poll.  The
        // cooperative main loop runs 3–35 ms under effect load — far too slow to
        // ever answer in-window (measured resp=1 / lateSkip≈100%).  So when
        // RESPONDING, OWN IN_1 on a dedicated Core-0 task that services it every
        // ~1 ms.  Core 1 is reserved for audio real-time (moving work there spiked
        // underruns 10×); Core 0 has the headroom.  The task SLEEPS 1 tick (1 ms @
        // 1000 Hz) each pass so it yields Core 0 to loopTask/storage, and it is
        // GATED OFF during uploads — the two things the prior prio-3 spin-task
        // lacked when it starved the upload pipeline (2026-05-30).  Listen-only
        // (no reply) is not time-critical → keep the cheap main-loop decode.
        if (_respond) {
            _taskRunning = true;
            if (xTaskCreatePinnedToCore(&JetiExpander::taskTrampoline, "jeti_in1",
                                        kTaskStackBytes, this, kTaskPrio,
                                        &_task, /*core=*/0) != pdPASS) {
                _task = nullptr;
                _taskRunning = false;
                SFX_LOG_WARN("[jexp] IN_1 task spawn FAILED — main-loop decode fallback");
            } else {
                SFX_LOG_INFO("[jexp] IN_1 task started (Core 0 prio %u, %u B stack)",
                             (unsigned)kTaskPrio, (unsigned)kTaskStackBytes);
            }
        }
        SFX_LOG_INFO("[jexp] started (rx=IN_1%s, esc=IN_2%s, baud=%lu)",
                     _task ? " task" : " loop",
                     _escPort ? "" : " off", (unsigned long)baud);
        return true;
    }

    // Main-loop entry (from JetiExInputRole::tick()).  Runs the cooperative
    // decode ONLY when the dedicated IN_1 task is NOT running — i.e. listen-only
    // mode.  When responding, the task owns the UART and this is a no-op so the
    // two never double-drive the port.
    void tickMainLoop() {
        // Deferred link-loss restart — REQUESTED by maybeResetRx (which runs
        // on the expander's own IN_1 task, where a UART teardown/re-init
        // crashes: the 2026-05 lesson that got the inline reset disabled)
        // and EXECUTED here on the main loop, the context end()/begin()
        // normally run in.  end() joins the task before touching the UART,
        // so the restart is race-free; a replug mid-restart just starts
        // decoding on the fresh UART.
        if (_restartReq.exchange(false, std::memory_order_acq_rel)) {
            restartAfterLinkLoss();
            return;
        }
        if (!_task) update();
    }

    // Drive one decode pass — called every ~1 ms by the IN_1 task (responding)
    // or every main-loop iteration via tickMainLoop() (listen-only).  Drains
    // IN_2 (ESC monitor) then IN_1 (channels +, when telemetry is enabled, the
    // reply hook), and refreshes built-in sensors.  No-op until begin()/end().
    void update() {
        if (!_running) return;
        // Main-loop lag (µs since the previous update()): the worst-case AGE of a
        // poll parsed in this drain — it could have arrived the instant the last
        // drain ended and waited the whole interval.  serveTelemetry() uses it as
        // the timeliness gate: a stale poll's ~2 ms reply would overrun the
        // master's ~4 ms window and collide with its next channel frame.
        const uint32_t nowUs = SFX_MICROS();
        _loopLagUs = (_lastUpdateUs == 0) ? 0 : (nowUs - _lastUpdateUs);
        if (_loopLagUs > _maxLoopLagUs) _maxLoopLagUs = _loopLagUs;
        _lastUpdateUs = nowUs;
        const uint32_t now = SFX_MILLIS();
        // IN_2 first: drain the ESC's telemetry into the hub before IN_1
        // re-parses the next echoed master frame.  Only when the downstream
        // link is in use (otherwise _escPort is null — see begin()).  ALWAYS
        // drain (bounded) — the monitor only watches health, never gates.
        if (_escPort) {
            _escMon.update(now);
            _escWatch.observe(now, _escMon.rxByteCount(), _escMon.frameCount(), "IN_2");
            updateEscPresence(now);   // promote/demote ESC + adjust poll cadence
        }
        _rxBus.update();            // IN_1: master frames → channels (+ reply hook)
        _rxWatch.observe(now, _rxBus.rxByteCount(), _rxBus.rxFrameCount(), "IN_1");
        maybeResetRx(now);          // UART reset/reconnect on prolonged IN_1 loss

#if SFX_INSTRUMENTATION
        // Concise rx health (every 2 s): bytes = line active? frames = decoding?
        // err = CRC failures (wrong baud / unhandled frame types / noise);
        // ch/valid = channel decode state.  The "mark what is wrong" counters.
        if (now - _lastRxLog >= 2000) {
            _lastRxLog = now;
            SFX_LOG_INFO("[jexp] IN_1 rxB=%lu rxF=%lu rxErr=%lu ch=%u valid=%d%s",
                         (unsigned long)_rxBus.rxByteCount(),
                         (unsigned long)_rxBus.rxFrameCount(),
                         (unsigned long)_rxBus.rxErrorCount(),
                         _rxBus.channelCount(), _rxBus.isValid() ? 1 : 0,
                         _rxWatch.noisy() ? " NOISY" : "");
        }
        // Two-way reply health (every 2 s, only while responding): polls vs resp
        // = reply coverage; echoShort + slotOver = TX/RX-turnaround issues;
        // lastUs/maxUs = did the reply fit the ~4 ms slot.  Cross-check rxErr
        // above — if it climbs as resp climbs, the TX reply is corrupting RX.
        if (_respond && now - _lastTxLog >= 2000) {
            _lastTxLog = now;
            SFX_LOG_INFO("[jexp] TX polls=%lu resp=%lu lateSkip=%lu echoShort=%lu slotOver=%lu lastUs=%lu maxUs=%lu loopLagUs=%lu maxLoopLagUs=%lu",
                         (unsigned long)_rxBus.pollsSeen(), (unsigned long)_rxBus.txResponseCount(),
                         (unsigned long)_lateSkip,
                         (unsigned long)_rxBus.echoShort(), (unsigned long)_rxBus.slotOverruns(),
                         (unsigned long)_rxBus.lastTxDurUs(), (unsigned long)_rxBus.maxTxDurUs(),
                         (unsigned long)_loopLagUs, (unsigned long)_maxLoopLagUs);
            _maxLoopLagUs = 0;   // reset the 2 s window peak
        }
        // Downstream ESC (IN_2) autodetect health (every 2 s, only when the
        // downstream link is up): present = autodetect state; polls = IN_2 master
        // polls issued (windowed into IN_1's quiet slot); replies = ESC telemetry
        // frames decoded; rxErr climbing with no replies ⇒ wrong baud / crosstalk.
        if (_escPort && now - _lastEscLog >= 2000) {
            _lastEscLog = now;
            SFX_LOG_INFO("[jexp] IN_2 ESC present=%d polls=%lu chMir=%lu replies=%lu sensors=%lu rxB=%lu rxErr=%lu",
                         _escPresent ? 1 : 0, (unsigned long)_escPolls,
                         (unsigned long)_escChanMirrors,
                         (unsigned long)_escMon.telemetryFrames(),
                         (unsigned long)_escMon.sensorUpdates(),
                         (unsigned long)_escMon.rxByteCount(),
                         (unsigned long)_escMon.errorCount());
            // Bench ground-truth: first bytes seen on IN_2 RX this window
            // (echo of our own TX + anything the slave sent).  Known TX
            // prefixes: 3E 03 poll (telemetry request), 3E 01 channel frame.
            {
                char hex[100];
                if (_escTee.takeHex(hex, sizeof(hex)) > 0)
                    SFX_LOG_INFO("[jexp] IN_2 rx[..48]: %s", hex);
            }
        }
#endif

        if (now - _lastBuiltin >= 1000 && _localDev != 0xFF) {
            _lastBuiltin = now;
            auto& hub = TelemetryHub::instance();
            TelemetryHub::ScopedLock lk(hub);
            hub.setSensor(_localDev, kUptimeId, SensorKind::Int, 0,
                          (int32_t)(now / 1000), now);
        }
        if (now - _lastExpire >= 1000) {
            _lastExpire = now;
            auto& hub = TelemetryHub::instance();
            TelemetryHub::ScopedLock lk(hub);
            hub.expireStale(now, 2000);   // disconnected ESC drops out
        }
    }

    /// Pause the IN_1 task's UART servicing while this predicate is true
    /// (Rule 28 upload exclusivity).  The prior Core-0 Jeti task starved the
    /// upload pipeline precisely because it had no such gate; the sketch wires
    /// this to the storage policy's isUploadActive().
    void setUploadGate(std::function<bool()> fn) { _uploadGate = std::move(fn); }

    /// Enable mirroring of captured channel frames onto the downstream (IN_2)
    /// link — REQUIRED for EX Bus slaves whose supervision needs master
    /// channel frames (Kolibri), but ONLY safe on boards whose IN_2 TX pin
    /// does not crosstalk the IN_1 RX (HubFX rev B split-pin headers; keep
    /// OFF on rev A).  Persistent across begin()/end().
    void setEscChannelMirror(bool on) { _escChanMirror = on; }
    uint32_t escChanMirrors() const { return _escChanMirrors; }

    /// The port begin() was offered as the downstream EX link (null = none).
    sfx_peripherals::InputPort* downstreamPort() const { return _escPortOrig; }

    /// Re-point the downstream (ESC) EX link at `port` — nullptr RELEASES it
    /// (a native esc-telemetry role is taking the UART), non-null (re)adopts
    /// it (the port switched back to the jeti-exbus downstream role).  Restarts
    /// the expander with the saved begin() parameter set, so IN_1 channel
    /// decode blips for one begin() (~ms) — fine for a config edit.  Main-loop
    /// context only (the role-attach path).  No-op when not running (the next
    /// attachInput scan picks the port up) or when already on that port.
    bool setDownstreamPort(sfx_peripherals::InputPort* port) {
        if (!_running) return true;
        if (_escPortOrig == port) return true;
        auto* rx = _rxPort;
        const uint32_t baud = _baud;
        const bool respond  = _respond;
        const uint16_t usn = _hubUsn, lsn = _hubLsn;
        char name[sizeof(_hubName)];
        std::snprintf(name, sizeof(name), "%s", _hubName);
        SFX_LOG_INFO("[jexp] downstream EX link %s — restarting expander",
                     port ? "adopted" : "released (native esc-telemetry owns the port)");
        end();
        return begin(rx, port, usn, lsn, name, baud, respond);
    }

    void end() {
        _running = false;             // task loop + update() become no-ops
        if (_task) {                  // wait for the task to exit before teardown
            for (int i = 0; i < 200 && _taskRunning; ++i) vTaskDelay(pdMS_TO_TICKS(1));
            _task = nullptr;
        }
        _rxBus.end();
        _escMon.end();
        if (_rxPort)  _rxPort->disable();
        if (_escPort) _escPort->disable();
        _rxPort = _escPort = nullptr;
        _escStream = nullptr;
    }

    bool running() const { return _running; }

    // ── HubFX-own telemetry (the local device) ───────────────────────
    void setLocalSensor(uint8_t id, const char* label, const char* unit,
                        uint8_t decimals, int32_t value, uint32_t nowMs) {
        if (_localDev == 0xFF) return;
        auto& hub = TelemetryHub::instance();
        TelemetryHub::ScopedLock lk(hub);
        hub.setSensor(_localDev, id, SensorKind::Int, decimals, value, nowMs);
        if (label) hub.setLabel(_localDev, id, label, unit);
    }
    void setLocalValue(uint8_t id, int32_t value, uint32_t nowMs) {
        if (_localDev == 0xFF) return;
        auto& hub = TelemetryHub::instance();
        TelemetryHub::ScopedLock lk(hub);
        // Preserve kind/decimals — re-set via the existing sensor entry.
        const auto* d = hub.device(_localDev);
        if (!d) return;
        for (uint8_t i = 0; i < d->sensorCount; ++i)
            if (d->sensors[i].id == id) {
                hub.setSensor(_localDev, id, d->sensors[i].kind,
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
    // Two-way (half-duplex reply) instrumentation.
    bool     responding()   const { return _respond; }
    uint32_t pollsSeen()    const { return _rxBus.pollsSeen(); }
    uint32_t echoShort()    const { return _rxBus.echoShort(); }
    uint32_t maxTxDurUs()   const { return _rxBus.maxTxDurUs(); }
    uint32_t slotOverruns() const { return _rxBus.slotOverruns(); }
    uint32_t lateSkip()     const { return _lateSkip; }      // replies gated (loop behind)
    uint32_t loopLagUs()    const { return _loopLagUs; }     // last main-loop interval
    uint32_t maxLoopLagUs() const { return _maxLoopLagUs; }  // worst main-loop interval
    uint32_t escBytes()  const { return _escMon.rxByteCount(); }
    uint32_t escFrames() const { return _escMon.telemetryFrames(); }
    uint32_t escErrors() const { return _escMon.errorCount(); }
    uint8_t  deviceCount() const { return TelemetryHub::instance().deviceCount(); }
    uint8_t  sensorCount() const { return TelemetryHub::instance().activeSensorCount(); }

    /// Current publish interval (ms) — scaled so each of the N active metric
    /// types refreshes at ~kPerValueHz: interval = 1000 / (N·perValueHz), clamped
    /// to [kMinReplyIntervalMs, kMaxReplyIntervalMs].  Recomputed per poll so it
    /// tracks the collection growing/shrinking (ESC hot-plug, local metrics).
    uint32_t replyIntervalMs() const {
        const uint32_t n = sensorCount() ? sensorCount() : 1u;
        uint32_t iv = 1000u / (n * kPerValueHz);
        if (iv < kMinReplyIntervalMs) iv = kMinReplyIntervalMs;
        if (iv > kMaxReplyIntervalMs) iv = kMaxReplyIntervalMs;
        return iv;
    }

    /// True while a link is active (bytes flowing) but NOT decoding valid
    /// frames — surfaced in diag so the operator sees WHY channels are dead
    /// (noisy line / wrong baud / unhandled frame types).  The drain keeps
    /// running regardless (we never give up on catching a real frame).
    bool rxLinkNoisy()  const { return _rxWatch.noisy(); }
    bool escLinkNoisy() const { return _escWatch.noisy(); }
    uint32_t rxBrownouts() const { return _rxBrownouts; }   // IN_1 UART resets (diag)
    uint32_t rxNoiseSecs() const { return _rxWatch.totalNoiseSecs; }

private:
    JetiExpander() = default;
    JetiExpander(const JetiExpander&) = delete;
    JetiExpander& operator=(const JetiExpander&) = delete;

    // ── Dedicated IN_1 servicing task (Core 0) ───────────────────────
    static void taskTrampoline(void* arg) {
        static_cast<JetiExpander*>(arg)->taskLoop();
    }
    void taskLoop() {
        while (_running) {
            // Upload exclusivity (Rule 28): skip ALL UART servicing while an
            // upload holds the pipeline, so we never steal Core-0 cycles from
            // the SD writer (the 2026-05-30 starvation).  The 1-tick sleep
            // yields Core 0 to loopTask/storage between passes — the half-duplex
            // reply's flush() also yields (uart_wait_tx_done blocks), so the
            // task's real CPU burn is microseconds despite the high priority.
            if (!(_uploadGate && _uploadGate())) update();
            vTaskDelay(pdMS_TO_TICKS(1));   // 1 ms @ 1000 Hz tick
        }
        _taskRunning = false;
        _task = nullptr;
        vTaskDelete(nullptr);
    }
    // Core 0 (NOT core 1 — audio real-time).  Priority ABOVE the audio-decoder
    // + CDC driver (both prio 5 on Core 0): the half-duplex reply must release
    // the line (txDisable) the instant the ~2 ms TX completes, or the master's
    // next channel frame collides with our still-driven line → lost tracking.
    // At prio 4 a mid-reply decoder preempt stretched the bracket to 17-29 ms
    // (bench 2026-06-10, build 845) and dropped the signal during engine-sound
    // MP3 churn.  At prio 6 the task resumes immediately after flush() yields and
    // runs txDisable before the master frame arrives.  Safe: the task sleeps 1 ms
    // between passes and yields through flush(), so it steals only microseconds
    // from the decoder (which has ring headroom).  6 KB stack covers the reply
    // build + the 2 s [jexp] DiagLog line (the hot path itself never logs).
    static constexpr uint32_t   kTaskStackBytes = 6144;
    static constexpr UBaseType_t kTaskPrio       = 6;

    // ── Link health monitor (NON-disabling) ──────────────────────────
    // The bounded frame-parser drain (kMaxDrainBytesPerCall) is the safety net
    // that stops a noisy/floating UART from EVER stalling the loop, so we always
    // KEEP DRAINING — even a marginal / mixed line (channel frames interleaved
    // with telemetry-request frames, or frame types this parser doesn't decode)
    // must keep getting drained or we'd starve the real channel frames.  This
    // monitor therefore never gates the drain; it only watches per-second
    // health (bytes flowing? valid frames decoding?), counts noisy seconds for
    // diag, and logs HEALTH TRANSITIONS (not per-second spam) so the operator
    // can see WHY channels are dead (line silent vs noisy-but-no-valid-frames)
    // without killing a real signal.  Earlier this DISABLED the drain after a
    // few no-frame seconds, which starved a connected receiver whose stream had
    // brief no-decode gaps — removed per that bench finding.
    static constexpr uint32_t kNoiseBytesPerSec = 200;   // line "active" floor

    struct LinkMonitor {
        uint32_t lastMs = 0, baseBytes = 0, baseFrames = 0;
        uint32_t noiseSecs = 0;       // consecutive sec: bytes flowing, 0 frames
        uint32_t totalNoiseSecs = 0;  // cumulative (diag)
        uint32_t deadSecs = 0;        // consecutive sec with NO valid frames (quiet OR noisy)
        bool     healthy = false;     // valid frames decoded in the last window
        bool     primed  = false;     // seen at least one window

        void reset(uint32_t now) {
            lastMs = now; baseBytes = baseFrames = 0;
            noiseSecs = totalNoiseSecs = deadSecs = 0; healthy = false; primed = false;
        }
        bool noisy() const { return primed && !healthy && noiseSecs > 0; }

        // Monitor only — NEVER stops the drain.  Updates health + logs the
        // transitions healthy<->noisy.
        void observe(uint32_t now, uint32_t bytes, uint32_t frames,
                     const char* label) {
            if (now - lastMs < 1000) return;
            const uint32_t db = bytes - baseBytes;
            const uint32_t df = frames - baseFrames;
            lastMs = now; baseBytes = bytes; baseFrames = frames;
            const bool was = healthy;
            if (df > 0) {
                healthy = true; noiseSecs = 0;
            } else if (db >= kNoiseBytesPerSec) {
                healthy = false; noiseSecs++; totalNoiseSecs++;
            } // else: line quiet (no bytes) — leave healthy as-is, no spam
            // Time since last VALID frame (quiet OR noisy) — drives the UART
            // reset/reconnect recovery in the expander (maybeResetRx).
            deadSecs = (df > 0) ? 0 : (deadSecs + 1);
            if (primed && was && !healthy)
                SFX_LOG_WARN("[jexp] %s: %lu B/s but 0 valid frames — line NOISY "
                             "(still draining to catch real frames)",
                             label, (unsigned long)db);
            else if (primed && !was && healthy)
                SFX_LOG_INFO("[jexp] %s: valid frames decoding (%lu/s)",
                             label, (unsigned long)df);
            primed = true;
        }
    };
    LinkMonitor _rxWatch;
    LinkMonitor _escWatch;

    // Telemetry hook (runs during update()'s _rxBus.update()): build the next
    // multi-device frame from the hub (under lock), then half-duplex TX it.
    void serveTelemetry(uint8_t pktId) {
        // Rate-limit replies (~30 Hz).  Each reply is a half-duplex TX turnaround
        // on IN_1; replying to EVERY poll leaves too little channel-RX headroom,
        // and the odd late reply overruns the slot and corrupts a frame (RC
        // signal gaps).  The radio tolerates skipped polls (it just asks again);
        // telemetry still updates smoothly.
        const uint32_t now = SFX_MILLIS();
        if (now - _lastRespMs < replyIntervalMs()) return;

        // TIMELINESS GATE (the real fix for the gun-trigger RX drop).  The Jeti
        // master reserves a ~4 ms window after a poll-with-break and tolerates a
        // SKIPPED reply (it just re-polls — non-answer is spec-legal: it also
        // sends no-break polls where no answer is expected).  Our reply is a
        // ~2 ms half-duplex TX, so it only fits if it STARTS promptly.  When the
        // cooperative main loop hitches (gun trigger / ROF-change handler runs on
        // it), this drain falls behind and the poll we just parsed is already
        // stale — TXing now would overrun into the master's next channel frame
        // and corrupt the RC stream (the symptom).  So if the loop lag exceeds
        // the slack (window − reply), DROP this reply.  Channel decode (the
        // priority) is untouched; telemetry just updates a beat later.
        if (_loopLagUs > kReplyLagBudgetUs) { _lateSkip++; return; }
        _lastRespMs = now;

        auto& hub = TelemetryHub::instance();
        uint8_t buf[40];
        uint8_t len = 0;
        {
            TelemetryHub::ScopedLock lk(hub);
            _seq++;
            // Priority: a fresh device MESSAGE (fault warning/error → radio
            // popup) preempts the normal rotation — each new message is
            // repeated kMsgRepeats times so a lossy slot can't swallow it.
            len = buildMessage(hub, buf);
            // Text (device names + sensor labels) every 8th reply — labels
            // rarely change, so spend the rest on packed value frames (max
            // throughput; the radio caches labels once seen).
            if (!len) {
                if ((_seq % 8) == 0) len = buildText(hub, buf); // names + labels
                else                 len = buildData(hub, buf); // packed values
                if (!len)            len = buildText(hub, buf);  // fallback
            }
        }
        if (len) _rxBus.sendTelemetry(pktId, buf, len);

        // IN_1 is now in its guaranteed-quiet slot: the master released the line
        // for ~4 ms and our reply consumed only ~2 ms.  This is the ONLY safe
        // window to TX on IN_2 — its TX crosstalks the adjacent IN_1 RX, so it
        // must land while IN_1 is idle or it corrupts a channel frame.
        // Emulate the REAL master cycle downstream.  When the poll is due:
        // channel frame immediately followed by the telemetry request, then
        // the guarded quiet reply window — a Kolibri only opens its reply
        // slot for a request that follows channel data the way a real
        // master sends it (bench 2026-07-14: detached probe polls got 4
        // grace replies then mute; polls colliding with mirrors got none).
        // Otherwise just keep the heartbeat mirror going.
        if (escPollDue(now)) {
            sendEscChan(now);      // channel data …
            sendEscPoll(now);      // … then the request, back-to-back
        } else {
            maybeMirrorEscChan(now);
        }
    }

    // ── Downstream ESC master link (IN_2) — Phase 2 ──────────────────
    // Capture the Rx's latest TELEMETRY-REQUEST (0x3A) frame as the poll template
    // to replay at the ESC.  We replay a RECENT real master poll (its packetId is
    // fresh from the Rx stream) rather than synthesise one — the ESC, a slave on
    // its bus, replies to any valid telemetry request; the hub decodes whatever
    // comes back via _escMon.  Capture is pure copy (no TX) — always safe.
    void captureEscPoll(const uint8_t* frame, uint8_t len) {
        if (!_escPort || len < 6) return;
        if (frame[4] == DATA_TELEMETRY && len <= sizeof(_escPoll)) {
            memcpy(_escPoll, frame, len);
            _escPollLen = len;
            return;
        }
        // Channel-data frames (0x31) — captured as the downstream bus
        // HEARTBEAT.  An EX Bus slave (bench: Kontronik Kolibri, 2026-07-14)
        // answers a few telemetry polls after power-up and then MUTES if it
        // never sees master channel frames — its bus supervision declares the
        // bus dead.  Mirroring the latest channel frame keeps the downstream
        // bus alive; see maybeMirrorEscChan (opt-in — split-pin boards only).
        if (frame[4] == DATA_CHANNEL && len <= sizeof(_escChan)) {
            memcpy(_escChan, frame, len);
            _escChanLen  = len;
            _escChanFresh = true;
        }
    }

    // Poll the ESC on IN_2 — CALLED ONLY from serveTelemetry, i.e. inside IN_1's
    // quiet reply slot.  The IN_2 TX (~1 ms) crosstalks the adjacent IN_1 RX, so
    // it MUST land here or it corrupts a channel frame (why plain mirroring was
    // disabled).  Cadence is the autodetect interval: fast when the ESC answers,
    // slow probe when absent.  The reply is decoded by _escMon in update().
    bool escPollDue(uint32_t now) const {
        return _escPort && _escStream && _escPollLen != 0 &&
               (now - _escLastPollMs >= _escPollIntervalMs);
    }

    void sendEscPoll(uint32_t now) {
        if (!_escPort || !_escStream || _escPollLen == 0) return;
        _escLastPollMs = now;
        _escPolls++;
        _escPort->txEnable();
        _escStream->write(_escPoll, _escPollLen);
        _escStream->flush();                                   // ~1 ms — fits the slot
        _escPort->txDisable();
    }

    /// Send the latest captured channel frame NOW (poll pairing) — uses the
    /// last frame even when not fresh; the radio refreshes it ~100×/s anyway.
    void sendEscChan(uint32_t now) {
        if (!_escChanMirror || !_escPort || !_escStream || _escChanLen == 0) return;
        _escLastChanMs = now;
        _escChanFresh  = false;
        _escChanMirrors++;
        _escPort->txEnable();
        _escStream->write(_escChan, _escChanLen);
        _escStream->flush();
        _escPort->txDisable();
    }

    // Mirror the latest captured CHANNEL frame onto IN_2 — the downstream bus
    // heartbeat that keeps an EX Bus slave's supervision happy (without it a
    // Kolibri mutes a few replies after power-up; see captureEscPoll).  Rate-
    // limited to kEscChanMirrorMs and OPT-IN (setEscChannelMirror): rev A's
    // adjacent IN_1/IN_2 pins crosstalk on IN_2 TX, so only split-pin boards
    // (HubFX rev B: IN_2 TX=GPIO3 far from IN_1 RX=GPIO2) enable it.  Runs in
    // the same post-reply quiet slot as maybePollEsc; the ~3.5 ms flush blocks
    // the task briefly — IN_1 bytes queue in the UART ring meanwhile and the
    // reply timeliness gate (lateSkip) covers the rare overlap with a poll.
    void maybeMirrorEscChan(uint32_t now) {
        if (!_escChanFresh) return;
        // NEVER transmit into the slave's reply window: a telemetry poll
        // GRANTS the downstream bus to the slave for the next few ms — a
        // mirror landing there collides with the reply (bench 2026-07-14).
        if (now - _escLastPollMs < kEscReplyGuardMs) return;
        if (now - _escLastChanMs < kEscChanMirrorMs) return;
        sendEscChan(now);
    }

    // Prolonged IN_1 loss — count the brownout for diag.  ⚠ The actual UART
    // reset/reconnect is DISABLED: re-initialising the port (`configureJetiEx`)
    // from inside the Core-0 IN_1 task that's actively draining that same UART
    // crashed the board (DoubleException, no coredump) the moment IN_1 went dead
    // (Jeti unplugged) — 2026-06-13.  The generic InputDispatcher detection +
    // DOWN signal already cover "the link is gone"; a SAFE re-init must happen
    // OFF this task (queue the request to a worker, or re-begin only when the
    // task is parked) — TODO before re-enabling.  Detection-only for now: a
    // plain unplug recovers on replug regardless.
    void maybeResetRx(uint32_t /*now*/) {
        if (!_rxPort || !_running) return;
        const uint32_t dead = _rxWatch.deadSecs;
        if (dead == 0 || (dead % kRxResetSecs) != 0) return;
        if (dead == _lastRxResetAtDead) return;          // once per second-crossing
        _lastRxResetAtDead = dead;
        ++_rxBrownouts;
        SFX_LOG_WARN("[jexp] IN_1 dead %lus (brownout #%u) — holding outputs",
                     (unsigned long)dead, (unsigned)_rxBrownouts);
        // Prolonged loss: request a full expander restart (UART re-init) at a
        // slower cadence than the brownout log.  Runs on the MAIN LOOP via
        // tickMainLoop — NEVER from this task (the inline configureJetiEx
        // here crashed; a UART can't be torn down from the task reading it).
        // Passive recovery still applies meanwhile: the UART keeps listening,
        // so a plain replug resumes decode without the restart — this is the
        // safety net for a wedged UART/framing state.
        if ((dead % kRxRestartSecs) == 0) {
            _restartReq.store(true, std::memory_order_release);
        }
    }

    /// Full self-restart after prolonged IN_1 loss — main-loop context only
    /// (invoked from tickMainLoop).  Re-runs begin() with the saved
    /// parameter set; on failure the next kRxRestartSecs crossing retries.
    void restartAfterLinkLoss() {
        if (!_running) return;
        auto* rx   = _rxPort;
        auto* esc  = _escPortOrig;
        const uint32_t baud = _baud;
        const bool respond  = _respond;
        const uint16_t usn = _hubUsn, lsn = _hubLsn;
        char name[sizeof(_hubName)];
        std::snprintf(name, sizeof(name), "%s", _hubName);
        SFX_LOG_WARN("[jexp] restarting after link loss (brownout #%u) — UART re-init",
                     (unsigned)_rxBrownouts);
        end();
        if (!begin(rx, esc, usn, lsn, name, baud, respond)) {
            SFX_LOG_ERROR("[jexp] restart FAILED — will retry on the next %lus crossing",
                          (unsigned long)kRxRestartSecs);
        }
    }

    // ESC presence autodetect — called each task pass from update().  PRESENT
    // (fast poll) the moment _escMon decodes a fresh telemetry reply; ABSENT
    // (slow probe) after kEscAbsentMs of silence.  The hub's expireStale drops
    // the device from the radio set on demotion.  Handles hot-plug both ways:
    // a freshly-plugged ESC is caught by the slow probe; an unplugged one ages
    // out.  No config change needed — `downstream:` only enables the link.
    void updateEscPresence(uint32_t now) {
        if (!_escPort) return;
        const uint32_t frames = _escMon.telemetryFrames();
        if (frames != _escLastReplyFrames) {                   // a reply arrived
            _escLastReplyFrames = frames;
            _escLastReplyMs = now;
            if (!_escPresent) {
                _escPresent = true;
                _escPollIntervalMs = kEscActivePollMs;
                SFX_LOG_INFO("[jexp] IN_2 ESC present — telemetry replying (poll %lums)",
                             (unsigned long)kEscActivePollMs);
            }
        } else if (_escPresent && now - _escLastReplyMs > kEscAbsentMs) {
            _escPresent = false;
            _escPollIntervalMs = kEscProbePollMs;
            SFX_LOG_INFO("[jexp] IN_2 ESC absent — back to %lums probe",
                         (unsigned long)kEscProbePollMs);
        }
    }

    /// Map an agnostic hub sensor onto its Jeti EX wire type — the SOLE
    /// point where collection values become Jeti-typed.  Numeric sensors get
    /// the smallest EX integer that fits the current magnitude (the EX data
    /// block carries the type per value, so it may legally vary frame to
    /// frame); Gps/DateTime pass their packed encodings through.  This also
    /// fixes the old fixed-type trap where a producer picked Int6 (5
    /// magnitude bits, |v| <= 31) for a value that later exceeded it.
    static ExDataType exTypeFor(const TelemetryHub::Sensor& s) {
        switch (s.kind) {
            case SensorKind::Gps:      return ExDataType::Gps;
            case SensorKind::DateTime: return ExDataType::DateTime;
            case SensorKind::Int:      break;
        }
        const int32_t v = s.value;
        if (v >= -31 && v <= 31)               return ExDataType::Int6;
        if (v >= -8191 && v <= 8191)           return ExDataType::Int14;
        if (v >= -2097151 && v <= 2097151)     return ExDataType::Int22;
        return ExDataType::Int30;
    }

    // ── Multi-device rotation (cursors advanced under the hub lock) ───
    // Pack SEVERAL active sensor values from ONE device into a single EX data
    // frame (the radio groups by USN/LSN, so a frame stays single-device — the
    // ESC sends its own sensors this same way).  Multiplies telemetry
    // throughput per answered slot WITHOUT raising the emit rate (keeps the
    // ~40 Hz channel-decode guard), so each metric refreshes ~kMaxValuesPerFrame×
    // faster and the radio stops browning out.  Round-robin: pack from _dataSen
    // forward within the current device; when its sensors are exhausted, advance
    // to the next device so no device is starved.
    uint8_t buildData(TelemetryHub& hub, uint8_t* buf) {
        const uint8_t devN = hub.deviceCount();
        if (!devN) return 0;
        // Find a device with ≥1 active sensor (bounded scan; skip empty/inactive).
        const TelemetryHub::Device* d = nullptr;
        for (uint8_t t = 0; t < devN; ++t) {
            const auto* cand = hub.device(_dataDev);
            bool any = false;
            if (cand && cand->active)
                for (uint8_t j = 0; j < cand->sensorCount; ++j)
                    if (cand->sensors[j].active) { any = true; break; }
            if (any) { d = cand; break; }
            _dataDev = (uint8_t)((_dataDev + 1) % devN);
            _dataSen = 0;
        }
        if (!d) return 0;
        if (_dataSen >= d->sensorCount) _dataSen = 0;

        // Head, then pack up to kMaxValuesPerFrame active sensors (buffer-guarded
        // — buf is 40 B; each value ≤5 B + 1 B crc).
        uint8_t pos = exBlockHead(buf, d->usn, d->lsn);
        uint8_t packed = 0;
        while (packed < kMaxValuesPerFrame && _dataSen < d->sensorCount && pos + 5 <= 38) {
            const auto& s = d->sensors[_dataSen++];
            if (!s.active) continue;
            pos += encodeSensorValue(&buf[pos], s.id, exTypeFor(s), s.value, s.decimals);
            ++packed;
        }
        // Exhausted this device's list → next device next time (fair rotation).
        if (_dataSen >= d->sensorCount) { _dataSen = 0; _dataDev = (uint8_t)((_dataDev + 1) % devN); }
        if (packed == 0) return 0;
        buf[1] = 0x40 | (uint8_t)(pos - 1);          // data (0b01) | len
        buf[pos] = crc8_ex(&buf[1], (uint8_t)(pos - 1));
        return (uint8_t)(pos + 1);
    }

    // ── Device condition messages (EX Message packet, v1.07) ─────────
    // A producer bumping a device's msg.seq (e.g. the ESC monitor on a fault
    // CHANGE) gets the text pushed to the radio as an EX Message block with
    // its class (2=warning, 3/4=error) — DC/DS log + popup.  Repeated
    // kMsgRepeats times per seq; a cleared message (empty text) just resets
    // the repeat machinery silently.
    uint8_t buildMessage(TelemetryHub& hub, uint8_t* buf) {
        const uint8_t devN = hub.deviceCount();
        for (uint8_t i = 0; i < devN && i < TelemetryHub::kMaxDevices; ++i) {
            const auto* d = hub.device(i);
            if (!d || !d->active) continue;
            const auto& m = d->msg;
            if (m.seq == 0) continue;                           // never set
            if (m.seq != _msgSeqSent[i]) {                      // new/changed
                _msgSeqSent[i] = m.seq;
                _msgRepeat[i]  = m.text[0] ? kMsgRepeats : 0;   // cleared → silent
            }
            if (!_msgRepeat[i]) continue;
            _msgRepeat[i]--;
            return buildExMessageBlock(buf, d->usn, d->lsn,
                                       /*msgType=*/(uint8_t)(0xE0 | i),
                                       m.cls, m.text);
        }
        return 0;
    }

    uint8_t buildText(TelemetryHub& hub, uint8_t* buf) {
        uint8_t devN = hub.deviceCount();
        if (!devN) return 0;
        const uint16_t maxSlots = (uint16_t)devN * (TelemetryHub::kMaxSensorsPerDevice + 1);
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
    // Saved begin() parameters for the deferred link-loss restart.  _escPortOrig
    // keeps the ORIGINAL downstream port even when ESC bring-up demoted _escPort
    // to null, so a restart re-offers both links.
    sfx_peripherals::InputPort* _escPortOrig = nullptr;
    uint16_t _hubUsn = 0;
    uint16_t _hubLsn = 0;
    char     _hubName[16] = {};
    std::atomic<bool> _restartReq{false};   // set on the IN_1 task, consumed on the main loop
    sfx::Stream*                     _escStream = nullptr;
    uint32_t _baud             = 125000;   // IN_1 baud — kept for UART re-init on loss
    uint32_t _rxBrownouts      = 0;        // IN_1 UART resets performed (diag)
    uint32_t _lastRxResetAtDead = 0;       // deadSecs at the last reset (one per crossing)
    JetiExBus                   _rxBus;
    JetiExTelemetryMonitor      _escMon;

    // Two-way telemetry reply is RUNTIME-gated via `_respond` (set from the
    // attach config — see begin()'s respondTelemetry param), not a compile flag.
    static constexpr uint8_t  kMsgRepeats        = 3;    // per-seq EX Message sends
    uint32_t _msgSeqSent[TelemetryHub::kMaxDevices] = {};
    uint8_t  _msgRepeat [TelemetryHub::kMaxDevices] = {};
    static constexpr uint8_t  kUptimeId          = 1;    // built-in HubFX-own sensor
    // Publish rate limiter.  Each reply carries ONE value (round-robin over the
    // collection), so to refresh every metric TYPE at ~kPerValueHz the emit rate
    // must scale with the active-sensor count N: interval = 1000 / (N·perValueHz),
    // clamped.  The radio consumes ~10 Hz/value (faster is invisible), so 10 Hz is
    // the per-value target.  The MIN interval is the channel-decode guard (don't
    // answer faster than ~40 Hz or the half-duplex reply eats RC headroom); past
    // ~4 metrics each value degrades gracefully below 10 Hz.  The MAX keeps the
    // link warm (≥5 Hz emit) so the Rx never flags "sensor absent" when N is tiny.
    static constexpr uint32_t kPerValueHz        = 10;   // per-metric-type refresh target
    // Emit floor — the RX polls a telemetry slot every ~12 ms (~81 Hz).  Answer
    // essentially EVERY slot so the radio's telemetry-presence watchdog never
    // sees a gap (the "connection drops + reappears" brownout with many
    // sensors).  Was 25 ms (~40 Hz = ~1-in-2 slots), which this radio flagged as
    // unstable.  The timeliness gate still drops a reply when the loop lags, and
    // slotOver/rxErr stay watched — back off toward 16–20 ms if channel decode
    // (IN_1 rxErr/valid) ever degrades under the higher TX duty.
    static constexpr uint32_t kMinReplyIntervalMs = 12;  // ~answer every RX slot (~81 Hz)
    static constexpr uint32_t kMaxReplyIntervalMs = 200; // ≥5 Hz emit (keep the link warm)
    // Values PACKED per EX data frame (same device).  Each answered slot then
    // carries several metrics instead of one, so per-value refresh scales ~Nx
    // without raising the emit rate (no channel-decode risk).  4 → a ≤28-byte
    // frame (~2.5–3 ms reply, fits the master's ~4 ms slot); raise only if
    // `slotOver` stays 0 and `maxUs` has headroom.
    static constexpr uint8_t  kMaxValuesPerFrame = 4;
    // Downstream ESC (IN_2) active master poll + autodetect (Phase 2).  The poll
    // TX is crosstalk-windowed into IN_1's quiet slot (maybePollEsc), so the rate
    // is bounded by the IN_1 reply cadence too.  PROBE while no ESC answers (slow,
    // just enough to catch a hot-plug); ACTIVE once it replies; demote after
    // kEscAbsentMs of silence.
    static constexpr uint32_t kEscProbePollMs    = 200;  // no ESC: hot-plug probe
    static constexpr uint32_t kEscActivePollMs   = 75;   // ESC replying: fresh telemetry
    static constexpr uint32_t kEscAbsentMs       = 600;  // silence → demote to probe
    static constexpr uint32_t kEscChanMirrorMs   = 25;   // downstream channel-frame heartbeat (40 Hz)
    static constexpr uint32_t kEscReplyGuardMs   = 10;   // no mirror TX this long after a poll (slave reply window)
    static constexpr uint32_t kRxResetSecs       = 3;    // brownout log cadence while IN_1 is dead
    static constexpr uint32_t kRxRestartSecs     = 15;   // dead this long → full expander restart (UART re-init), then every 15 s
    // Timeliness-gate slack = master window (~4 ms) − our reply (~2 ms).  If the
    // main loop lagged more than this since the last drain, the parsed poll is
    // too stale to answer in-window — skip (see serveTelemetry()).  Generous at
    // first; watch `lateSkip` vs channel `valid` during a gun event and tighten.
    static constexpr uint32_t kReplyLagBudgetUs  = 1500;
    uint32_t _lastRespMs = 0;            // last Rx-reply time (rate limit)

    // Downstream ESC (IN_2) master-poll + autodetect state (Phase 2).
    uint8_t  _escPoll[16]        = {};   // captured Rx telemetry-request (poll template)
    uint8_t  _escPollLen         = 0;
    uint8_t  _escChan[64]        = {};   // captured Rx channel-data frame (bus heartbeat)
    uint8_t  _escChanLen         = 0;
    bool     _escChanFresh       = false;
    bool     _escChanMirror      = false;   // opt-in (split-pin boards only)
    uint32_t _escLastChanMs      = 0;
    uint32_t _escChanMirrors     = 0;       // diag counter
#if SFX_INSTRUMENTATION
    EscTeeStream _escTee;                   // IN_2 RX hex snapshot (bench diag)
#endif
    uint32_t _escLastPollMs      = 0;
    uint32_t _escPollIntervalMs  = kEscProbePollMs;   // start in probe until a reply
    uint32_t _escLastReplyFrames = 0;
    uint32_t _escLastReplyMs     = 0;
    bool     _escPresent         = false;
    uint32_t _escPolls           = 0;    // diag — IN_2 polls issued
    bool     _respond    = false;        // two-way telemetry reply enabled (runtime, from attach cfg)
    uint32_t _lastTxLog  = 0;            // last [jexp] TX instrumentation log time
    uint8_t  _localDev   = 0xFF;         // hub index of the HubFX-own device

    // Timeliness-gate state (Core: main loop).  _loopLagUs = µs since the prior
    // update(); _maxLoopLagUs = worst seen (diag); _lateSkip = replies dropped
    // because the loop was behind (the gate firing — the gun-event signature).
    uint32_t _lastUpdateUs = 0;
    uint32_t _loopLagUs    = 0;
    uint32_t _maxLoopLagUs = 0;
    uint32_t _lateSkip     = 0;

    // Dedicated IN_1 task (responding mode) — see taskLoop()/begin().
    TaskHandle_t          _task        = nullptr;   // null ⇒ cooperative main-loop decode
    volatile bool         _taskRunning = false;     // shutdown handshake (end() waits on it)
    std::function<bool()> _uploadGate;              // true ⇒ pause UART servicing (Rule 28)

    // Rotation cursors (data + text walk independently across the hub).
    uint16_t _seq     = 0;
    uint8_t  _dataDev = 0, _dataSen = 0;
    uint8_t  _textDev = 0;
    int16_t  _textSen = -1;               // -1 = device name, else sensor index

    bool      _running     = false;     // begin() → true; update() gates on it
    uint32_t  _lastBuiltin = 0;         // 1 Hz uptime-sensor refresh (in update)
    uint32_t  _lastExpire  = 0;         // 1 Hz stale-device expiry (in update)
#if SFX_INSTRUMENTATION
    uint32_t  _lastRxLog   = 0;         // 2 s rx-health diag (in update)
    uint32_t  _lastEscLog  = 0;         // 2 s IN_2 ESC autodetect diag (in update)
#endif
};

}  // namespace JetiEx

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_JETI_EXPANDER_H
