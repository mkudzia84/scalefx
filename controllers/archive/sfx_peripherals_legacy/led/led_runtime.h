/*
 * LedRuntime<N, TPwm> — N-channel LED runtime.
 *
 * Replaces the legacy LedManager + LedCollection two-layer wrapper with a
 * single template, used identically by:
 *   - HubFX local PCA9685 LEDs                  LedRuntime<8, PCA9685>
 *   - Expander dedicated LEDs (NativeGpio)       LedRuntime<K, NativeGpio>
 *   - Expander PWM-borrowed LED slots            LedRuntime<M, PwmDutyAdapter<TPwms>>
 *
 * Per-channel state:
 *   - `LedControlT<TPwm>` — single-LED output (on/off/brightness, active-high/low)
 *   - `LedEventSeq`       — event-queue interpreter (ON, OFF, FADE_*, FADING, BEACON)
 *
 * The wire-format dispatcher (CoreServer for expander-side, HubFX-LED
 * dispatcher for hub-local) parses the address byte and calls into the
 * right LedRuntime instance with a 0-based channel index in `[0, N)`.
 * No 1-based legacy.  No `ILedManager` virtual interface.  No LightFX-
 * protocol struct dependency.
 *
 * Queue lifecycle:
 *   loadQueue(idx, flags, events, count) — replaces the queue, parks duty 0
 *   startQueue(idx)                       — plays from event 0
 *   stopQueue(idx) / restartQueue(idx) / clearQueue(idx) — self-evident
 *   On natural completion of a non-REPEAT queue, update() fires the
 *   registered `onQueueDone(idx)` callback exactly once.
 *
 * Master brightness lives here (single source of truth); push to each
 * channel's LedControlT on change so LedEventSeq emissions honour it.
 */

#ifndef SFX_LED_RUNTIME_H
#define SFX_LED_RUNTIME_H

#include <array>
#include <cstdint>
#include <functional>

#include "led_control.h"
#include "led_event_seq.h"
#include "led_events.h"

#include <serial/components/components.h>
#include <serial/components/led_status.h>

namespace sfx_peripherals {

template <size_t N, typename TPwm>
class LedRuntime {
public:
    static constexpr size_t COUNT = N;
    using PwmBackend = TPwm;
    using QueueDoneCb = std::function<void(uint8_t idx)>;

    LedRuntime() {
        for (size_t i = 0; i < N; i++) _enabled[i] = true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────

    /// Bind to a PWM-output backend and configure each channel.
    /// `pins` indexes into the backend (GPIO pin number for NativeGpio,
    /// channel 0..15 for PCA9685, etc.).
    void begin(TPwm* backend, const uint8_t (&pins)[N],
               bool activeLow = false, bool pwm = true);

    /// Tear down — stops all sequences and releases the backend pointer.
    void end();

    /// Stop all sequences, drop brightness to 0 on every channel.
    /// Called by CoreServer::enterSafeState().
    void allOff();

    /// Tick — drives playing sequences and fires the queue-done edge
    /// callback exactly once per natural completion.  Call from loop().
    void update();

    // ─────────────────────────────────────────────────────────────────
    // Per-channel operations — all 0-based, idx in [0, N)
    // ─────────────────────────────────────────────────────────────────

    /// Set immediate brightness 0..255.  Stops any running queue on
    /// the channel (the queue would overwrite the value on the next
    /// tick otherwise).
    bool setBrightness(uint8_t idx, uint8_t brightness_0_255);

    /// Replace the channel's event queue.  `flags` carries
    /// LedQueueFlags::* bits — currently only REPEAT (auto-restart from
    /// event 0 on completion).  Loading does NOT start the queue.
    bool loadQueue(uint8_t idx, uint8_t flags,
                   const ComponentPacket::LedEvent* events, uint8_t count);

    /// Start playback from event 0.
    bool startQueue(uint8_t idx);

    /// Stop playback (queue remains loaded; can be restarted).
    bool stopQueue(uint8_t idx);

    /// Restart from event 0 without re-uploading.
    bool restartQueue(uint8_t idx);

    /// Clear the queue (stop + remove events).
    bool clearQueue(uint8_t idx);

    /// Hard reset: stop + clear + brightness 0 + re-enable.
    bool resetChannel(uint8_t idx);

    /// Enable / disable a channel.  Disabled channels emit no output
    /// and reject brightness / queue writes (return false).
    bool enableChannel(uint8_t idx, bool enabled);

    // ─────────────────────────────────────────────────────────────────
    // Master brightness (multiplicative scaler 0..100%)
    // ─────────────────────────────────────────────────────────────────

    void    setMasterBrightness(uint8_t pct);
    uint8_t masterBrightness() const { return _masterBrightness_pct; }

    // ─────────────────────────────────────────────────────────────────
    // Queries
    // ─────────────────────────────────────────────────────────────────

    bool query     (uint8_t idx, ComponentPacket::LedChannelStatus& out) const;
    bool queueStatus(uint8_t idx, ComponentPacket::LedQueueStatus& out) const;

    bool isEnabled (uint8_t idx) const { return idx < N && _enabled[idx]; }
    bool isPlaying (uint8_t idx) const {
        return idx < N && const_cast<LedEventSeq&>(_sequences[idx]).isPlaying();
    }
    bool isAttached() const { return _attached; }

    // ─────────────────────────────────────────────────────────────────
    // Callbacks
    // ─────────────────────────────────────────────────────────────────

    /// Fires on natural completion of a non-REPEAT queue.  idx is 0-based.
    /// Suppressed for REPEAT queues, for stop()/clear()/reset() (which
    /// are master-initiated, not natural).
    void setQueueDoneCallback(QueueDoneCb cb) { _onQueueDone = std::move(cb); }

    // ─────────────────────────────────────────────────────────────────
    // Direct access (advanced — diagnostics, tests, custom binding)
    // ─────────────────────────────────────────────────────────────────

    LedControlT<TPwm>&       channel(uint8_t idx)       { return _channels[idx]; }
    const LedControlT<TPwm>& channel(uint8_t idx) const { return _channels[idx]; }
    LedEventSeq&       sequence(uint8_t idx)       { return _sequences[idx]; }
    const LedEventSeq& sequence(uint8_t idx) const { return _sequences[idx]; }

    /// Enabled-state bitmask — bit `i` set iff channel `i` is enabled.
    /// Caps at the bit width of uint32_t (32 channels).  STATUS_BROADCAST
    /// summary helper.
    uint32_t enabledFlags() const {
        uint32_t mask = 0;
        for (size_t i = 0; i < N && i < 32; i++) {
            if (_enabled[i]) mask |= (1u << i);
        }
        return mask;
    }

    /// Playing-state bitmask — bit `i` set iff sequence `i` is playing.
    uint32_t playingFlags() const {
        uint32_t mask = 0;
        for (size_t i = 0; i < N && i < 32; i++) {
            if (const_cast<LedEventSeq&>(_sequences[i]).isPlaying()) mask |= (1u << i);
        }
        return mask;
    }

private:
    std::array<LedControlT<TPwm>, N>  _channels{};
    std::array<LedEventSeq, N>        _sequences{};
    std::array<bool, N>               _enabled{};
    std::array<bool, N>               _queueRunning{};   ///< edge guard for one-shot completion
    uint8_t                           _masterBrightness_pct = 100;
    QueueDoneCb                       _onQueueDone;
    bool                              _attached = false;
};

}  // namespace sfx_peripherals

#include "led_runtime.ipp"

#endif  // SFX_LED_RUNTIME_H
