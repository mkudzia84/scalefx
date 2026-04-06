/*
 * LED Manager — Template Implementation
 *
 * Included at the bottom of led_manager.h.
 */

#ifndef LED_MANAGER_IPP
#define LED_MANAGER_IPP

// ============================================================================
// Operations
// ============================================================================

template <uint8_t N, typename G>
uint8_t LedManager<N, G>::ledSet(uint8_t ch, uint8_t brightness) {
    if (!_enabled[ch - 1]) return LightFxError::CHANNEL_DISABLED;
    _sequences[ch - 1].stop();
    _channels[ch - 1].setBrightness(brightness);
    return LightFxError::OK;
}

template <uint8_t N, typename G>
uint8_t LedManager<N, G>::ledOff(uint8_t ch) {
    if (ch == 0) {
        for (uint8_t i = 0; i < N; i++) {
            if (!_enabled[i]) continue;
            _sequences[i].stop();
            _channels[i].off();
        }
    } else {
        if (!_enabled[ch - 1]) return LightFxError::CHANNEL_DISABLED;
        _sequences[ch - 1].stop();
        _channels[ch - 1].off();
    }
    return LightFxError::OK;
}

template <uint8_t N, typename G>
uint8_t LedManager<N, G>::seqClear(uint8_t ch) {
    if (ch == 0) {
        for (uint8_t i = 0; i < N; i++) {
            if (!_enabled[i]) continue;
            _sequences[i].stop();
            _sequences[i].clear();
        }
    } else {
        if (!_enabled[ch - 1]) return LightFxError::CHANNEL_DISABLED;
        _sequences[ch - 1].stop();
        _sequences[ch - 1].clear();
    }
    return LightFxError::OK;
}

template <uint8_t N, typename G>
uint8_t LedManager<N, G>::seqAdd(uint8_t ch, uint8_t eventType,
                                  uint16_t p1, uint16_t p2,
                                  uint8_t p3, uint8_t p4) {
    if (ch < 1 || ch > N) return LightFxError::INVALID_CHANNEL;
    if (!_enabled[ch - 1]) return LightFxError::CHANNEL_DISABLED;

    LedEventSeq& seq = _sequences[ch - 1];
    if (seq.isFull()) return LightFxError::SEQ_FULL;

    ILedEvent* event = nullptr;

    switch (eventType) {
        case LightFxEventType::ON:
            // p1=duration, p3=brightness
            event = new LedOn(p1, p3 > 0 ? p3 : 100);
            break;
        case LightFxEventType::OFF:
            // p1=duration
            event = new LedOff(p1);
            break;
        case LightFxEventType::FLASH:
            // p1=interval, p2=duration, p3=brightness, p4=duty
            event = new LedFlashing(p1, p2, p3 > 0 ? p3 : 100, p4 > 0 ? p4 : 50);
            break;
        case LightFxEventType::FADE_IN:
            // p1=duration, p3=brightness
            event = new LedFadeIn(p1, p3 > 0 ? p3 : 100);
            break;
        case LightFxEventType::FADE_OUT:
            // p1=duration, p3=brightness
            event = new LedFadeOut(p1, p3 > 0 ? p3 : 100);
            break;
        case LightFxEventType::FADING:
            // p1=cycle, p2=duration, p3=min, p4=max
            event = new LedFading(p1, p2, p3, p4 > 0 ? p4 : 100);
            break;
        case LightFxEventType::BEACON:
            // p1=cycle, p2=duration, p3=flashPercent, p4=maxBrightness
            event = new LedBeacon(p1, p2, p3 > 0 ? p3 : 15, 0, p4 > 0 ? p4 : 100);
            break;
        default:
            return LightFxError::INVALID_EVENT;
    }

    if (!event) return LightFxError::INVALID_EVENT;

    if (!seq.add(event)) {
        delete event;
        return LightFxError::SEQ_FULL;
    }
    return LightFxError::OK;
}

template <uint8_t N, typename G>
uint8_t LedManager<N, G>::seqStart(uint8_t ch) {
    if (ch == 0) {
        for (uint8_t i = 0; i < N; i++) {
            if (!_enabled[i]) continue;
            if (_sequences[i].count() > 0) _sequences[i].start();
        }
    } else {
        if (!_enabled[ch - 1]) return LightFxError::CHANNEL_DISABLED;
        _sequences[ch - 1].start();
    }
    return LightFxError::OK;
}

template <uint8_t N, typename G>
uint8_t LedManager<N, G>::seqStop(uint8_t ch) {
    if (ch == 0) {
        for (uint8_t i = 0; i < N; i++) {
            if (!_enabled[i]) continue;
            _sequences[i].stop();
        }
    } else {
        if (!_enabled[ch - 1]) return LightFxError::CHANNEL_DISABLED;
        _sequences[ch - 1].stop();
    }
    return LightFxError::OK;
}

template <uint8_t N, typename G>
uint8_t LedManager<N, G>::seqRestart(uint8_t ch) {
    if (ch == 0) {
        for (uint8_t i = 0; i < N; i++) {
            if (!_enabled[i]) continue;
            if (_sequences[i].count() > 0) _sequences[i].start();
        }
    } else {
        if (!_enabled[ch - 1]) return LightFxError::CHANNEL_DISABLED;
        _sequences[ch - 1].start();
    }
    return LightFxError::OK;
}

template <uint8_t N, typename G>
uint8_t LedManager<N, G>::setMasterBrightness(uint8_t pct) {
    _masterBrightness_pct = pct;
    for (uint8_t i = 0; i < N; i++) {
        _channels[i].setMasterBrightness_pct(pct);
    }
    return LightFxError::OK;
}

template <uint8_t N, typename G>
uint8_t LedManager<N, G>::resetChannel(uint8_t ch) {
    auto doReset = [this](uint8_t idx) {
        _sequences[idx].stop();
        _sequences[idx].clear();
        _channels[idx].off();
        _enabled[idx] = true;
    };

    if (ch == 0) {
        for (uint8_t i = 0; i < N; i++) {
            doReset(i);
        }
        // Also reset master brightness
        setMasterBrightness(100);
    } else {
        doReset(ch - 1);
    }
    return LightFxError::OK;
}

template <uint8_t N, typename G>
uint8_t LedManager<N, G>::enableChannel(uint8_t ch, bool enabled) {
    if (ch == 0) {
        for (uint8_t i = 0; i < N; i++) {
            if (!enabled) {
                _sequences[i].stop();
                _channels[i].off();
            }
            _enabled[i] = enabled;
        }
    } else {
        if (!enabled) {
            _sequences[ch - 1].stop();
            _channels[ch - 1].off();
        }
        _enabled[ch - 1] = enabled;
    }
    return LightFxError::OK;
}

// ============================================================================
// Queries
// ============================================================================

template <uint8_t N, typename G>
void LedManager<N, G>::getSeqStatus(uint8_t ch, LightFxSeqStatus& status) {
    if (ch < 1 || ch > N) return;
    LedEventSeq& seq = _sequences[ch - 1];
    status.channel = ch;
    status.playing = seq.isPlaying();
    status.eventCount = seq.count();
    status.currentIndex = seq.currentIndex();
    status.loopCount = seq.loopCount();
    status.brightness = _channels[ch - 1].brightness();
}

template <uint8_t N, typename G>
void LedManager<N, G>::getSeqQueue(uint8_t ch, LightFxSeqQueue& queue) {
    if (ch < 1 || ch > N) return;
    LedEventSeq& seq = _sequences[ch - 1];
    queue.channel = ch;
    queue.count = seq.count();
    queue.currentIndex = seq.currentIndex();
    queue.playing = seq.isPlaying();
    queue.brightness = _channels[ch - 1].brightness();

    for (uint8_t i = 0; i < queue.count && i < 24; i++) {
        ILedEvent* event = seq.eventAt(i);
        if (event) {
            queue.events[i].type = event->typeId();
            queue.events[i].duration = (uint16_t)event->duration();
            queue.events[i].param1 = 0;  // Param not accessible from interface
        }
    }
}

template <uint8_t N, typename G>
void LedManager<N, G>::getChannelStatus(uint8_t ch, LightFxChannelStatus& status) {
    if (ch < 1 || ch > N) return;
    status.channel = ch;
    status.brightness = _channels[ch - 1].brightness();
    status.seqPlaying = _sequences[ch - 1].isPlaying();
    status.seqEventCount = _sequences[ch - 1].count();
}

#endif // LED_MANAGER_IPP
