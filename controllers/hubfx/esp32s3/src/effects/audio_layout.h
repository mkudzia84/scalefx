/*
 * audio_layout.h — per-board mixer-channel allocation for HubFX.
 *
 *   `HubFxLayout` names every audio channel slot on this board's
 *   TAS5825P 8-channel mixer.  Renaming or renumbering a slot is a
 *   one-line edit here; every effect references symbols, never raw
 *   integers.  Compile-time `static_assert` rejects duplicate slot
 *   numbers and out-of-range allocations.
 *
 *   The channel-NUMBER is BOARD-fixed (hardware layout).  Per-track
 *   STEREO ROUTING (L / R / both) is EFFECT-config-driven and lives
 *   in each effect's `outputMask` field — see `EngineFxConfig`,
 *   `GunDef`, etc.
 *
 *   When a new audio-emitting effect lands, add its slots here and
 *   the compile-time uniqueness check catches any collision.
 */

#ifndef HUBFX_AUDIO_LAYOUT_H
#define HUBFX_AUDIO_LAYOUT_H

#include <cstddef>
#include <cstdint>

#include <audio/audio_config.h>          // AUDIO_MAX_CHANNELS

namespace hubfx::effects::audio {

struct HubFxLayout {
    // ── System ─────────────────────────────────────────────────────
    /// Channel 0 — system alerts: battery low, connection lost,
    /// errors, success chimes, future voice messages.  Owned by
    /// `AlertServicePolicy`.
    static constexpr uint8_t Alert   = 0;

    // ── EngineFx ───────────────────────────────────────────────────
    /// A/B flip-flop pair for startup → running → stopping.  EngineA
    /// hosts the startup and shutdown sounds; EngineB hosts the
    /// running loop (so cross-fade in v2 can play both for ~500 ms).
    static constexpr uint8_t EngineA = 1;
    static constexpr uint8_t EngineB = 2;

    // ── GunFx ──────────────────────────────────────────────────────
    /// One gun slot for now.  Multi-gun support adds Gun1, Gun2 etc.
    /// here without touching effect code.
    static constexpr uint8_t Gun0    = 3;

    // ── Spare ──────────────────────────────────────────────────────
    /// 4..7 reserved for future ambient / music / voice / mission
    /// audio.  Drop-in: rename one of these as new effects land.
    static constexpr uint8_t Spare0  = 4;
    static constexpr uint8_t Spare1  = 5;
    static constexpr uint8_t Spare2  = 6;
    static constexpr uint8_t Spare3  = 7;

    static_assert(Spare3 < AUDIO_MAX_CHANNELS,
                  "audio layout exceeds mixer width — bump AUDIO_MAX_CHANNELS or drop a slot");
};

// Compile-time uniqueness check — duplicate slot numbers above would
// silently let two effects fight over the same channel.  Lives at
// namespace scope (rather than inside the struct) so it can reference
// the now-complete `HubFxLayout` type.
namespace detail {
constexpr bool hubFxLayoutNoDuplicates() {
    constexpr uint8_t slots[] = {
        HubFxLayout::Alert,   HubFxLayout::EngineA, HubFxLayout::EngineB,
        HubFxLayout::Gun0,    HubFxLayout::Spare0,  HubFxLayout::Spare1,
        HubFxLayout::Spare2,  HubFxLayout::Spare3,
    };
    constexpr size_t n = sizeof(slots) / sizeof(slots[0]);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            if (slots[i] == slots[j]) return false;
        }
    }
    return true;
}
static_assert(hubFxLayoutNoDuplicates(),
              "duplicate channel number in HubFxLayout — every slot must be unique");
}  // namespace detail

}  // namespace hubfx::effects::audio

#endif  // HUBFX_AUDIO_LAYOUT_H
