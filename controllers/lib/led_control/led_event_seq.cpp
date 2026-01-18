/*
 * LED Event Sequence Library - Implementation
 * 
 * Manages a looping sequence of LED events.
 */

#include "led_event_seq.h"
#include "led_control.h"

// ============================================================================
// Constructor / Destructor
// ============================================================================

LedEventSeq::~LedEventSeq() {
    clear();
}

// ============================================================================
// Event Management
// ============================================================================

bool LedEventSeq::add(ILedEvent* event) {
    if (!event || _count >= LED_EVENT_SEQ_MAX) {
        return false;
    }
    
    _events[_count++] = event;
    return true;
}

void LedEventSeq::clear() {
    stop();
    
    // Delete all events
    for (uint8_t i = 0; i < _count; i++) {
        if (_events[i]) {
            delete _events[i];
            _events[i] = nullptr;
        }
    }
    _count = 0;
    _currentIndex = 0;
    _loopCount = 0;
}

ILedEvent* LedEventSeq::eventAt(uint8_t index) const {
    if (index >= _count) {
        return nullptr;
    }
    return _events[index];
}

// ============================================================================
// Playback Control
// ============================================================================

void LedEventSeq::start() {
    startAt(0);
}

void LedEventSeq::startAt(uint8_t index) {
    if (_count == 0 || index >= _count) {
        return;
    }
    
    _currentIndex = index;
    _playing = true;
    _loopCount = 0;
    
    // Start the first event
    if (_events[_currentIndex]) {
        _events[_currentIndex]->start(millis());
    }
}

void LedEventSeq::stop() {
    _playing = false;
    
    // Turn off LED if attached
    if (_led) {
        _led->off();
    }
}

int16_t LedEventSeq::update() {
    if (!_playing || _count == 0) {
        return -1;
    }
    
    ILedEvent* event = _events[_currentIndex];
    if (!event) {
        return -1;
    }
    
    uint32_t now = millis();
    int16_t intensity = event->update(now);
    
    // Check if current event is complete
    if (intensity < 0 || event->isComplete()) {
        advanceToNext();
        
        // Get intensity from new event
        event = _events[_currentIndex];
        if (event) {
            intensity = event->update(millis());
            if (intensity < 0) {
                intensity = 0;
            }
        } else {
            intensity = 0;
        }
    }
    
    // Apply to LedControl if attached
    if (_led && intensity >= 0) {
        _led->setBrightness((uint8_t)intensity);
    }
    
    return intensity;
}

ILedEvent* LedEventSeq::currentEvent() const {
    if (!_playing || _currentIndex >= _count) {
        return nullptr;
    }
    return _events[_currentIndex];
}

uint32_t LedEventSeq::totalDuration() const {
    uint32_t total = 0;
    for (uint8_t i = 0; i < _count; i++) {
        if (_events[i]) {
            total += _events[i]->duration();
        }
    }
    return total;
}

// ============================================================================
// Private Methods
// ============================================================================

void LedEventSeq::advanceToNext() {
    _currentIndex++;
    
    // Loop back to start if at end
    if (_currentIndex >= _count) {
        _currentIndex = 0;
        _loopCount++;
    }
    
    // Start the next event
    if (_events[_currentIndex]) {
        _events[_currentIndex]->start(millis());
    }
}
