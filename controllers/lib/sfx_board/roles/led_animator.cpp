/*
 * LedAnimator implementation.
 */

#include "led_animator.h"

#include <cstring>

namespace sfx_core {

bool LedAnimator::loadQueue(const Event* events, size_t count) {
    if (count > MAX_EVENTS) return false;
    stop();
    if (count > 0) std::memcpy(_queue, events, count * sizeof(Event));
    _count  = (uint8_t)count;
    _cursor = 0;
    return true;
}

void LedAnimator::start() {
    if (_count == 0 || !_port) return;
    _cursor        = 0;
    _eventStart_ms = millis();
    _playing       = true;
    // Initial state — derive from first event.
    const Event& e0 = _queue[0];
    if (e0.kind == EV_ON) {
        _currentBright = e0.brightness;
        writeOutput(_currentBright);
    } else if (e0.kind == EV_OFF) {
        _currentBright = 0;
        writeOutput(0);
    } else if (e0.kind == EV_FADE) {
        _fadeFromBright = _currentBright;
        _fadeToBright   = e0.brightness;
    }
}

void LedAnimator::stop() {
    _playing = false;
    _currentBright = 0;
    if (_port) writeOutput(0);
}

void LedAnimator::tick() {
    if (!_playing || _count == 0 || !_port) return;

    const Event&    ev  = _queue[_cursor];
    const uint32_t  now = millis();
    const uint32_t  dt  = now - _eventStart_ms;
    bool advance = false;

    switch (ev.kind) {
        case EV_ON:
            _currentBright = ev.brightness;
            writeOutput(_currentBright);
            advance = true;       // instantaneous, advance immediately
            break;
        case EV_OFF:
            _currentBright = 0;
            writeOutput(0);
            advance = true;
            break;
        case EV_FADE:
            if (dt >= ev.duration_ms || ev.duration_ms == 0) {
                _currentBright = _fadeToBright;
                writeOutput(_currentBright);
                advance = true;
            } else {
                const int32_t delta = (int32_t)_fadeToBright - (int32_t)_fadeFromBright;
                const int32_t step  = (delta * (int32_t)dt) / (int32_t)ev.duration_ms;
                _currentBright = (uint8_t)((int32_t)_fadeFromBright + step);
                writeOutput(_currentBright);
            }
            break;
        case EV_HOLD:
            if (dt >= ev.duration_ms) advance = true;
            break;
        case EV_REPEAT:
            _cursor        = 0;
            _eventStart_ms = now;
            return;
        default:
            advance = true;       // unknown opcode — skip
            break;
    }

    if (advance) {
        _cursor++;
        if (_cursor >= _count) {
            _playing = false;
            _currentBright = 0;
            writeOutput(0);
            if (_onDone) _onDone();
            return;
        }
        _eventStart_ms = now;
        // Prime fade start brightness on the new event.
        const Event& next = _queue[_cursor];
        if (next.kind == EV_FADE) {
            _fadeFromBright = _currentBright;
            _fadeToBright   = next.brightness;
        }
    }
}

void LedAnimator::writeOutput(uint8_t brightness255) {
    if (!_port) return;
    // Scale 0..255 → 0..port.maxDuty() through master brightness.
    const uint32_t scaled = (uint32_t)brightness255 * _masterBrightness / 255u;
    const uint32_t duty   = scaled * _port->maxDuty() / 255u;
    _port->setDuty((uint16_t)duty);
}

}  // namespace sfx_core
