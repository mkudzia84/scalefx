/*
 * RX Input Common Types — Shared definitions for RC receiver input channels
 *
 * Provides constants, types, and the ChannelSource concept (duck-typed interface)
 * used by PpmInput, PwmCollection, SbusInput, and JetiExChannelSource.
 *
 * ChannelSource concept (duck-typed — no ABC, no virtual):
 *   bool     begin(...)           — source-specific initialization
 *   void     update()             — poll / refresh channel values
 *   void     end()                — stop and release resources
 *   uint16_t channel_us(i)        — channel value in µs (1-based index)
 *   uint8_t  channelCount()       — number of channels currently available
 *   bool     isValid()            — true if signal is present and recent
 *   bool     isInitialized()      — true if begin() succeeded
 */

#ifndef SFX_RX_COMMON_H
#define SFX_RX_COMMON_H

#include <cstdint>

// ════════════════════════════════════════════════════════════════
//  Shared constants for all receiver input types
// ════════════════════════════════════════════════════════════════

namespace RxConfig {
    /// Maximum channels any source can report.  Sized for Jeti EX Bus,
    /// which carries up to 24 proportional channels per frame.  SBUS is
    /// protocol-fixed at 16 (see `SbusConfig::NUM_CHANNELS`); the 24
    /// ceiling is the Jeti / PPM cap.
    constexpr uint8_t MAX_CHANNELS = 24;

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

// ════════════════════════════════════════════════════════════════
//  Futaba S.Bus constants
// ════════════════════════════════════════════════════════════════

namespace SbusConfig {
    constexpr uint32_t BAUD_RATE    = 100000;   ///< 100 kbaud, 8E2, inverted
    constexpr uint8_t  FRAME_SIZE   = 25;       ///< Fixed 25-byte frame
    constexpr uint8_t  START_BYTE   = 0x0F;
    constexpr uint8_t  END_BYTE     = 0x00;     ///< Some receivers use other values
    constexpr uint8_t  NUM_CHANNELS = 16;       ///< 16 proportional (11-bit)
    constexpr uint32_t FRAME_GAP_US = 2500;     ///< Inter-frame gap threshold (µs)

    // Flag byte (byte 23) bit masks
    constexpr uint8_t FLAG_CH17        = 0x01;
    constexpr uint8_t FLAG_CH18        = 0x02;
    constexpr uint8_t FLAG_FRAME_LOST  = 0x04;
    constexpr uint8_t FLAG_FAILSAFE    = 0x08;

    /// Convert 11-bit SBUS raw value to µs
    /// Raw 172→988µs, 992→1500µs, 1811→2012µs
    inline uint16_t rawToUs(uint16_t raw) {
        return static_cast<uint16_t>((raw * 5 + 4) / 8 + 880);
    }
}

// ════════════════════════════════════════════════════════════════
//  Jeti EX Bus constants (see also jeti_ex/jeti_ex_common.h)
// ════════════════════════════════════════════════════════════════

namespace JetiConfig {
    constexpr uint32_t BAUD_LOW     = 125000;   ///< Standard EX Bus baud
    constexpr uint32_t BAUD_HIGH    = 250000;   ///< High-speed EX Bus baud
    constexpr uint8_t  NUM_CHANNELS = 24;       ///< Up to 24 proportional (EX Bus spec)

    /// Convert Jeti raw channel value to µs (value is in 1/8 µs units)
    inline uint16_t rawToUs(uint16_t raw) {
        return static_cast<uint16_t>(raw >> 3);
    }
}

#endif // SFX_RX_COMMON_H
