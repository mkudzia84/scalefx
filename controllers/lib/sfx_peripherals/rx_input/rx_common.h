/*
 * RX Input Common Types — Shared definitions for RC receiver input channels
 *
 * Provides constants, types, and the ChannelSource concept (duck-typed interface)
 * that both PpmInput<TPolicy> and PwmCollection implement.
 *
 * ChannelSource concept (duck-typed — no ABC, no virtual):
 *   bool     begin(...)           — platform-specific initialization
 *   void     update()             — poll / refresh channel values
 *   void     end()                — stop and release resources
 *   uint16_t channel_us(i)        — channel value in µs (1-based index)
 *   uint8_t  channelCount()       — number of channels currently available
 *   bool     isValid()            — true if signal is present and recent
 *   bool     isInitialized()      — true if begin() succeeded
 */

#ifndef SFX_RX_COMMON_H
#define SFX_RX_COMMON_H

#include <Arduino.h>
#include <cstdint>

namespace RxConfig {
    /// Maximum channels any source can report
    constexpr uint8_t MAX_CHANNELS = 16;

    /// PPM frame gap threshold (µs) — pulse > this starts a new frame
    constexpr uint16_t PPM_SYNC_US = 3000;

    /// Minimum valid channel pulse width (µs)
    constexpr uint16_t MIN_PULSE_US = 800;

    /// Maximum valid channel pulse width (µs)
    constexpr uint16_t MAX_PULSE_US = 2200;

    /// Center / neutral pulse width (µs)
    constexpr uint16_t CENTER_US = 1500;

    /// Signal-loss timeout (ms) — no valid frame for this long → invalid
    constexpr uint32_t SIGNAL_TIMEOUT_MS = 500;

    /// Default PPM polarity (true = rising edge starts pulse)
    constexpr bool PPM_RISING_EDGE = true;
}

#endif // SFX_RX_COMMON_H
