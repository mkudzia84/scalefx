/*
 * LED Manager — Channel Management for Multi-Channel LED Systems
 *
 * Manages N LED channels with per-channel event sequences, enable/disable,
 * and master brightness.  Provides the ILedManager abstract interface so
 * that the LED protocol server can work with any channel count or PWM
 * backend without templates.
 *
 * Template parameters:
 *   MaxChannels  — Number of LED channels (compile-time constant)
 *   TGpio        — PWM backend type (default: NativeGpio); must satisfy
 *                  the PwmOutput concept (pwm/pwm_output.h)
 *
 * Protocol conventions (same as LightFX):
 *   - Channel numbers are 1-based in the public API (matches wire protocol)
 *   - Channel 0 means "all channels" for broadcast operations
 *   - Internal arrays are 0-based
 *
 * Usage (LightFX Pico — 8 native GPIO channels):
 *   LedManager<8> ledManager;                 // = LedManager<8, NativeGpio>
 *   const uint8_t pins[8] = {0,1,2,3,4,5,6,7};
 *   ledManager.begin(pins);                   // uses NativeGpio singleton
 *   ledManager.ledSet(1, 80);                 // ch 1 → 80%
 *   ledManager.seqStart(0);                   // start all
 *   ledManager.update();                      // call in loop()
 *
 * Usage (HubFX ESP32-S3 — 8 PCA9685 channels):
 *   PCA9685 pwm;
 *   pwm.begin(Wire, 0x70);
 *   LedManager<8, PCA9685> ledManager;
 *   const uint8_t pins[8] = {0,1,2,3,4,5,6,7};
 *   ledManager.begin(&pwm, pins);
 *   ledManager.ledSet(3, 50);
 *   ledManager.update();
 *
 * Data structs (LightFxSeqStatus etc.) are in <serial/lightfx/lightfx.h>.
 */

#ifndef LED_MANAGER_H
#define LED_MANAGER_H

#include <Arduino.h>
#include <serial/lightfx/lightfx.h>
#include "led_control.h"
#include "led_event_seq.h"
#include "led_events.h"

// ============================================================================
// ILedManager — Abstract interface for LED channel management
// ============================================================================

/**
 * @brief Abstract interface for multi-channel LED management.
 *
 * Used by LedProtocolServer so it can work with any LedManager<N, TGpio>
 * specialization.  Virtual dispatch overhead is negligible at protocol
 * handling rates (~100 commands/sec max).
 */
class ILedManager {
public:
    virtual ~ILedManager() = default;

    // ========================================================================
    // Info
    // ========================================================================

    /// Number of LED channels managed
    virtual uint8_t channelCount() const = 0;

    /// Check if channel is valid (1-based)
    virtual bool isValidChannel(uint8_t ch) const = 0;

    /// Check if channel is valid or 0 (all)
    virtual bool isValidChannelOrAll(uint8_t ch) const = 0;

    // ========================================================================
    // Operations (return error code, 0 = OK)
    // ========================================================================

    /// Set LED brightness on a single channel (1-based)
    virtual uint8_t ledSet(uint8_t ch, uint8_t brightness) = 0;

    /// Turn off LED channel(s) — 0 = all
    virtual uint8_t ledOff(uint8_t ch) = 0;

    /// Clear event sequence on channel(s) — 0 = all
    virtual uint8_t seqClear(uint8_t ch) = 0;

    /// Add event to a channel's sequence
    virtual uint8_t seqAdd(uint8_t ch, uint8_t eventType,
                           uint16_t p1, uint16_t p2,
                           uint8_t p3, uint8_t p4,
                           uint8_t p5 = 0) = 0;

    /// Start sequence playback — 0 = all
    virtual uint8_t seqStart(uint8_t ch) = 0;

    /// Stop sequence playback — 0 = all
    virtual uint8_t seqStop(uint8_t ch) = 0;

    /// Restart sequence from beginning — 0 = all
    virtual uint8_t seqRestart(uint8_t ch) = 0;

    /// Configure repeat / one-shot mode for a sequence.  When
    /// `repeat == false` the sequence stops after its last event and
    /// `seqIsComplete()` latches true; when `repeat == true` (default
    /// for back-compat with LightFX-style infinite loops) the
    /// sequence loops forever and `seqIsComplete()` always returns
    /// false.  ch == 0 applies to every channel.
    virtual uint8_t seqSetRepeat(uint8_t ch, bool repeat) = 0;

    /// Returns true if a one-shot sequence on `ch` has reached its
    /// last event.  Latches until the next seqStart()/seqRestart() so
    /// callers can poll once and observe the completion edge.  ch
    /// must be 1-based; ch==0 is invalid for this query.
    virtual bool    seqIsComplete(uint8_t ch) const = 0;

    /// Set master brightness percentage (0-100)
    virtual uint8_t setMasterBrightness(uint8_t pct) = 0;

    /// Reset channel(s) to defaults (stop, clear, off, re-enable) — 0 = all
    virtual uint8_t resetChannel(uint8_t ch) = 0;

    /// Enable or disable channel(s) — 0 = all
    virtual uint8_t enableChannel(uint8_t ch, bool enabled) = 0;

    // ========================================================================
    // Queries (fill protocol structs)
    // ========================================================================

    /// Fill sequence status for a channel (1-based)
    virtual void getSeqStatus(uint8_t ch, LightFxSeqStatus& status) = 0;

    /// Fill sequence queue detail for a channel (1-based)
    virtual void getSeqQueue(uint8_t ch, LightFxSeqQueue& queue) = 0;

    /// Fill channel status for a channel (1-based)
    virtual void getChannelStatus(uint8_t ch, LightFxChannelStatus& status) = 0;

    // ========================================================================
    // State
    // ========================================================================

    /// Check if a channel is enabled (1-based)
    virtual bool isChannelEnabled(uint8_t ch) const = 0;

    /// Get current master brightness percentage (0-100)
    virtual uint8_t masterBrightness() const = 0;

    // ========================================================================
    // Tick
    // ========================================================================

    /// Update all active sequences — call in loop()
    virtual void update() = 0;
};

// ============================================================================
// LedManager<MaxChannels, TGpio> — Concrete LED channel manager
// ============================================================================

/**
 * @brief Template LED channel manager with pluggable PWM backend.
 *
 * @tparam MaxChannels  Number of LED channels (compile-time constant)
 * @tparam TGpio        PWM backend type (NativeGpio or PCA9685) —
 *                      must satisfy the PwmOutput concept.
 */
template <uint8_t MaxChannels, typename TGpio = NativeGpio>
class LedManager : public ILedManager {
public:
    LedManager() {
        for (uint8_t i = 0; i < MaxChannels; i++) {
            _enabled[i] = true;
        }
    }

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize channels on a shared PWM backend.
     *
     * Calls channel(i).begin(gpio, pins[i], activeLow, pwm) for each channel
     * and attaches the event sequence to the LED output.
     *
     * @param gpio     Pointer to initialised PWM backend (must outlive manager)
     * @param pins     Array of MaxChannels pin numbers
     * @param activeLow  true if LEDs are active-low
     * @param pwm        true to enable PWM brightness
     */
    void begin(TGpio* gpio, const uint8_t* pins, bool activeLow = false, bool pwm = true) {
        for (uint8_t i = 0; i < MaxChannels; i++) {
            _channels[i].begin(gpio, pins[i], activeLow, pwm);
            _channels[i].off();
            _sequences[i].attachLed(&_channels[i]);
            _enabled[i] = true;
        }
    }

    /**
     * @brief Convenience: initialize channels on native GPIO pins.
     *
     * Uses NativeGpio::instance() as the GPIO provider.
     * Only available when TGpio = NativeGpio (SFINAE).
     *
     * @param pins     Array of MaxChannels pin numbers
     * @param activeLow  true if LEDs are active-low
     * @param pwm        true to enable PWM brightness
     */
    template <typename U = TGpio>
    std::enable_if_t<std::is_same<U, NativeGpio>::value>
    begin(const uint8_t* pins, bool activeLow = false, bool pwm = true) {
        begin(&NativeGpio::instance(), pins, activeLow, pwm);
    }

    /**
     * @brief Finalize setup after channels have been configured externally.
     *
     * For advanced setups where each channel's begin() was called individually,
     * this method attaches event sequences and marks all channels enabled.
     */
    void beginConfigured() {
        for (uint8_t i = 0; i < MaxChannels; i++) {
            _sequences[i].attachLed(&_channels[i]);
            _enabled[i] = true;
        }
    }

    /**
     * @brief Shutdown — stop all sequences and turn off LEDs
     */
    void shutdown() {
        for (uint8_t i = 0; i < MaxChannels; i++) {
            _sequences[i].stop();
            _channels[i].off();
        }
        _masterBrightness_pct = 100;
    }

    /**
     * @brief Safe init — same as shutdown + re-enable all channels
     */
    void init() {
        shutdown();
        for (uint8_t i = 0; i < MaxChannels; i++) {
            _enabled[i] = true;
        }
    }

    // ========================================================================
    // Direct Access (for STATUS data, firmware-level queries)
    // ========================================================================

    /// Access channel by 0-based index
    LedControlT<TGpio>&       channel(uint8_t index)       { return _channels[index]; }
    const LedControlT<TGpio>& channel(uint8_t index) const { return _channels[index]; }

    /// Access sequence by 0-based index
    LedEventSeq&       sequence(uint8_t index)       { return _sequences[index]; }
    const LedEventSeq& sequence(uint8_t index) const { return _sequences[index]; }

    // ========================================================================
    // ILedManager — Info
    // ========================================================================

    uint8_t channelCount() const override { return MaxChannels; }

    bool isValidChannel(uint8_t ch) const override {
        return ch >= 1 && ch <= MaxChannels;
    }

    bool isValidChannelOrAll(uint8_t ch) const override {
        return ch == 0 || (ch >= 1 && ch <= MaxChannels);
    }

    // ========================================================================
    // ILedManager — Operations
    // ========================================================================

    uint8_t ledSet(uint8_t ch, uint8_t brightness) override;
    uint8_t ledOff(uint8_t ch) override;
    uint8_t seqClear(uint8_t ch) override;
    uint8_t seqAdd(uint8_t ch, uint8_t eventType,
                   uint16_t p1, uint16_t p2,
                   uint8_t p3, uint8_t p4,
                   uint8_t p5 = 0) override;
    uint8_t seqStart(uint8_t ch) override;
    uint8_t seqStop(uint8_t ch) override;
    uint8_t seqRestart(uint8_t ch) override;
    uint8_t seqSetRepeat(uint8_t ch, bool repeat) override;
    bool    seqIsComplete(uint8_t ch) const override;
    uint8_t setMasterBrightness(uint8_t pct) override;
    uint8_t resetChannel(uint8_t ch) override;
    uint8_t enableChannel(uint8_t ch, bool enabled) override;

    // ========================================================================
    // ILedManager — Queries
    // ========================================================================

    void getSeqStatus(uint8_t ch, LightFxSeqStatus& status) override;
    void getSeqQueue(uint8_t ch, LightFxSeqQueue& queue) override;
    void getChannelStatus(uint8_t ch, LightFxChannelStatus& status) override;

    // ========================================================================
    // ILedManager — State
    // ========================================================================

    bool isChannelEnabled(uint8_t ch) const override {
        return (ch >= 1 && ch <= MaxChannels) ? _enabled[ch - 1] : false;
    }

    uint8_t masterBrightness() const override { return _masterBrightness_pct; }

    /// Get enabled flags as a bitmask (bit 0 = ch 1)
    uint8_t enabledFlags() const {
        uint8_t flags = 0;
        for (uint8_t i = 0; i < MaxChannels && i < 8; i++) {
            if (_enabled[i]) flags |= (1 << i);
        }
        return flags;
    }

    /// Get sequence-playing flags as a bitmask (bit 0 = ch 1)
    uint8_t seqPlayingFlags() const {
        uint8_t flags = 0;
        for (uint8_t i = 0; i < MaxChannels && i < 8; i++) {
            if (_sequences[i].isPlaying()) flags |= (1 << i);
        }
        return flags;
    }

    // ========================================================================
    // ILedManager — Tick
    // ========================================================================

    void update() override {
        for (uint8_t i = 0; i < MaxChannels; i++) {
            if (_sequences[i].isPlaying()) {
                _sequences[i].update();
            }
        }
    }

private:
    LedControlT<TGpio> _channels[MaxChannels];
    LedEventSeq _sequences[MaxChannels];
    bool _enabled[MaxChannels];
    uint8_t _masterBrightness_pct = 100;
};

// ============================================================================
// Template Implementation
// ============================================================================

#include "led_manager.ipp"

#endif // LED_MANAGER_H
