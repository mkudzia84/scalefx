/*
 * LedRuntime<N, TPwm> — implementation.
 *
 * Included at the bottom of led_runtime.h.
 */

#ifndef SFX_LED_RUNTIME_IPP
#define SFX_LED_RUNTIME_IPP

namespace sfx_peripherals {

namespace detail {

/// Convert a wire-format LedEvent to a heap-allocated ILedEvent subclass.
/// Returns nullptr on unknown type — caller must handle.
inline ILedEvent* buildEvent(const ComponentPacket::LedEvent& e) {
    using namespace ComponentPacket;
    const uint32_t duration_ms32 = (uint32_t)e.p1 | ((uint32_t)e.p2 << 16);
    switch (e.type) {
        case LedEventType::ON: {
            // p1|p2 = duration_ms (32-bit), p3 = brightness 0..100,
            // p4 = power-saving flag, p5 = power-saving PWM duty
            const uint8_t bri    = e.p3 > 0 ? e.p3 : 100;
            const bool    saver  = e.p4 != 0;
            const uint8_t saverPct = saver ? (e.p5 ? e.p5 : 78) : 78;
            return new LedOn((uint16_t)duration_ms32, bri, saver, saverPct);
        }
        case LedEventType::OFF:
            return new LedOff((uint16_t)duration_ms32);
        case LedEventType::FLASHING:
            // p1 = period_ms, p2 = duration_ms, p3 = brightness, p4 = duty
            return new LedFlashing(e.p1, e.p2,
                                   e.p3 > 0 ? e.p3 : 100,
                                   e.p4 > 0 ? e.p4 : 50);
        case LedEventType::FADE_IN:
            // p1|p2 = duration_ms, p3 = target brightness
            return new LedFadeIn((uint16_t)duration_ms32,
                                 e.p3 > 0 ? e.p3 : 100);
        case LedEventType::FADE_OUT:
            return new LedFadeOut((uint16_t)duration_ms32,
                                  e.p3 > 0 ? e.p3 : 100);
        case LedEventType::FADING:
            // p1 = period_ms, p2 = duration_ms, p3 = peak, p4 = trough
            return new LedFading(e.p1, e.p2, e.p3,
                                 e.p4 > 0 ? e.p4 : 100);
        case LedEventType::BEACON:
            // p1 = flash_ms, p2 = off_ms, p3 = brightness, p4 = repeat-count
            return new LedBeacon(e.p1, e.p2,
                                 e.p3 > 0 ? e.p3 : 15, e.p5,
                                 e.p4 > 0 ? e.p4 : 100);
        default:
            return nullptr;
    }
}

}  // namespace detail

// ─────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────

template <size_t N, typename TPwm>
void LedRuntime<N, TPwm>::begin(TPwm* backend, const uint8_t (&pins)[N],
                                 bool activeLow, bool pwm) {
    if (_attached) return;
    if (!backend) return;
    for (size_t i = 0; i < N; i++) {
        _channels[i].begin(backend, pins[i], activeLow, pwm);
        _channels[i].off();
        _sequences[i].attachLed(&_channels[i]);
        _enabled[i] = true;
        _queueRunning[i] = false;
    }
    _masterBrightness_pct = 100;
    _attached = true;
}

template <size_t N, typename TPwm>
void LedRuntime<N, TPwm>::end() {
    if (!_attached) return;
    for (size_t i = 0; i < N; i++) {
        _sequences[i].stop();
        _channels[i].off();
        _channels[i].end();
    }
    _attached = false;
}

template <size_t N, typename TPwm>
void LedRuntime<N, TPwm>::allOff() {
    if (!_attached) return;
    for (size_t i = 0; i < N; i++) {
        _sequences[i].stop();
        _channels[i].off();
        _queueRunning[i] = false;
    }
}

template <size_t N, typename TPwm>
void LedRuntime<N, TPwm>::update() {
    if (!_attached) return;
    for (size_t i = 0; i < N; i++) {
        if (_sequences[i].isPlaying()) {
            _sequences[i].update();
        }
        // One-shot completion edge: fire callback once when a non-REPEAT
        // queue reaches its terminal state.  `_queueRunning[i]` was set
        // at loadQueue() time iff REPEAT was clear.
        if (_queueRunning[i] && _sequences[i].isComplete()) {
            _queueRunning[i] = false;
            if (_onQueueDone) _onQueueDone((uint8_t)i);
        }
    }
}

// ─────────────────────────────────────────────────────────────────
// Per-channel ops
// ─────────────────────────────────────────────────────────────────

template <size_t N, typename TPwm>
bool LedRuntime<N, TPwm>::setBrightness(uint8_t idx, uint8_t brightness_0_255) {
    if (idx >= N || !_attached || !_enabled[idx]) return false;
    _sequences[idx].stop();
    _queueRunning[idx] = false;
    // LedControlT::setBrightness takes 0..100.
    const uint8_t pct = (uint8_t)((uint32_t)brightness_0_255 * 100u / 255u);
    _channels[idx].setBrightness(pct);
    return true;
}

template <size_t N, typename TPwm>
bool LedRuntime<N, TPwm>::loadQueue(uint8_t idx, uint8_t flags,
                                    const ComponentPacket::LedEvent* events,
                                    uint8_t count) {
    if (idx >= N || !_attached) return false;
    LedEventSeq& seq = _sequences[idx];
    seq.stop();
    seq.clear();

    for (uint8_t i = 0; i < count; i++) {
        ILedEvent* ev = detail::buildEvent(events[i]);
        if (!ev) return false;             // unknown event type
        if (!seq.add(ev)) {                // sequence buffer full
            delete ev;
            return false;
        }
    }

    const bool repeat = (flags & ComponentPacket::LedQueueFlags::REPEAT) != 0;
    seq.setRepeat(repeat);
    _queueRunning[idx] = !repeat;  // only arm completion edge for one-shot
    return true;
}

template <size_t N, typename TPwm>
bool LedRuntime<N, TPwm>::startQueue(uint8_t idx) {
    if (idx >= N || !_attached || !_enabled[idx]) return false;
    _sequences[idx].start();
    return true;
}

template <size_t N, typename TPwm>
bool LedRuntime<N, TPwm>::stopQueue(uint8_t idx) {
    if (idx >= N || !_attached) return false;
    _sequences[idx].stop();
    _queueRunning[idx] = false;   // master-initiated stop → no DONE event
    return true;
}

template <size_t N, typename TPwm>
bool LedRuntime<N, TPwm>::restartQueue(uint8_t idx) {
    if (idx >= N || !_attached || !_enabled[idx]) return false;
    _sequences[idx].start();      // start() also restarts at event 0
    return true;
}

template <size_t N, typename TPwm>
bool LedRuntime<N, TPwm>::clearQueue(uint8_t idx) {
    if (idx >= N || !_attached) return false;
    _sequences[idx].stop();
    _sequences[idx].clear();
    _queueRunning[idx] = false;
    return true;
}

template <size_t N, typename TPwm>
bool LedRuntime<N, TPwm>::resetChannel(uint8_t idx) {
    if (idx >= N || !_attached) return false;
    _sequences[idx].stop();
    _sequences[idx].clear();
    _channels[idx].off();
    _queueRunning[idx] = false;
    _enabled[idx] = true;
    return true;
}

template <size_t N, typename TPwm>
bool LedRuntime<N, TPwm>::enableChannel(uint8_t idx, bool enabled) {
    if (idx >= N || !_attached) return false;
    if (!enabled) {
        _sequences[idx].stop();
        _channels[idx].off();
        _queueRunning[idx] = false;
    }
    _enabled[idx] = enabled;
    return true;
}

template <size_t N, typename TPwm>
void LedRuntime<N, TPwm>::setMasterBrightness(uint8_t pct) {
    if (pct > 100) pct = 100;
    _masterBrightness_pct = pct;
    // Push to each channel so LedEventSeq emissions honour the scaling.
    for (size_t i = 0; i < N; i++) {
        _channels[i].setMasterBrightness_pct(pct);
    }
}

// ─────────────────────────────────────────────────────────────────
// Queries
// ─────────────────────────────────────────────────────────────────

template <size_t N, typename TPwm>
bool LedRuntime<N, TPwm>::query(uint8_t idx,
                                ComponentPacket::LedChannelStatus& out) const {
    if (idx >= N || !_attached) return false;
    out.addr         = idx;     // caller wraps with LedAddr::dedicated/pwmBorrowed
    // LedControlT.brightness() returns 0..100 — scale up to 0..255 for wire.
    const uint8_t bri_0_100 = _channels[idx].brightness();
    out.brightness   = (uint8_t)((uint32_t)bri_0_100 * 255u / 100u);
    LedEventSeq& seq = const_cast<LedEventSeq&>(_sequences[idx]);
    if (seq.isPlaying())          out.queueState = ComponentPacket::LedQueueState::PLAYING;
    else                           out.queueState = ComponentPacket::LedQueueState::IDLE;
    out.currentEvent = seq.currentIndex();
    return true;
}

template <size_t N, typename TPwm>
bool LedRuntime<N, TPwm>::queueStatus(uint8_t idx,
                                       ComponentPacket::LedQueueStatus& out) const {
    if (idx >= N || !_attached) return false;
    out.addr            = idx;
    const uint8_t bri_0_100 = _channels[idx].brightness();
    out.brightness      = (uint8_t)((uint32_t)bri_0_100 * 255u / 100u);
    LedEventSeq& seq    = const_cast<LedEventSeq&>(_sequences[idx]);
    out.eventCount      = seq.count();
    out.currentEvent    = seq.currentIndex();
    ILedEvent* current  = seq.eventAt(out.currentEvent);
    out.currentType     = current ? current->typeId() : 0;
    out.timeInEvent_ms  = 0;     // TODO: surface from LedEventSeq when it exposes elapsed
    out.timeRemaining_ms = 0;
    out.queueState      = seq.isPlaying() ? ComponentPacket::LedQueueState::PLAYING
                                           : ComponentPacket::LedQueueState::IDLE;
    uint8_t flags = 0;
    // REPEAT flag — LedEventSeq tracks per-channel repeat policy.
    if (_enabled[idx])         flags |= ComponentPacket::LedStatusFlags::ENABLED;
    if (_queueRunning[idx])    /* armed for completion edge */ {}
    else if (seq.count() > 0 && !seq.isPlaying() && seq.isComplete()) {
        flags |= ComponentPacket::LedStatusFlags::COMPLETE_LATCHED;
    }
    out.flags = flags;
    return true;
}

}  // namespace sfx_peripherals

#endif  // SFX_LED_RUNTIME_IPP
