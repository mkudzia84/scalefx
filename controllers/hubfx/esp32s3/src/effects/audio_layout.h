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
    /// A/B pair mirrors the EngineFx pattern — two guns can fire at
    /// the same time without stealing each other's channel.  GunFx
    /// routes shots by gun id: even ids → GunA, odd ids → GunB.
    /// (Was a single `Gun0` originally; bumped to a pair after we
    /// found the literal-2 collision with EngineB on 2026-05-23.)
    static constexpr uint8_t GunA    = 3;
    static constexpr uint8_t GunB    = 4;

    // ── GearControl ────────────────────────────────────────────────
    /// Undercarriage transit sounds — the looping deploy/retract motor
    /// whine the GearControlService starts when any gear begins moving
    /// and stops when the whole set settles.  (Was the unnamed
    /// `Reserved` slot; allocated to gear 2026-06-11.)
    ///
    /// AUDIO_MAX_CHANNELS dropped 8 → 6 in Phase 4 polish (2026-05-27)
    /// to free PSRAM for the AudioAssetCache budget.  The former
    /// Spare0/Spare1 slots (6, 7) were retired with no audio consumer
    /// ever assigned to them.
    static constexpr uint8_t Gear = 5;

    static_assert(Gear < AUDIO_MAX_CHANNELS,
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
        HubFxLayout::GunA,    HubFxLayout::GunB,    HubFxLayout::Gear,
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
