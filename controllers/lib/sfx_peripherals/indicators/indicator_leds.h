/*
 * IndicatorServicePolicy — Connection / error LED state machine.
 *
 * Satisfies the SystemServicePolicy concept (sfx_serial/serial/core/system_service.h)
 * so it can sit in a `BoardServer<...>` policy pack alongside the wire-
 * protocol-handling policies (Audio, Storage, …).  Owns no packet range
 * — `ownsType()` always returns false, `handle()` returns NotMyCommand.
 * Its job is purely runtime: tick the LED outputs every loop() iteration
 * based on the lifecycle state set by the board firmware.
 *
 * LED 0 (Connection):
 *   - Blink 500 ms:  Waiting for INIT (power-on, not yet connected)
 *   - Solid ON:      Connected and initialised
 *   - OFF:           Connection lost (watchdog timeout triggered)
 *
 * LED 1 (Error / Warning, three-tier priority):
 *   - Fast blink 200 ms:  Error condition (highest priority)
 *   - Slow blink 500 ms:  Warning condition (e.g. low voltage)
 *   - OFF:                Normal operation
 *
 * Usage (post-wave-4 — instantiated inside SfxServer's templated
 * BoardServer pack):
 *
 *   IndicatorServicePolicy::Config cfg{ .connectionPin = 13, .errorPin = 14 };
 *   board.policy<IndicatorServicePolicy>().configure(cfg);
 *
 *   // Lifecycle hooks (called by BoardServicePolicy on INIT / SHUTDOWN /
 *   // keepalive timeout) flip the runtime flags:
 *   board.policy<IndicatorServicePolicy>().setConnected(true);
 *   board.policy<IndicatorServicePolicy>().setErrorCondition(true);
 *
 *   // BoardServer::update() ticks every policy each loop() iteration.
 *
 * Pre-wave-4 (today): SfxServer owns a single IndicatorServicePolicy
 * directly and calls update() from its loop().  Existing
 * `server.indicators().setConnected(...)` callers keep working through
 * the accessor.
 */

#ifndef INDICATOR_LEDS_H
#define INDICATOR_LEDS_H

#include <Arduino.h>
#include <cstdint>
#include <cstddef>

#include "led/led_control.h"
#include <serial/core/core.h>             // CommandHandleResult

namespace sfx_core { class ServiceContext; }

class IndicatorServicePolicy {
public:
    /// Indicator LEDs are purely diagnostic — no IDENTIFY capability bit.
    static constexpr uint32_t kCapabilityBits = 0u;

    struct Config {
        int connectionPin = -1;    ///< -1 = disabled
        int errorPin      = -1;    ///< -1 = disabled
    };

    IndicatorServicePolicy() = default;

    // ── SystemServicePolicy surface ───────────────────────────────────

    bool begin(sfx_core::ServiceContext* /*ctx*/) {
        // No wire surface — ctx unused.  Board firmware calls configure()
        // separately so it can supply pin numbers without changing the
        // begin() signature shared by every policy.
        return true;
    }

    bool ownsType(uint8_t /*type*/) const { return false; }

    CommandHandleResult handle(uint8_t /*type*/, const uint8_t*, size_t) {
        return CommandHandleResult::NotMyCommand;
    }

    /// Tick the LED state machines.  Called by `BoardServer::update()`
    /// once per loop() iteration.
    void update();

    // ── Configuration (called from board setup) ───────────────────────

    /// Bind the indicator LEDs to specific pins.  Disabled when pin = -1.
    /// Can be called before or after `BoardServer::begin()` — initialises
    /// the LedControl on call.
    void configure(const Config& cfg);

    /// Legacy two-arg form for boards that have not migrated to Config.
    void configure(int connectionPin, int errorPin) {
        configure(Config{ connectionPin, errorPin });
    }

    // ── State setters (called by lifecycle hooks) ─────────────────────

    /// Set connection state (true = INIT received, false = shutdown).
    void setConnected(bool connected) { _connected = connected; }

    /// Set watchdog-triggered state (true = keepalive timeout).
    void setWatchdogTriggered(bool triggered) { _watchdogTriggered = triggered; }

    /// Set error condition (fast blink, highest priority on error LED).
    void setErrorCondition(bool error) { _errorCondition = error; }

    /// Set warning condition (slow blink, lower priority than error).
    void setWarningCondition(bool warning) { _warningCondition = warning; }

    // ── State queries ─────────────────────────────────────────────────

    bool isConnected() const { return _connected; }
    bool isWatchdogTriggered() const { return _watchdogTriggered; }

    /// Const-ref access to the underlying LedControl instances (for
    /// status-payload construction).
    const LedControl& connectionLed() const { return _leds[0]; }
    const LedControl& errorLed() const { return _leds[1]; }

private:
    LedControl _leds[2];
    bool _connected         = false;
    bool _watchdogTriggered = false;
    bool _errorCondition    = false;
    bool _warningCondition  = false;

    static constexpr uint16_t BLINK_WAITING_ms = 500;
    static constexpr uint16_t BLINK_ERROR_ms   = 200;
    static constexpr uint16_t BLINK_WARNING_ms = 500;
};

/// @deprecated In-flight alias for callers that haven't migrated to the
///             policy name.  Same class.
using IndicatorLedManager = IndicatorServicePolicy;

#endif // INDICATOR_LEDS_H
