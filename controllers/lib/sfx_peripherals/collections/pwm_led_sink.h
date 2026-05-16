/*
 * PwmLedSink — C++20 concept for a PWM source that the LED runtime
 * can adopt as an output.
 *
 * No polymorphism — `PwmCollection<N, TSense>` satisfies this concept
 * directly (writeDuty / pwmChannelCount / isInLedMode are concrete
 * methods on the template).  `LedCollection<K, TGpio, TPwmSink>` is
 * templated on the sink type so the cross-coupling between the two
 * collections happens entirely at compile time.
 *
 *   board firmware:
 *     PwmCollection<6, MySensePolicy>            pwms;   // satisfies PwmLedSink
 *     LedCollection<8, NativeGpio, decltype(pwms)> leds;  // sink type bound at compile time
 *
 * A type satisfies `PwmLedSink` when it provides:
 *
 *   uint8_t pwmChannelCount()              const   — how many channels exist
 *   bool    isInLedMode    (uint8_t idx)   const   — true iff that channel is currently in PwmLed mode
 *   void    writeDuty      (uint8_t idx, uint16_t duty)  — drive duty (0..1000 thousandths)
 *
 * `writeDuty` bypasses any mode gate inside the sink — the LED runtime
 * is the authoritative caller while a channel sits in PwmLed mode and
 * must be allowed to drive the duty without per-call mode checks.
 */

#ifndef SFX_PWM_LED_SINK_H
#define SFX_PWM_LED_SINK_H

#include <concepts>
#include <cstdint>

namespace sfx_peripherals {

template <typename T>
concept PwmLedSink = requires(T& s, uint8_t idx, uint16_t duty) {
    { s.pwmChannelCount()       } -> std::convertible_to<uint8_t>;
    { s.isInLedMode(idx)        } -> std::convertible_to<bool>;
    { s.writeDuty(idx, duty)    };
};

/// Zero-overhead default for boards / LED collections that have no
/// PWM-borrowed extension outputs.  Reports zero channels so the
/// LedCollection's PWM-borrowed code paths short-circuit cleanly.
struct NullPwmSink {
    constexpr uint8_t pwmChannelCount()        const { return 0; }
    constexpr bool    isInLedMode(uint8_t)     const { return false; }
    constexpr void    writeDuty(uint8_t, uint16_t)   {}
};
static_assert(PwmLedSink<NullPwmSink>);

}  // namespace sfx_peripherals

#endif  // SFX_PWM_LED_SINK_H
