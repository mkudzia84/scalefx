/*
 * Output Channel Mask — YAML ↔ Bitmask Bridge
 *
 * Provides OutputChannelMask wrapper type and YamlScalarConvert specialization
 * so the declarative schema DSL can parse human-readable strings ("all", "ch1",
 * "ch2", "ch1+ch2") directly into a uint8_t bitmask.
 *
 * In code: stored and used as uint8_t via implicit conversion.
 * In config.yaml: written as readable strings.
 *
 * Depends on:
 *   - yaml_parser.h (YamlScalarConvert base template)
 *   - hubfx_audio.h (AudioChannel namespace)
 */

#ifndef AUDIO_OUTPUT_CONVERT_H
#define AUDIO_OUTPUT_CONVERT_H

#include <cstring>
#include <config/yaml_parser.h>
#include "../hubfx_audio.h"

// ============================================================================
// OutputChannelMask — Thin Wrapper for Type-Safe YAML Conversion
// ============================================================================

/// Wraps a uint8_t channel bitmask so it gets its own YamlScalarConvert
/// specialization without affecting all uint8_t fields globally.
/// Implicit conversions make it transparent where uint8_t is expected.
struct OutputChannelMask {
    uint8_t value = AudioChannel::ALL;

    constexpr OutputChannelMask() = default;
    constexpr OutputChannelMask(uint8_t v) : value(v) {}

    constexpr operator uint8_t() const { return value; }
};

// ============================================================================
// YAML String → Bitmask Parser
// ============================================================================

/**
 * Parse human-readable channel string to bitmask.
 *
 * Accepted formats (case-insensitive):
 *   "all"      → CH1 | CH2  (0x03)
 *   "ch1"      → CH1        (0x01)
 *   "ch2"      → CH2        (0x02)
 *   "ch1+ch2"  → CH1 | CH2  (0x03)
 *   "none"     → 0x00
 *
 * Extensible: digits 3-8 map to future CH3-CH8 bits.
 *
 * @param str  YAML scalar string
 * @return Bitmask value, or AudioChannel::ALL if unrecognized
 */
inline uint8_t parseOutputChannels(const char* str) {
    if (!str || str[0] == '\0') return AudioChannel::ALL;

    // Quick single-word checks
    char c0 = str[0];
    if (c0 == 'a' || c0 == 'A') return AudioChannel::ALL;   // "all"
    if (c0 == 'n' || c0 == 'N') return 0;                    // "none"

    // Parse "ch1", "ch2", "ch1+ch2" etc. by scanning for channel digits
    uint8_t mask = 0;
    for (const char* p = str; *p; ++p) {
        switch (*p) {
            case '1': mask |= AudioChannel::CH1; break;
            case '2': mask |= AudioChannel::CH2; break;
            // Future: case '3': mask |= 0x04; break;
            // Future: case '4': mask |= 0x08; break;
            default: break;
        }
    }
    return mask ? mask : AudioChannel::ALL;
}

// ============================================================================
// YamlScalarConvert Specialization
// ============================================================================

/// Parse YAML string ("all"/"ch1"/"ch2"/"ch1+ch2") → OutputChannelMask.
template<>
struct YamlScalarConvert<OutputChannelMask> {
    static OutputChannelMask parse(const char* s, OutputChannelMask d) {
        if (!s || s[0] == '\0') return d;
        return OutputChannelMask{parseOutputChannels(s)};
    }
};

#endif // AUDIO_OUTPUT_CONVERT_H
