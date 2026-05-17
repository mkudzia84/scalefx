/*
 * LED Event Sequence Library - Header
 * 
 * Manages a sequence of LED events that play in order and loop.
 * 
 * Features:
 *   - Up to 24 events in a sequence
 *   - Automatic looping when sequence completes
 *   - Integration with LedControl for PWM output
 * 
 * Usage:
 *   LedEventSeq seq;
 *   seq.add(new LedOn(500));        // On for 500ms
 *   seq.add(new LedOff(500));       // Off for 500ms
 *   seq.add(new LedFadeIn(1000));   // Fade in over 1s
 *   seq.add(new LedFadeOut(1000));  // Fade out over 1s
 *   seq.start();
 *   
 *   // In loop:
 *   int16_t pwm = seq.update();     // Get current PWM value
 */

#ifndef LED_EVENT_SEQ_H
#define LED_EVENT_SEQ_H

#include <Arduino.h>
#include "led_events.h"

// Maximum number of events in a sequence
#define LED_EVENT_SEQ_MAX 24

// Forward declaration — ILedOutput is defined in led_control.h
class ILedOutput;

// ============================================================================
// LedEventSeq Class
// ============================================================================

/**
 * @brief Manages a looping sequence of LED events
 * 
 * Events are played in order, and when the last event completes,
 * the sequence loops back to the first event.
 *
 * @note If an event has duration=0 (infinite), it never completes.
 *       The sequence will stall on that event and never advance or
 *       loop.  Use an infinite ON/OFF as the last event to hold a
 *       terminal state (e.g., FADE_IN → ON∞ fades in then stays on).
 */
class LedEventSeq {
public:
    LedEventSeq() = default;
    ~LedEventSeq();
    
    // ========================================================================
    // Event Management
    // ========================================================================

    /**
     * @brief Add an event to the sequence (takes ownership)
     * @param event Pointer to event (will be deleted when cleared)
     * @return true if added, false if sequence is full
     */
    bool add(ILedEvent* event);

    /**
     * @brief Clear all events from the sequence
     */
    void clear();

    /**
     * @brief Get number of events in sequence
     */
    uint8_t count() const { return _count; }

    /**
     * @brief Check if sequence is empty
     */
    bool isEmpty() const { return _count == 0; }

    /**
     * @brief Check if sequence is full
     */
    bool isFull() const { return _count >= LED_EVENT_SEQ_MAX; }

    /**
     * @brief Get event at index (nullptr if out of range)
     */
    ILedEvent* eventAt(uint8_t index) const;

    // ========================================================================
    // Playback Control
    // ========================================================================

    /**
     * @brief Start playing the sequence from the beginning
     */
    void start();

    /**
     * @brief Start playing from a specific event index
     * @param index Event index to start from
     */
    void startAt(uint8_t index);

    /**
     * @brief Stop playback
     */
    void stop();

    /**
     * @brief Set repeat / one-shot mode for the sequence.
     *
     * When `repeat == true` (default) the sequence loops back to event 0
     * after the last event completes — matches the historical
     * "looping sequence" behaviour LightFX has always had.
     *
     * When `repeat == false` the sequence stops automatically after the
     * last event finishes; `isPlaying()` becomes false and a follow-up
     * call to `isComplete()` returns true so the LedManager can fire
     * the LED_QUEUE_DONE async packet exactly once.
     *
     * Safe to call before or during playback — takes effect on the
     * next loop boundary.  Per-call default for `start()` /
     * `seqStart()` paths can be set via this helper.
     */
    void setRepeat(bool repeat) { _repeat = repeat; }

    /// True when a one-shot sequence has finished its last event.
    /// Latches until the next start() call so the caller can poll it
    /// once and reliably observe the completion edge.  Always false
    /// when `_repeat == true` (looping sequences never "complete").
    bool isComplete() const { return _completed; }

    /**
     * @brief Update sequence and get current brightness
     * @return Brightness 0-100, or -1 if stopped/empty
     */
    int16_t update();

    /**
     * @brief Check if sequence is currently playing
     */
    bool isPlaying() const { return _playing; }

    /**
     * @brief Get current event index
     */
    uint8_t currentIndex() const { return _currentIndex; }

    /**
     * @brief Get current event (nullptr if not playing)
     */
    ILedEvent* currentEvent() const;

    /**
     * @brief Get total duration of one complete sequence cycle (ms)
     */
    uint32_t totalDuration() const;

    /**
     * @brief Get number of times sequence has looped
     */
    uint32_t loopCount() const { return _loopCount; }

    // ========================================================================
    // LedControl Integration
    // ========================================================================

    /**
     * @brief Attach to an ILedOutput for automatic PWM output.
     * Accepts any LedControlT<TGpio> (they all implement ILedOutput).
     * @param led Pointer to ILedOutput instance
     */
    void attachLed(ILedOutput* led) { _led = led; }

    /**
     * @brief Detach from the LED output
     */
    void detachLed() { _led = nullptr; }

    /**
     * @brief Check if attached to an LED output
     */
    bool hasLed() const { return _led != nullptr; }

private:
    void advanceToNext();

    ILedEvent* _events[LED_EVENT_SEQ_MAX] = {nullptr};
    uint8_t _count = 0;
    uint8_t _currentIndex = 0;
    bool _playing = false;
    bool _repeat = true;        ///< true = loop forever, false = stop after last event
    bool _completed = false;    ///< latches true on natural one-shot completion (cleared by start())
    uint32_t _loopCount = 0;
    ILedOutput* _led = nullptr;
};

#endif // LED_EVENT_SEQ_H
