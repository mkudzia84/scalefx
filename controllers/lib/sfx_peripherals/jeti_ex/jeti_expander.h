/*
 * jeti_expander.h — JetiExpander
 *
 * Jeti EX Bus telemetry EXPANDER (the spec's expander topology, page 1).  A
 * board-unique singleton (Rule 14) that bridges two half-duplex Jeti links and
 * runs on a dedicated Core-0 FreeRTOS task (above the Arduino loopTask) so the
 * time-critical ~4 ms response slots are never blocked by the soft main loop —
 * and never touch the hard-real-time audio on Core 1.
 *
 *   IN_1 (Rx side, we are SLAVE): answer the receiver's telemetry polls with
 *   the MERGED multi-device set from the hub (half-duplex TX), and decode RC
 *   channels.
 *
 * The TelemetryHub is the shared merge point.  Devices keep their identity,
 * so the radio shows every producer (HubFx-own sensors, native ESC telemetry
 * from the esc-telemetry role, ...) under its own name.
 *
 * (The former IN_2 downstream EX-Bus master link — polling an ESC as a Jeti
 * slave with mirrored channel frames — was REMOVED 2026-07-15: native ESC
 * telemetry supersedes it, and its ~3.5 ms mirror TX every 25 ms on this task
 * was a standing input-latency tax.  See telemetry/esc/ for the decoders.)
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
#include <cstring>                 // memset (discovery caches)
#include <functional>

#include <ports/input_port.h>
#include <serial/diag_log.h>
#include "jeti_ex_bus.h"
#include <telemetry/telemetry_hub.h>

namespace JetiEx {

using sfx_telemetry::TelemetryHub;
using sfx_telemetry::SensorKind;


class JetiExpander {
public:
    static JetiExpander& instance() {
        static JetiExpander inst;            // C++11 thread-safe static local
        return inst;
    }

    /// Start the expander on the Rx-facing link (IN_1).  Registers the
    /// HubFX-own device (usn/lsn/name) as a local hub device, wires the
    /// responder hook, and spawns the Core-0 task.  Idempotent.
    bool begin(sfx_peripherals::InputPort* rxPort,
               uint16_t hubUsn, uint16_t hubLsn, const char* hubName,
               uint32_t baud = 125000,
               bool respondTelemetry = false) {
        if (_running) return true;                    // already running
        if (!rxPort) return false;
        _rxPort  = rxPort;
        _baud    = baud;                              // kept for UART re-init on loss
        // Save the full begin() parameter set so the deferred link-loss
        // restart (maybeResetRx -> tickMainLoop) can re-run begin() verbatim.
        _hubUsn = hubUsn;
        _hubLsn = hubLsn;
        std::snprintf(_hubName, sizeof(_hubName), "%s", hubName ? hubName : "HubFx");
        if (!rxPort->configureJetiEx(baud)) return false;
        sfx::Stream* rxStream = rxPort->uartStream();
        if (!rxStream) return false;

        if (!_rxBus.begin(rxStream)) return false;
        // Fresh link monitors — the new bus counters start at zero, and stale
        // baselines from a previous run would read as a wrap (spurious NOISY)
        // and keep deadSecs climbing across a restart.
        _rxWatch  = LinkMonitor{};
        _lastRxResetAtDead = 0;
        std::memset(_typeRank, 0, sizeof _typeRank);      // fresh discovery, fresh widths
        std::memset(_msgSeqSent, 0, sizeof _msgSeqSent);
        std::memset(_msgRepeat, 0, sizeof _msgRepeat);
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
        SFX_LOG_INFO("[jexp] started (rx=IN_1%s, baud=%lu)",
                     _task ? " task" : " loop", (unsigned long)baud);
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
    // IN_1 (channels + the reply hook) and refreshes built-in sensors.
    // No-op until begin()/end().
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
            // Corruption ground truth: hex of the FIRST CRC-failed frame since
            // the last window.  Read against a healthy channel frame
            // (3E 03 28 <id> 31 20 <16 x u16LE> crc16): missing bytes = ISR/
            // ring drop, bit garbage = electrical, shifted = framing/parser.
            uint8_t ff[64];
            const uint8_t fn = _rxBus.takeFailedFrame(ff, sizeof ff);
            if (fn) {
                char hex[129];
                for (uint8_t i = 0; i < fn && i < 64; ++i)
                    std::snprintf(&hex[i * 2], 3, "%02X", ff[i]);
                SFX_LOG_INFO("[jexp] IN_1 failed-frame[%u]: %s", (unsigned)fn, hex);
            }
        }
        // Two-way reply health (every 2 s, only while responding): polls vs resp
        // = reply coverage; echoShort + slotOver = TX/RX-turnaround issues;
        // lastUs/maxUs = did the reply fit the ~4 ms slot.  Cross-check rxErr
        // above — if it climbs as resp climbs, the TX reply is corrupting RX.
        if (_respond && now - _lastTxLog >= 2000) {
            // MEASURED reply rate over the window (the "how fast does
            // telemetry actually trigger back" number): replies/s and packed
            // data values/s.  Compare against the target interval
            // (replyIntervalMs) to spot gating/beat losses.
            const uint32_t rNow = _rxBus.txResponseCount();
            const uint32_t dtMs = now - _lastTxLog;   // ~2000
            const uint32_t respHz = dtMs ? ((rNow - _lastRespCount) * 1000u) / dtMs : 0;
            const uint32_t valsPs = dtMs ? ((_dataValsSent - _lastValsSent) * 1000u) / dtMs : 0;
            _lastRespCount = rNow;
            _lastValsSent  = _dataValsSent;
            _lastTxLog = now;
            SFX_LOG_INFO("[jexp] TX polls=%lu resp=%lu respHz=%lu vals/s=%lu target=%lums lateSkip=%lu echoShort=%lu slotOver=%lu lastUs=%lu maxUs=%lu loopLagUs=%lu maxLoopLagUs=%lu",
                         (unsigned long)_rxBus.pollsSeen(), (unsigned long)rNow,
                         (unsigned long)respHz, (unsigned long)valsPs,
                         (unsigned long)replyIntervalMs(),
                         (unsigned long)_lateSkip,
                         (unsigned long)_rxBus.echoShort(), (unsigned long)_rxBus.slotOverruns(),
                         (unsigned long)_rxBus.lastTxDurUs(), (unsigned long)_rxBus.maxTxDurUs(),
                         (unsigned long)_loopLagUs, (unsigned long)_maxLoopLagUs);
            _maxLoopLagUs = 0;   // reset the 2 s window peak
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
            hub.expireStale(now, 2000);   // a silent producer's device drops out
        }
    }

    /// Pause the IN_1 task's UART servicing while this predicate is true
    /// (Rule 28 upload exclusivity).  The prior Core-0 Jeti task starved the
    /// upload pipeline precisely because it had no such gate; the sketch wires
    /// this to the storage policy's isUploadActive().
    void setUploadGate(std::function<bool()> fn) { _uploadGate = std::move(fn); }

    void end() {
        _running = false;             // task loop + update() become no-ops
        if (_task) {                  // wait for the task to exit before teardown
            for (int i = 0; i < 200 && _taskRunning; ++i) vTaskDelay(pdMS_TO_TICKS(1));
            _task = nullptr;
        }
        _rxBus.end();
        if (_rxPort) _rxPort->disable();
        _rxPort = nullptr;
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

    // Telemetry hook (runs during update()'s _rxBus.update()): build the next
    // multi-device frame from the hub (under lock), then half-duplex TX it.
    void serveTelemetry(uint8_t pktId) {
        // Rate-limit replies (~30 Hz).  Each reply is a half-duplex TX turnaround
        // on IN_1; replying to EVERY poll leaves too little channel-RX headroom,
        // and the odd late reply overruns the slot and corrupts a frame (RC
        // signal gaps).  The radio tolerates skipped polls (it just asks again);
        // telemetry still updates smoothly.
        const uint32_t now = SFX_MILLIS();
        // Beat-tolerant gate: the master polls every ~11.6 ms while the floor
        // interval is 12 ms — a strict >= interval check skips almost every
        // OTHER poll (measured 48 Hz replies vs 86 Hz polls, halving sensor
        // refresh).  Allow a 3 ms early margin so consecutive polls stay
        // eligible; the interval still bounds the average rate.
        const uint32_t iv = replyIntervalMs();
        if (now - _lastRespMs + 3 < iv) return;

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
        const uint32_t baud = _baud;
        const bool respond  = _respond;
        const uint16_t usn = _hubUsn, lsn = _hubLsn;
        char name[sizeof(_hubName)];
        std::snprintf(name, sizeof(name), "%s", _hubName);
        SFX_LOG_WARN("[jexp] restarting after link loss (brownout #%u) — UART re-init",
                     (unsigned)_rxBrownouts);
        end();
        if (!begin(rx, usn, lsn, name, baud, respond)) {
            SFX_LOG_ERROR("[jexp] restart FAILED — will retry on the next %lus crossing",
                          (unsigned long)kRxRestartSecs);
        }
    }

    /// Map an agnostic hub sensor onto its Jeti EX wire type — the SOLE
    /// point where collection values become Jeti-typed.  Numeric sensors get
    /// the smallest EX integer that fits the current magnitude, with TWO
    /// stability rules for the radio's persistent sensor database:
    ///   (1) floor at Int14 — an idle value of 0 must not flap to Int6 and
    ///       back as the metric comes alive;
    ///   (2) STICKY WIDENING — once a sensor has been sent wider, it never
    ///       narrows again (per-device/per-slot rank cache, reset with the
    ///       expander).  This also keeps the fix for the old fixed-type trap
    ///       (a producer picking Int6 for a value that later exceeded it).
    /// Gps/DateTime pass their packed encodings through.
    ExDataType exTypeFor(uint8_t devIdx, uint8_t senIdx, const TelemetryHub::Sensor& s) {
        switch (s.kind) {
            case SensorKind::Gps:      return ExDataType::Gps;
            case SensorKind::DateTime: return ExDataType::DateTime;
            case SensorKind::Int:      break;
        }
        const int32_t v = s.value;
        uint8_t rank = 0;                                       // Int14 floor
        if (v < -8191 || v > 8191)         rank = 1;            // Int22
        if (v < -2097151 || v > 2097151)   rank = 2;            // Int30
        if (devIdx < TelemetryHub::kMaxDevices &&
            senIdx < TelemetryHub::kMaxSensorsPerDevice) {
            uint8_t& seen = _typeRank[devIdx][senIdx];
            if (rank > seen) seen = rank;
            rank = seen;
        }
        return rank == 0 ? ExDataType::Int14
             : rank == 1 ? ExDataType::Int22
                         : ExDataType::Int30;
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

        // Head, then pack up to kMaxValuesPerFrame active sensors.  Guard on
        // the EX SPEC cap — the whole telegram (head + values + crc8) must
        // stay <= 29 B: after a worst-case 5 B value, pos <= 28, +1 crc = 29.
        uint8_t pos = exBlockHead(buf, d->usn, d->lsn);
        uint8_t packed = 0;
        while (packed < kMaxValuesPerFrame && _dataSen < d->sensorCount && pos + 5 <= 28) {
            const uint8_t senIdx = _dataSen;
            const auto& s = d->sensors[_dataSen++];
            if (!s.active) continue;
            pos += encodeSensorValue(&buf[pos], s.id, exTypeFor(_dataDev, senIdx, s), s.value, s.decimals);
            ++packed;
            ++_dataValsSent;
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
    uint16_t _hubUsn = 0;
    uint16_t _hubLsn = 0;
    char     _hubName[16] = {};
    std::atomic<bool> _restartReq{false};   // set on the IN_1 task, consumed on the main loop
    uint32_t _baud             = 125000;   // IN_1 baud — kept for UART re-init on loss
    uint32_t _rxBrownouts      = 0;        // IN_1 UART resets performed (diag)
    uint32_t _lastRxResetAtDead = 0;       // deadSecs at the last reset (one per crossing)
    JetiExBus                   _rxBus;

    // Two-way telemetry reply is RUNTIME-gated via `_respond` (set from the
    // attach config — see begin()'s respondTelemetry param), not a compile flag.
    static constexpr uint8_t  kMsgRepeats        = 3;    // per-seq EX Message sends
    uint32_t _msgSeqSent[TelemetryHub::kMaxDevices] = {};
    uint8_t  _msgRepeat [TelemetryHub::kMaxDevices] = {};
    // Sticky per-sensor EX width rank (0=Int14,1=Int22,2=Int30) — see exTypeFor.
    uint8_t  _typeRank[TelemetryHub::kMaxDevices][TelemetryHub::kMaxSensorsPerDevice] = {};
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
    // without raising the emit rate (no channel-decode risk).  6 packs the EX
    // telegram to its spec cap (29 B total — the buildData loop guard) for
    // ~1.5x more values per RF-relayed frame; raised 4 → 6 on 2026-08-12 when
    // the JIVE PRO device pushed the sensor count to 17 and the radio's
    // per-sensor refresh dropped to ~1/s (intermittent LOST flags).  Bench
    // preconditions held: slotOver=0, maxUs=2565 in the ~4 ms slot.
    static constexpr uint8_t  kMaxValuesPerFrame = 6;
    static constexpr uint32_t kRxResetSecs       = 3;    // brownout log cadence while IN_1 is dead
    static constexpr uint32_t kRxRestartSecs     = 15;   // dead this long → full expander restart (UART re-init), then every 15 s
    // Timeliness-gate slack = master window (~4 ms) − our reply (~2 ms).  If the
    // main loop lagged more than this since the last drain, the parsed poll is
    // too stale to answer in-window — skip (see serveTelemetry()).  Generous at
    // first; watch `lateSkip` vs channel `valid` during a gun event and tighten.
    static constexpr uint32_t kReplyLagBudgetUs  = 1500;
    uint32_t _lastRespMs = 0;            // last Rx-reply time (rate limit)

    bool     _respond    = false;        // two-way telemetry reply enabled (runtime, from attach cfg)
    uint32_t _lastTxLog  = 0;            // last [jexp] TX instrumentation log time
    uint8_t  _localDev   = 0xFF;         // hub index of the HubFX-own device

    // Timeliness-gate state (Core: main loop).  _loopLagUs = µs since the prior
    // update(); _maxLoopLagUs = worst seen (diag); _lateSkip = replies dropped
    // because the loop was behind (the gate firing — the gun-event signature).
    uint32_t _lastUpdateUs = 0;
    uint32_t _lastRespCount = 0;   // respHz window baseline
    uint32_t _dataValsSent  = 0;   // packed data values total
    uint32_t _lastValsSent  = 0;
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
#endif
};

}  // namespace JetiEx

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_JETI_EXPANDER_H
