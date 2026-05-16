/*
 * PwmChannelLedOutput<TPwmSink> — adapter from a PwmCollection
 * channel to the existing `ILedOutput` interface in
 * sfx_peripherals/led/led_control.h.
 *
 * Templated on the sink type (which must satisfy the `PwmLedSink`
 * concept), so the cross-coupling resolves entirely at compile time —
 * no `IPwmLedSink*` virtual dispatch.  Inherits from `ILedOutput`
 * because that's the existing contract the LedEventSeq runtime drives
 * its outputs through; refactoring `ILedOutput` to a concept is a
 * separate (larger) change that touches every existing slave's LED
 * driver.
 *
 * Usage:
 *   PwmCollection<6, MySensePolicy>           pwms;
 *   PwmChannelLedOutput<decltype(pwms)>       trigger_led_output;
 *   trigger_led_output.attach(&pwms, /*channel*\/2);
 *
 *   // wire into existing LedControl machinery just like a
 *   // LedControl<TGpio> instance.
 *
 * Wire mapping:
 *   ILedOutput::setBrightness(0..255)  →  PwmCollection duty 0..1000
 *   ILedOutput::on()                    →  duty 1000 (full)
 *   ILedOutput::off()                   →  duty 0
 */

#ifndef SFX_PWM_CHANNEL_LED_OUTPUT_H
#define SFX_PWM_CHANNEL_LED_OUTPUT_H

#include <cstdint>
#include <sfx_peripherals/led/led_control.h>      // ILedOutput

#include "pwm_led_sink.h"

namespace sfx_peripherals {

template <PwmLedSink TPwmSink>
class PwmChannelLedOutput : public ILedOutput {
public:
    PwmChannelLedOutput() = default;

    /// Bind to a PwmCollection (via the PwmLedSink concept) + the
    /// channel index within that collection.  The channel must be in
    /// PwmLed mode for writes to land — `PwmCollection::writeDuty`
    /// bypasses the mode gate (the LED runtime is the authoritative
    /// caller while the channel sits in PwmLed mode), so technically
    /// this works regardless, but the ports won't be in the right
    /// hardware state if the mode hasn't been switched.
    void attach(TPwmSink* sink, uint8_t pwmIdx) {
        _sink = sink;
        _idx  = pwmIdx;
    }

    void detach()                  { _sink = nullptr; }
    bool isAttached() const        { return _sink != nullptr; }

    // ── ILedOutput interface ─────────────────────────────────────────

    void setBrightness(uint8_t brightness) override {
        if (!_sink) return;
        // 0..255 → 0..1000 duty thousandths.  Linear here because the
        // upstream gamma mapping (in LedEventSeq's event classes) has
        // already shaped the curve for perceptual uniformity.
        uint16_t duty = (uint16_t)((uint32_t)brightness * 1000u / 255u);
        _sink->writeDuty(_idx, duty);
    }

    void on() override {
        if (_sink) _sink->writeDuty(_idx, 1000);
    }

    void off() override {
        if (_sink) _sink->writeDuty(_idx, 0);
    }

private:
    TPwmSink* _sink = nullptr;
    uint8_t   _idx  = 0;
};

}  // namespace sfx_peripherals

#endif  // SFX_PWM_CHANNEL_LED_OUTPUT_H
