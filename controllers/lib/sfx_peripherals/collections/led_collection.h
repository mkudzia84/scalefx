/*
 * LedCollection<K, TGpio> — facade around the existing LedManager,
 *                           extended with PWM-borrowed virtual outputs.
 *
 * lib/sfx_peripherals/led/led_manager.h already provides a full
 * templatised LED-channel manager with light-program runtime, BAM
 * software PWM, and event-sequence support.  LedCollection wraps that
 * with the generic-slave protocol vocabulary AND adopts PWM channels
 * that are currently in PwmLed mode as additional virtual LED
 * outputs.  Both kinds of outputs share **one** event-sequence runtime
 * — same LedEventSeq engine, same gamma curve, same `update()` tick.
 *
 * What this layer adds vs. raw LedManager:
 *   - protocol-aligned method signatures (matches slave.h LED_* packets)
 *   - ComponentInfo enumeration for COMPONENT_LIST_RESP
 *   - lifecycle hooks (attach / detach) tied to the slave's INIT state
 *   - PWM-borrowed output pool: pass a PwmCollection<M>* at attach
 *     time and any PWM channel currently in PwmLed mode is rendered
 *     by this LED runtime
 *
 * Addressing (matches slave.h SlavePacket::LedAddr helpers):
 *   - bit 7 = 0  →  dedicated LED, idx 0..(K-1)
 *   - bit 7 = 1  →  PWM-borrowed LED, idx 0..(M-1) within the
 *                   PwmCollection (channel must be in PwmLed mode)
 *
 * See instructions/15-GENERIC-SLAVE-REFACTOR.md § "LED runtime
 * ownership" for the full lifecycle of channels switching in/out of
 * PwmLed mode.
 */

#ifndef SFX_LED_COLLECTION_H
#define SFX_LED_COLLECTION_H

#include <array>
#include <cstdint>
#include <functional>

#include <sfx_peripherals/led/led_manager.h>

#include "component_event.h"
#include <serial/slave/component_kind.h>
#include "pwm_led_sink.h"

namespace sfx_peripherals {

/// Per-channel pin-on-backend descriptor.  PWM backend type is supplied
/// as a template parameter so this works against NativeGpio (Pico
/// boards via analogWrite) or PCA9685 (I²C 16-channel PWM chip) without
/// code duplication.  Both backends satisfy the `PwmOutput` concept in
/// `pwm/pwm_output.h`.
struct LedSpec {
    uint8_t pin;             ///< pin index within the PWM backend
    uint8_t flags;           ///< LedFlags::* bitmask
};

/// `LedCollection<K, TGpio, TPwmSink>` — K dedicated LedDigital outputs
/// plus optional PWM-borrowed extension slots.  Templated on the
/// PWM-sink type so the cross-coupling with PwmCollection happens
/// entirely at compile time (no virtual dispatch, no abstract base).
///
/// `TPwmSink` must satisfy the `PwmLedSink` concept (see
/// `pwm_led_sink.h`).  `PwmCollection<N, TSense>` satisfies it
/// directly — pass it as the third template arg, or leave the default
/// `NullPwmSink` for boards that don't borrow PWM channels.
///
/// Example (LightFX — dedicated only, no PWM borrowed):
///   LedCollection<8, NativeGpio> leds;                      // TPwmSink = NullPwmSink
///
/// Example (HubFX — 8 PCA9685 LEDs + PWM-borrowed channels):
///   PwmCollection<6, HubFxSensePolicy>                pwms;
///   LedCollection<8, PCA9685, decltype(pwms)>         leds;
///   leds.configure(specs, &ledPwm, &pwms);
template <size_t K, typename TGpio, PwmLedSink TPwmSink = NullPwmSink>
class LedCollection {
public:
    static constexpr size_t COUNT = K;
    using GpioProvider = TGpio;
    using PwmSink      = TPwmSink;

    /// Wire dedicated-LED specs + GPIO provider.  The `pwmSink`
    /// pointer is optional — supply it when this board has PWM
    /// channels that can be runtime-flipped to PwmLed mode.  When
    /// omitted (or `TPwmSink = NullPwmSink`), the collection only
    /// manages the K dedicated outputs and the PWM-borrowed code
    /// paths short-circuit cleanly.
    void configure(const std::array<LedSpec, K>& specs,
                   TGpio*                       provider,
                   TPwmSink*                    pwmSink = nullptr) {
        _specs    = specs;
        _provider = provider;
        _pwmSink  = pwmSink;
    }

    bool attach();
    void detach();
    void update();   ///< drives the event-sequence runtime — call from loop()

    /// Stop every running program and drop every output (dedicated and
    /// PWM-borrowed) to brightness 0.  Invoked from
    /// `SlaveServer::enterSafeState()`.
    void allOff();

    // ── Per-channel API (address byte: bit 7 = PWM-borrowed) ─────────

    bool setBrightness(uint8_t addr, uint8_t brightness);   ///< 0=off, 255=full

    /// Load an LED program into a per-channel program slot.  Programs
    /// are LedEventSeq instances (lib/sfx_peripherals/led/led_event_seq.h);
    /// the protocol marshals them as a [count:u8][LedEvent×N] payload.
    bool loadProgram(uint8_t addr, uint8_t progId, const LedEvent* events, uint8_t count);

    /// Start a program on `addr`.  `flags` carries the bits defined in
    /// `SlavePacket::LedProgramFlags` (REPEAT, SYNC_START).  When
    /// REPEAT is **clear** and the program runs to its last event,
    /// `_onProgramDone` fires once with (addr, progId) — SlaveServer
    /// turns that into a `LED_PROGRAM_DONE` async packet.  When
    /// REPEAT is set, the program loops forever and the callback
    /// never fires; only `LED_PROGRAM_STOP` ends it.
    bool runProgram (uint8_t addr, uint8_t progId, uint8_t flags);
    bool stopProgram(uint8_t addr);

    /// Restart the running program at event 0 without unloading it.
    bool restartProgram(uint8_t addr);

    /// Hard reset: stop + clear queue + brightness 0 + re-enable.
    /// addr = 0xFF broadcasts to every dedicated + PWM-borrowed
    /// channel on this collection.
    bool resetChannel(uint8_t addr);

    /// Enable / disable a channel.  Disabled channels emit no
    /// output and ignore brightness / program writes (writes still
    /// ACK; the status flag reflects the disabled state).
    bool enableChannel(uint8_t addr, bool enabled);

    /// Master brightness percentage 0..100 — multiplicative scaler
    /// applied to every channel uniformly (dedicated + PWM-borrowed).
    bool setMasterBrightness(uint8_t pct);

    bool query(uint8_t addr,
               uint8_t& out_brightness,
               uint8_t& out_progId,
               uint8_t& out_progState) const;

    /// Detailed sequence status — current event index, event count,
    /// active event type, time remaining in current event, repeat
    /// count, status flags.  Mirrors LightFxSeqStatus from
    /// `serial/lightfx/lightfx.h` (the same struct LedManager fills).
    bool seqStatus(uint8_t addr, LightFxSeqStatus& out) const;

    // ── Program-done callback ────────────────────────────────────────
    using ProgramDoneCb = std::function<void(uint8_t addr, uint8_t progId)>;
    void setProgramDoneCallback(ProgramDoneCb cb) { _onProgramDone = std::move(cb); }

    /// Board-local event callback (indicator LEDs / buzzers / logs).
    /// Fires on Activated / Deactivated / ProgramStarted / ProgramEnded /
    /// ProgramStopped / SafeStateEntered.  See `component_event.h`.
    void setEventCallback(ComponentEventCb cb) { _onEvent = std::move(cb); }

    // ── PWM-coupling lifecycle (called by PwmCollection) ─────────────

    /// Notification from the PwmCollection that channel `pwmIdx` has
    /// switched INTO PwmLed mode.  LedCollection adopts it as an
    /// extension output starting at the next update().
    void onPwmEnteredLedMode(uint8_t pwmIdx);

    /// Notification that channel `pwmIdx` has switched OUT of PwmLed
    /// mode.  Stops any program running on the channel and drops the
    /// extension output.
    void onPwmLeftLedMode(uint8_t pwmIdx);

    // ── Component enumeration ────────────────────────────────────────

    ComponentInfo describe(size_t i) const {
        return ComponentInfo{
            .index = (uint8_t)i,
            .kind  = ComponentKind::LedDigital,
            .flags = (uint8_t)(_specs[i].flags | LedFlags::SUPPORTS_PROGRAMS),
            .reserved = 0,
        };
    }

private:
    std::array<LedSpec, K>          _specs{};
    LedManager<K, TGpio>            _manager{};
    TGpio*                          _provider = nullptr;
    TPwmSink*                       _pwmSink  = nullptr;   ///< optional — for PwmLed-mode outputs
    ProgramDoneCb                   _onProgramDone;
    ComponentEventCb                _onEvent;
    bool                            _attached = false;

    // Per-channel program-state mirror — stored so the program-done
    // callback can supply the correct progId (LedEventSeq doesn't
    // know the protocol-level program identity).  `_progRunning` is
    // a one-shot edge guard: armed by runProgram() with REPEAT clear,
    // consumed by update() when LedEventSeq::isComplete() fires.
    std::array<uint8_t, K>          _progIds{};
    std::array<bool, K>             _progRunning{};   ///< awaiting natural-end edge
};

}  // namespace sfx_peripherals

#endif  // SFX_LED_COLLECTION_H
