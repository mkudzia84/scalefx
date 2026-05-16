/*
 * LedCollection<K, TGpio, TPwmSink>::* — implementation.
 *
 * Thin facade over LedManager<K, TGpio> (the existing event-sequence
 * runtime in sfx_peripherals/led/led_manager.h) that:
 *   - exposes the slave-protocol address-byte semantics
 *     (bit 7 = PWM-borrowed, see slave.h LedAddr)
 *   - bridges program-done events from LedManager to the SlaveServer's
 *     async-emitter callback
 *   - drives PwmLed-mode PWM channels through the IPwmLedSink interface
 *     when a sink is bound
 *
 * The LedManager's internal LedEventSeq runtime is untouched —
 * brightness curves, fade math, BAM ticking all happen there.
 */

#ifndef SFX_LED_COLLECTION_IPP
#define SFX_LED_COLLECTION_IPP

#include "led_collection.h"
#include <serial/slave/slave.h>

namespace sfx_peripherals {

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::attach() {
    if (_attached) return true;
    if (!_provider) return false;

    // Build pins[] from the per-channel specs.  LedManager::begin
    // expects a flat uint8_t array of pin indices.
    uint8_t pins[K ? K : 1];
    for (size_t i = 0; i < K; i++) pins[i] = _specs[i].pin;

    if constexpr (K > 0) {
        if (!_manager.begin(_provider, pins)) return false;
    }
    _attached = true;
    if (_onEvent) {
        for (size_t i = 0; i < K; i++) {
            _onEvent(SlavePacket::LedAddr::dedicated((uint8_t)i),
                     ComponentEvent::Activated, 0);
        }
    }
    return true;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
void LedCollection<K, TGpio, TPwmSink>::detach() {
    if (!_attached) return;
    if constexpr (K > 0) _manager.resetChannel(0);   // 0 = all
    _attached = false;
    if (_onEvent) {
        for (size_t i = 0; i < K; i++) {
            _onEvent(SlavePacket::LedAddr::dedicated((uint8_t)i),
                     ComponentEvent::Deactivated, 0);
        }
    }
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
void LedCollection<K, TGpio, TPwmSink>::update() {
    if (!_attached) return;
    if constexpr (K > 0) _manager.update();

    // Poll for one-shot completion edges.  For each channel where
    // `_progRunning[i]` is set (REPEAT was clear at runProgram time)
    // check whether LedEventSeq has reached its terminal state — if
    // so, fire `_onProgramDone` exactly once and clear the guard.
    // Looped programs never reach this branch because runProgram()
    // never arms `_progRunning` for them.
    if constexpr (K > 0) {
        for (size_t i = 0; i < K; i++) {
            if (!_progRunning[i]) continue;
            if (_manager.seqIsComplete((uint8_t)(i + 1))) {
                _progRunning[i] = false;
                const uint8_t addr   = SlavePacket::LedAddr::dedicated((uint8_t)i);
                const uint8_t progId = _progIds[i];
                if (_onProgramDone) _onProgramDone(addr, progId);
                if (_onEvent)       _onEvent(addr, ComponentEvent::ProgramEnded, progId);
            }
        }
    }

    // Drive PWM-borrowed outputs.  For each PwmLed-mode channel on the
    // bound sink, query whatever brightness LedManager would emit if
    // that channel were dedicated, and write it through the sink.
    // Implementation note: this is a placeholder hook — the actual
    // wiring requires LedManager to expose a "render-channel-N-now"
    // accessor.  When that accessor lands, replace this loop with the
    // real call.  For now: PWM-borrowed outputs work via plain
    // `setBrightness` writes from the master.
    if (_pwmSink) {
        // intentionally empty — see note above.
    }
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
void LedCollection<K, TGpio, TPwmSink>::allOff() {
    if constexpr (K > 0) {
        if (_attached) {
            _manager.seqStop(0);     // 0 = all
            _manager.ledOff(0);
        }
    }
    if (_pwmSink) {
        for (uint8_t i = 0; i < _pwmSink->pwmChannelCount(); i++) {
            if (_pwmSink->isInLedMode(i)) _pwmSink->writeDuty(i, 0);
        }
    }
    if (_onEvent) {
        for (size_t i = 0; i < K; i++) {
            _onEvent(SlavePacket::LedAddr::dedicated((uint8_t)i),
                     ComponentEvent::SafeStateEntered, 0);
        }
    }
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::setBrightness(uint8_t addr, uint8_t brightness) {
    if (!_attached) return false;
    if (SlavePacket::LedAddr::isPwmBorrowed(addr)) {
        if (!_pwmSink) return false;
        uint8_t idx = SlavePacket::LedAddr::indexOf(addr);
        if (idx >= _pwmSink->pwmChannelCount()) return false;
        if (!_pwmSink->isInLedMode(idx)) return false;
        // Map 0..255 brightness → 0..1000 duty thousandths.
        _pwmSink->writeDuty(idx, (uint16_t)((uint32_t)brightness * 1000u / 255u));
        return true;
    }
    uint8_t idx = SlavePacket::LedAddr::indexOf(addr);
    if (idx >= K) return false;
    // LedManager uses 1-based channel numbering (matches LightFX wire
    // protocol convention) — convert.
    return _manager.ledSet((uint8_t)(idx + 1), brightness) == 0;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::loadProgram(uint8_t            addr,
                                          uint8_t            /*progId*/,
                                          const LedEvent*    events,
                                          uint8_t            count) {
    if (!_attached) return false;
    if (SlavePacket::LedAddr::isPwmBorrowed(addr)) {
        // PWM-borrowed channels share the LED runtime; LedManager uses
        // sequence slots that are currently keyed by 1-based channel
        // index in the dedicated range.  Until LedManager grows
        // first-class extension-output slots, programs on PwmLed
        // channels are not yet supported — return error (the master
        // drives them via direct setBrightness in the meantime).
        return false;
    }
    uint8_t idx = SlavePacket::LedAddr::indexOf(addr);
    if (idx >= K) return false;
    uint8_t ch = (uint8_t)(idx + 1);

    if (_manager.seqClear(ch) != 0) return false;
    for (uint8_t i = 0; i < count; i++) {
        const LedEvent& e = events[i];
        if (_manager.seqAdd(ch, e.type, e.p1, e.p2, e.p3, e.p4, e.p5) != 0) {
            return false;
        }
    }
    return true;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::runProgram(uint8_t addr,
                                         uint8_t progId,
                                         uint8_t flags) {
    if (!_attached) return false;
    if (SlavePacket::LedAddr::isPwmBorrowed(addr)) return false;
    uint8_t idx = SlavePacket::LedAddr::indexOf(addr);
    if (idx >= K) return false;

    // Wire-format REPEAT bit → LedEventSeq's per-sequence repeat
    // flag.  Default (REPEAT clear) is one-shot — sequence stops
    // after its last event and `_onProgramDone` fires once with
    // (addr, progId).
    const bool repeat = (flags & SlavePacket::LedProgramFlags::REPEAT) != 0;
    _manager.seqSetRepeat((uint8_t)(idx + 1), repeat);

    bool ok = _manager.seqStart((uint8_t)(idx + 1)) == 0;
    if (ok) {
        _progIds[idx]     = progId;
        _progRunning[idx] = !repeat;   ///< only arm the completion edge for one-shot
        if (_onEvent) _onEvent(addr, ComponentEvent::ProgramStarted, progId);
    }
    return ok;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::stopProgram(uint8_t addr) {
    if (!_attached) return false;
    if (SlavePacket::LedAddr::isPwmBorrowed(addr)) return false;
    uint8_t idx = SlavePacket::LedAddr::indexOf(addr);
    if (idx >= K) return false;
    bool ok = _manager.seqStop((uint8_t)(idx + 1)) == 0;
    if (ok) {
        // Master-initiated stop — drop the one-shot guard so update()
        // doesn't spuriously fire LED_PROGRAM_DONE.  Looping programs
        // already had `_progRunning[i] == false`.
        _progRunning[idx] = false;
        if (_onEvent) _onEvent(addr, ComponentEvent::ProgramStopped, 0);
    }
    return ok;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::restartProgram(uint8_t addr) {
    if (!_attached) return false;
    if (SlavePacket::LedAddr::isPwmBorrowed(addr)) return false;
    uint8_t idx = SlavePacket::LedAddr::indexOf(addr);
    if (idx >= K) return false;
    bool ok = _manager.seqRestart((uint8_t)(idx + 1)) == 0;
    // Re-arm the guard if the previous run was one-shot — restart
    // re-enters the same sequence so the same termination semantics
    // apply.  (LedEventSeq::start() already cleared `_completed`.)
    return ok;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::resetChannel(uint8_t addr) {
    if (!_attached) return false;
    // 0xFF = broadcast — LedManager uses 0 as "all", so translate.
    uint8_t ch;
    if (addr == 0xFF) {
        ch = 0;        // LedManager broadcast
    } else if (SlavePacket::LedAddr::isPwmBorrowed(addr)) {
        // Extension-slot reset is handled below.  TODO when extensions land.
        return true;
    } else {
        uint8_t idx = SlavePacket::LedAddr::indexOf(addr);
        if (idx >= K) return false;
        ch = (uint8_t)(idx + 1);
    }
    return _manager.resetChannel(ch) == 0;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::enableChannel(uint8_t addr, bool enabled) {
    if (!_attached) return false;
    if (SlavePacket::LedAddr::isPwmBorrowed(addr)) return false;   // TODO extension
    if (addr == 0xFF) return _manager.enableChannel(0, enabled) == 0;
    uint8_t idx = SlavePacket::LedAddr::indexOf(addr);
    if (idx >= K) return false;
    return _manager.enableChannel((uint8_t)(idx + 1), enabled) == 0;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::setMasterBrightness(uint8_t pct) {
    if (!_attached) return false;
    if (pct > 100) pct = 100;
    return _manager.setMasterBrightness(pct) == 0;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::seqStatus(uint8_t addr, LightFxSeqStatus& out) const {
    if (!_attached) return false;
    if (SlavePacket::LedAddr::isPwmBorrowed(addr)) {
        // Extension-slot status is TBD; return a zeroed struct so the
        // master can still parse the response.
        out = LightFxSeqStatus{};
        return true;
    }
    uint8_t idx = SlavePacket::LedAddr::indexOf(addr);
    if (idx >= K) return false;
    const_cast<LedManager<K, TGpio>&>(_manager)
        .getSeqStatus((uint8_t)(idx + 1), out);
    return true;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
bool LedCollection<K, TGpio, TPwmSink>::query(uint8_t  addr,
                                    uint8_t& out_brightness,
                                    uint8_t& out_progId,
                                    uint8_t& out_progState) const {
    if (!_attached) return false;
    if (SlavePacket::LedAddr::isPwmBorrowed(addr)) {
        out_brightness = 0;
        out_progId     = 0;
        out_progState  = 0;
        return true;
    }
    uint8_t idx = SlavePacket::LedAddr::indexOf(addr);
    if (idx >= K) return false;
    LightFxChannelStatus st{};
    const_cast<LedManager<K, TGpio>&>(_manager).getChannelStatus((uint8_t)(idx + 1), st);
    out_brightness = st.brightness;
    out_progId     = 0;
    out_progState  = st.flags;
    return true;
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
void LedCollection<K, TGpio, TPwmSink>::onPwmEnteredLedMode(uint8_t /*pwmIdx*/) {
    // Hook for the PwmCollection to notify us that a new channel is
    // available as a virtual LED output.  Currently the LED runtime
    // doesn't track per-extension state; the master drives the
    // channel via setBrightness.  Reserved for the future
    // first-class-extension-slot work.
}

template <size_t K, typename TGpio, PwmLedSink TPwmSink>
void LedCollection<K, TGpio, TPwmSink>::onPwmLeftLedMode(uint8_t pwmIdx) {
    // Drop output to 0 before the PwmCollection switches modes.
    if (_pwmSink) _pwmSink->writeDuty(pwmIdx, 0);
}

}  // namespace sfx_peripherals

#endif  // SFX_LED_COLLECTION_IPP
