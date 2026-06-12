/*
 * LedAnimator implementation — single source of truth for LED event
 * behaviour. Wire format mirrors the LightFx master-side `LightEvent`
 * 10-byte record; see led_animator.h for the field-by-field layout.
 */

#include "led_animator.h"
#include <platform/sfx_platform.h>   // SFX_MILLIS()

#include <cstring>
#include <math.h>

#include <serial/diag_log.h>

namespace sfx_core {

namespace {

// Cyclic, self-repeating event kinds — Flash / Fading / Beacon — derive
// their brightness purely from the absolute clock (`now % cycle_ms`), so
// every channel running the same cycle stays phase-locked regardless of
// where it sits in a loop pattern.  Shared by BOTH tick paths: the
// non-loop state machine AND the phase-locked loop sampler (which used to
// omit these kinds, rendering a looping Fading/Flash/Beacon as a SOLID
// `brightnessPct` — the breathing/strobe preset that never breathed).
uint8_t cyclicSample(const LedAnimator::Event& e, uint32_t now) {
    switch (e.kind) {
        case LedAnimator::EV_FLASH: {
            // Square wave: brightnessPct for flashPct% of cycle, else 0.
            if (e.cycleMs == 0) return e.brightnessPct;     // degenerate ⇒ steady on
            const uint32_t phase    = now % e.cycleMs;
            const uint32_t highSpan  = ((uint32_t)e.cycleMs * (uint32_t)e.flashPct) / 100u;
            return (phase < highSpan) ? e.brightnessPct : 0;
        }
        case LedAnimator::EV_FADING: {
            // Sinusoid minPct..maxPct via (1 - cos(2π·t/T)) / 2.
            if (e.cycleMs == 0) return e.maxPct;
            const float    t    = (float)(now % e.cycleMs) / (float)e.cycleMs;
            const float    norm = 0.5f * (1.0f - cosf(6.2831853f * t));
            const int32_t  span = (int32_t)e.maxPct - (int32_t)e.minPct;
            return (uint8_t)((int32_t)e.minPct + (int32_t)(norm * (float)span + 0.5f));
        }
        case LedAnimator::EV_BEACON: {
            // At maxPct for flashPct% of cycle, else minPct.
            if (e.cycleMs == 0) return e.maxPct;
            const uint32_t phase    = now % e.cycleMs;
            const uint32_t highSpan  = ((uint32_t)e.cycleMs * (uint32_t)e.flashPct) / 100u;
            return (phase < highSpan) ? e.maxPct : e.minPct;
        }
        default:
            return e.brightnessPct;
    }
}

}  // namespace

bool LedAnimator::loadQueue(const Event* events, size_t count) {
    if (count > MAX_EVENTS) return false;
    stop();
    if (count > 0) std::memcpy(_queue, events, count * sizeof(Event));
    _count  = (uint8_t)count;
    _cursor = 0;

    // Phase-locked loop pattern? (flag carried on event[0]).  Precompute
    // the cycle period = Σ duration_ms so tick() can index by now % period.
    _loop = (count > 0) && (_queue[0].flags & FLAG_LOOP);
    _suppressDone = (count > 0) && (_queue[0].flags & FLAG_NO_DONE);
    _loopPeriodMs = 0;
    if (_loop) {
        for (uint8_t i = 0; i < _count; ++i) _loopPeriodMs += _queue[i].durationMs;
    }
    SFX_LOG_DEBUG("[LedAnimator] loadQueue: %u events%s",
                  (unsigned)count, _loop ? " (loop)" : "");
    return true;
}

void LedAnimator::start() {
    if (_count == 0 || !_port) return;
    _cursor           = 0;
    _eventStart_ms    = SFX_MILLIS();
    _playing          = true;
    _currentBrightPct = 0;
    const Event& e0   = _queue[0];

    // Prime per-event state.
    switch (e0.kind) {
        case EV_FADE_IN:
            _fadeFromPct = 0;
            _fadeToPct   = e0.brightnessPct;
            break;
        case EV_FADE_OUT:
            _fadeFromPct = e0.brightnessPct;
            _fadeToPct   = 0;
            break;
        default:
            break;
    }
    SFX_LOG_DEBUG("[LedAnimator] start: kind=%u dur=%u cycle=%u bright=%u",
                  (unsigned)e0.kind, (unsigned)e0.durationMs,
                  (unsigned)e0.cycleMs, (unsigned)e0.brightnessPct);
    tick(SFX_MILLIS());   // emit an initial sample so the port goes live immediately
}

void LedAnimator::stop() {
    _playing          = false;
    _currentBrightPct = 0;
    if (_port) writeOutputPct(0);
}

void LedAnimator::tick(uint32_t now) {
    if (!_playing || _count == 0 || !_port) return;

    // ── Phase-locked loop mode ────────────────────────────────────────
    // The whole queue is one repeating cycle; the active segment is
    // chosen by absolute `now % period`, so every looping channel with
    // the same period stays in lockstep and never drifts.  Arbitrary
    // patterns (single flash, double flash, mutually-offset pulses) are
    // just a sequence of timed On/Off (and fade) events.
    if (_loop) {
        if (_loopPeriodMs == 0) {
            // No segment durations sum > 0 — the classic case is a SINGLE
            // run-forever cyclic event (`{ kind: fading/beacon/flash,
            // cycle_ms: T }` with duration_ms 0) on a `loop: true` track,
            // which Studio's beacon/strobe/breathing presets emit.  There's
            // no time-windowed queue to walk, so render event[0] directly;
            // cyclic kinds still animate on their own `cycle_ms`.  (Without
            // this the whole looping channel renders DARK — the "breathing
            // / sinusoidal beacon does nothing" bug.)
            const Event& e = _queue[0];
            writeOutputPct(e.kind == EV_OFF ? 0 : cyclicSample(e, now));
            return;
        }
        const uint32_t pos = now % _loopPeriodMs;
        uint32_t acc = 0;
        for (uint8_t i = 0; i < _count; ++i) {
            const Event& e = _queue[i];
            if (pos < acc + e.durationMs) {
                const uint32_t local = pos - acc;          // ms into this segment
                uint8_t b;
                switch (e.kind) {
                    case EV_OFF:      b = 0; break;
                    case EV_FADE_IN:  b = (e.durationMs ? (uint8_t)((uint32_t)e.brightnessPct * local / e.durationMs) : e.brightnessPct); break;
                    case EV_FADE_OUT: b = (e.durationMs ? (uint8_t)((uint32_t)e.brightnessPct * (e.durationMs - local) / e.durationMs) : 0); break;
                    // Cyclic kinds sample from the absolute clock (phase-locked),
                    // NOT the loop segment — a looping Fading/Flash/Beacon must
                    // animate, not freeze at brightnessPct.
                    case EV_FLASH:
                    case EV_FADING:
                    case EV_BEACON:   b = cyclicSample(e, now); break;
                    case EV_ON:
                    default:          b = e.brightnessPct; break;   // On (and any non-fade) = solid
                }
                _currentBrightPct = b;
                writeOutputPct(b);
                return;
            }
            acc += e.durationMs;
        }
        // pos < period guarantees a hit; fall through only on degenerate data.
        writeOutputPct(0);
        return;
    }

    const Event&   ev  = _queue[_cursor];
    // `now` is the shared, once-per-pass clock (see header).  `dt` (time
    // since this event started) drives the per-event DURATION; cyclic
    // PHASE uses absolute `now % cycle_ms` so channels stay phase-locked.
    const uint32_t dt  = now - _eventStart_ms;
    bool advanceQueue  = false;

    switch (ev.kind) {
        case EV_ON: {
            _currentBrightPct = ev.brightnessPct;
            writeOutputPct(_currentBrightPct);
            // durationMs == 0 ⇒ terminal: hold this brightness forever
            // (don't advance). Otherwise advance after dur_ms.
            if (ev.durationMs != 0 && dt >= ev.durationMs) advanceQueue = true;
            break;
        }
        case EV_OFF: {
            _currentBrightPct = 0;
            writeOutputPct(0);
            if (ev.durationMs != 0 && dt >= ev.durationMs) advanceQueue = true;
            break;
        }
        case EV_FADE_IN: {
            if (ev.durationMs == 0 || dt >= ev.durationMs) {
                _currentBrightPct = _fadeToPct;
                writeOutputPct(_currentBrightPct);
                advanceQueue = (ev.durationMs != 0);   // hold final if terminal
            } else {
                const int32_t delta = (int32_t)_fadeToPct - (int32_t)_fadeFromPct;
                const int32_t step  = (delta * (int32_t)dt) / (int32_t)ev.durationMs;
                _currentBrightPct   = (uint8_t)((int32_t)_fadeFromPct + step);
                writeOutputPct(_currentBrightPct);
            }
            break;
        }
        case EV_FADE_OUT: {
            if (ev.durationMs == 0 || dt >= ev.durationMs) {
                _currentBrightPct = _fadeToPct;
                writeOutputPct(_currentBrightPct);
                advanceQueue = (ev.durationMs != 0);
            } else {
                const int32_t delta = (int32_t)_fadeToPct - (int32_t)_fadeFromPct;
                const int32_t step  = (delta * (int32_t)dt) / (int32_t)ev.durationMs;
                _currentBrightPct   = (uint8_t)((int32_t)_fadeFromPct + step);
                writeOutputPct(_currentBrightPct);
            }
            break;
        }
        case EV_FLASH:
        case EV_FADING:
        case EV_BEACON: {
            // Cyclic kinds — square wave / sinusoid / beacon.  Phase comes
            // from the shared absolute clock (`now % cycle_ms`) so channels
            // stay locked; `dt` only governs when the event hands off.
            _currentBrightPct = cyclicSample(ev, now);
            writeOutputPct(_currentBrightPct);
            if (ev.durationMs != 0 && dt >= ev.durationMs) advanceQueue = true;
            break;
        }
        default:
            // Unknown opcode — skip to keep the queue moving.
            advanceQueue = true;
            break;
    }

    if (advanceQueue) advance();
}

void LedAnimator::advance() {
    _cursor++;
    if (_cursor >= _count) {
        _playing          = false;
        _currentBrightPct = 0;
        writeOutputPct(0);
        SFX_LOG_DEBUG("[LedAnimator] queue done (%u events played)", (unsigned)_count);
        if (_onDone && !_suppressDone) _onDone();   // fire-and-forget blinks stay silent
        return;
    }
    _eventStart_ms = SFX_MILLIS();
    const Event& next = _queue[_cursor];
    // Prime per-event state.
    switch (next.kind) {
        case EV_FADE_IN:
            _fadeFromPct = _currentBrightPct;
            _fadeToPct   = next.brightnessPct;
            break;
        case EV_FADE_OUT:
            _fadeFromPct = _currentBrightPct;
            _fadeToPct   = 0;
            break;
        default:
            break;
    }
    SFX_LOG_DEBUG("[LedAnimator] advance to event %u: kind=%u dur=%u",
                  (unsigned)_cursor, (unsigned)next.kind, (unsigned)next.durationMs);
}

void LedAnimator::writeOutputPct(uint8_t brightnessPct) {
    if (!_port) return;
    if (brightnessPct > 100) brightnessPct = 100;
    // bright_pct × master_pct ⇒ scaled_pct (0..100)
    // scaled_pct ⇒ duty: pct * maxDuty / 100
    const uint32_t scaledPct = ((uint32_t)brightnessPct *
                                 (uint32_t)_masterBrightnessPct) / 100u;
    const uint32_t duty      = (scaledPct * (uint32_t)_port->maxDuty()) / 100u;
    _port->setDuty((uint16_t)duty);
}

}  // namespace sfx_core
