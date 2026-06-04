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

#include <platform/sfx_stream.h>   // sfx::Stream (was <Arduino.h>)
#include <cstdint>

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
               uint32_t baud = 125000, bool useDownstream = false) {
        if (_running) return true;                    // already running
        if (!rxPort) return false;
        _rxPort  = rxPort;
        if (!rxPort->configureJetiEx(baud)) return false;
        sfx::Stream* rxStream = rxPort->uartStream();
        if (!rxStream) return false;

        // Downstream (ESC) link on IN_2.  Brought up ONLY when the operator
        // opts in (`useDownstream`, from /hubfx.yaml's `downstream: true` on the
        // JetiEX input port — default false, exposed in Studio).  When off,
        // draining IN_2 would feed a JetiTelemetryHub that NOTHING reads (with
        // forwarding/reply disabled), and on a floating / noisy IN_2 it is a
        // main-loop STALL source (the unbounded-drain class of bug) — so we
        // leave the port unconfigured + passive.  Channel RX on IN_1 is
        // unaffected either way.
        _escPort   = nullptr;
        _escStream = nullptr;
        if (useDownstream && escPort && escPort->configureJetiEx(baud)) {
            _escStream = escPort->uartStream();
            if (_escStream) { _escPort = escPort; _escMon.begin(_escStream); }
        }

        if (!_rxBus.begin(rxStream)) return false;
        // TELEMETRY DISABLED pending the power/brownout fix (2026-05-30).  The
        // ~3 ms half-duplex TX reply on the shared IN_1 line is electrically
        // marginal while the engine/heater current spike sags the rail, which
        // corrupts the channel RX (rxE floods, ----).  RX-only is immune (rxE=0),
        // so we run pure channel RX with NO reply until the rail is stiffened
        // (separate ESP32 supply / bulk cap / heater soft-start).  Flip
        // kRespondToTelemetry back on once power is fixed.  See project memory
        // "BROWNOUT x half-duplex-TX interaction".
        if (kRespondToTelemetry) {
            _rxBus.setTxPort(rxPort);                 // half-duplex TX on IN_1 (reply)
            _rxBus.onTelemetryRequest([this](uint8_t pkt){ serveTelemetry(pkt); });
        }
        // Forward-to-ESC mirrors the Rx's polls out IN_2 — its second-UART TX
        // crosstalks GPIO6->GPIO5 onto the IN_1 channel RX.  Gated off so IN_2
        // stays a passive downstream port (no telemetry forwarding between the
        // links for now).  Re-enable via the windowed-forward approach later.
        if (kForwardToEsc && _escPort) {
            _rxBus.onRawFrame([this](const uint8_t* f, uint8_t l){ forwardToEsc(f, l); });
        }

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
        SFX_LOG_INFO("[jexp] started in main loop (rx=IN_1, esc=IN_2%s, baud=%lu)",
                     _escPort ? "" : " off", (unsigned long)baud);
        return true;
    }

    // Drive one decode pass — called every main-loop iteration from
    // JetiExInputRole::tick().  Drains IN_2 (ESC monitor) then IN_1 (channels +,
    // when telemetry is enabled, the reply hook), and refreshes built-in
    // sensors.  No-op until begin() / after end().
    void update() {
        if (!_running) return;
        const uint32_t now = SFX_MILLIS();
        // IN_2 first: drain the ESC's telemetry into the hub before IN_1
        // re-parses the next echoed master frame.  Only when the downstream
        // link is in use (otherwise _escPort is null — see begin()).  ALWAYS
        // drain (bounded) — the monitor only watches health, never gates.
        if (_escPort) {
            _escMon.update(now);
            _escWatch.observe(now, _escMon.rxByteCount(), _escMon.frameCount(), "IN_2");
        }
        _rxBus.update();            // IN_1: master frames → channels (+ reply hook)
        _rxWatch.observe(now, _rxBus.rxByteCount(), _rxBus.rxFrameCount(), "IN_1");

#if SFX_INSTRUMENTATION
        // Concise rx health (every 2 s): bytes = line active? frames = decoding?
        // err = CRC failures (wrong baud / unhandled frame types / noise);
        // ch/valid = channel decode state.  The "mark what is wrong" counters.
        if (now - _lastRxLog >= 2000) {
            _lastRxLog = now;
            SFX_LOG_DEBUG("[jexp] IN_1 rxB=%lu rxF=%lu rxErr=%lu ch=%u valid=%d%s",
                         (unsigned long)_rxBus.rxByteCount(),
                         (unsigned long)_rxBus.rxFrameCount(),
                         (unsigned long)_rxBus.rxErrorCount(),
                         _rxBus.channelCount(), _rxBus.isValid() ? 1 : 0,
                         _rxWatch.noisy() ? " NOISY" : "");
        }
#endif

        if (now - _lastBuiltin >= 1000 && _localDev != 0xFF) {
            _lastBuiltin = now;
            auto& hub = JetiTelemetryHub::instance();
            JetiTelemetryHub::ScopedLock lk(hub);
            hub.setSensor(_localDev, kUptimeId, ExDataType::Int22, 0,
                          (int32_t)(now / 1000), now);
        }
        if (now - _lastExpire >= 1000) {
            _lastExpire = now;
            auto& hub = JetiTelemetryHub::instance();
            JetiTelemetryHub::ScopedLock lk(hub);
            hub.expireStale(now, 2000);   // disconnected ESC drops out
        }
    }

    void end() {
        _running = false;             // update() becomes a no-op
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

    /// True while a link is active (bytes flowing) but NOT decoding valid
    /// frames — surfaced in diag so the operator sees WHY channels are dead
    /// (noisy line / wrong baud / unhandled frame types).  The drain keeps
    /// running regardless (we never give up on catching a real frame).
    bool rxLinkNoisy()  const { return _rxWatch.noisy(); }
    bool escLinkNoisy() const { return _escWatch.noisy(); }
    uint32_t rxNoiseSecs() const { return _rxWatch.totalNoiseSecs; }

private:
    JetiExpander() = default;
    JetiExpander(const JetiExpander&) = delete;
    JetiExpander& operator=(const JetiExpander&) = delete;

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
        bool     healthy = false;     // valid frames decoded in the last window
        bool     primed  = false;     // seen at least one window

        void reset(uint32_t now) {
            lastMs = now; baseBytes = baseFrames = 0;
            noiseSecs = totalNoiseSecs = 0; healthy = false; primed = false;
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

    // Raw-frame hook: poll the downstream ESC by mirroring the Rx's TELEMETRY-
    // REQUEST frames to it (half-duplex).  Rate-limited so the ESC is polled at
    // ~kEscPollIntervalMs, NOT once per Rx frame: mirroring every frame blocks
    // the task on flush() for ~3 ms each and starves IN_1's channel RX (signal
    // loss).  Only 0x3A frames matter — channel frames don't make the ESC reply.
    //
    // NOTE: this forwards Rx->ESC only; the reverse CONFIG-relay (ESC config /
    // menu replies -> Rx) is NOT IMPLEMENTED YET (deferred — proprietary format,
    // synchronous-proxy timing).  The monitor extracts the ESC's 0x3A telemetry;
    // its config/menu replies are dropped.
    void forwardToEsc(const uint8_t* frame, uint8_t len) {
        if (!_escPort || !_escStream) return;
        if (len < 6 || frame[4] != DATA_TELEMETRY) return;     // telemetry polls only
        const uint32_t now = SFX_MILLIS();
        if (now - _lastFwdMs < kEscPollIntervalMs) return;     // rate-limit
        _lastFwdMs = now;
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
    sfx::Stream*                     _escStream = nullptr;
    JetiExBus                   _rxBus;
    JetiExTelemetryMonitor      _escMon;

    // The forward-to-ESC TX on IN_2 (GPIO6) couples onto the ADJACENT IN_1
    // (GPIO5) channel RX and drops the RC signal — confirmed: the IN_1 reply is
    // clean, the "second-UART piping" is what hurts.  Off until the IN_2 poll is
    // timed into IN_1's silent window (or routed off the adjacent pin).  The
    // IN_1 telemetry RESPONSE is unaffected and stays on.
    static constexpr bool     kForwardToEsc       = false;
    // Telemetry reply DISABLED pending the power/brownout fix — the half-duplex
    // TX on the shared IN_1 line corrupts channel RX while engine/heater sag the
    // rail.  RX-only is immune (rxE=0).  Flip to true once power is stiffened.
    static constexpr bool     kRespondToTelemetry = false;
    static constexpr uint8_t  kUptimeId          = 1;    // built-in HubFX-own sensor
    static constexpr uint32_t kEscPollIntervalMs = 100;  // ESC poll rate (~10 Hz)
    static constexpr uint32_t kRespIntervalMs    = 50;   // Rx reply rate cap (~20 Hz)
    uint32_t _lastFwdMs  = 0;            // last ESC-forward time (rate limit)
    uint32_t _lastRespMs = 0;            // last Rx-reply time (rate limit)
    uint8_t  _localDev   = 0xFF;         // hub index of the HubFX-own device

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
