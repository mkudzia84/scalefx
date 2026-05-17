/*
 * LedAnimator — event-queue LED animation role on a `PwmPort`.
 *
 * Self-contained role: owns a small fixed-capacity event queue, a master
 * brightness multiplier, and the runtime state of the currently-playing
 * event.  Tick via `update()` from the role-service policy.
 *
 * Event types (single byte opcode + payload):
 *   ON           [brightness:u8]
 *   OFF          (no payload)
 *   FADE         [target:u8][duration_ms:u16LE]
 *   HOLD         [duration_ms:u16LE]                 — keep current state
 *   REPEAT       (no payload)                        — restart queue from start
 *
 * `LED_QUEUE_DONE` fires once (async, TAG_ASYNC) when the queue runs out
 * without a REPEAT and falls back to OFF.  REPEAT keeps the queue
 * looping silently.
 *
 * The port abstraction means LedAnimator works identically against a
 * native MCU PWM pin, a PCA9685 channel, or any future expander — the
 * board picks the driver at declaration time; the role doesn't care.
 */

#ifndef SFX_LED_ANIMATOR_H
#define SFX_LED_ANIMATOR_H

#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <ports/pwm_port.h>

namespace sfx_core {

class LedAnimator {
public:
    static constexpr size_t MAX_EVENTS = 32;

    enum EventKind : uint8_t {
        EV_ON     = 0x01,
        EV_OFF    = 0x02,
        EV_FADE   = 0x03,
        EV_HOLD   = 0x04,
        EV_REPEAT = 0x05,
    };

    struct Event {
        uint8_t  kind        = EV_OFF;
        uint8_t  brightness  = 0;    ///< for ON / FADE target
        uint16_t duration_ms = 0;    ///< for FADE / HOLD
    };

    /// Called (async, TAG_ASYNC) when the queue finishes without REPEAT.
    using DoneCallback = std::function<void()>;

    LedAnimator() = default;
    explicit LedAnimator(sfx_peripherals::PwmPort* port) : _port(port) {}

    /// Re-bind to a different port (used when the role is re-emplaced).
    void bind(sfx_peripherals::PwmPort* port) { _port = port; }

    /// Replace the queue contents.  Stops playback.  Returns false if
    /// `count > MAX_EVENTS`.
    bool loadQueue(const Event* events, size_t count);

    /// Start playing the queue from index 0.
    void start();
    /// Stop playback (force OFF on the port).
    void stop();

    /// Master brightness multiplier (0..255, default 255).  Scales every
    /// event's brightness when writing to the port.
    void setMasterBrightness(uint8_t b) { _masterBrightness = b; }
    uint8_t masterBrightness() const    { return _masterBrightness; }

    /// True iff the queue is currently playing.
    bool isPlaying() const { return _playing; }

    /// Number of events currently loaded.
    uint8_t queueDepth() const { return _count; }

    /// Register the done callback.  Fired once per queue completion.
    void onQueueDone(DoneCallback cb) { _onDone = std::move(cb); }

    /// Tick — drive the state machine.  Called from `update()`.
    void tick();

private:
    void writeOutput(uint8_t brightness255);   ///< scales to port maxDuty()

    sfx_peripherals::PwmPort* _port = nullptr;

    Event    _queue[MAX_EVENTS] {};
    uint8_t  _count            = 0;
    uint8_t  _cursor           = 0;       ///< index of currently-active event
    bool     _playing          = false;

    // Active-event state machine
    uint32_t _eventStart_ms    = 0;
    uint8_t  _fadeFromBright   = 0;       ///< for FADE
    uint8_t  _fadeToBright     = 0;
    uint8_t  _currentBright    = 0;       ///< last value sent to port (unscaled)
    uint8_t  _masterBrightness = 255;

    DoneCallback _onDone;
};

}  // namespace sfx_core

#endif  // SFX_LED_ANIMATOR_H
