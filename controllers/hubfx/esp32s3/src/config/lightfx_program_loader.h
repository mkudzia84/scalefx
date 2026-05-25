/*
 * lightfx_program_loader.h — parse a `/lightfx/programs/<Name>.yaml`
 * file into a `hubfx::effects::lightfx::Program` struct.
 *
 * One file per program (Phase 3-4 of the config rollout).  `hubfx.yaml`
 * references program names; this loader resolves each name to
 * `/lightfx/programs/<name>.yaml` and walks the YAML node tree by hand
 * (the declarative schema DSL doesn't compose nested sequences cleanly,
 * and a program has channels[].events[]).
 *
 * ─── Schema v2 (current, recommended) ────────────────────────────────
 *
 * Programs reference LED channels BY NAME — the channel name resolves
 * against `/hubfx.yaml`'s `ports:` block (each PortMapping has a
 * `label:` field that's the channel handle).  This mirrors Rule 43 —
 * inputs are already addressed by name; v2 extends the same convention
 * to OUTPUT ports so programs become device-portable and port rewires
 * stay localised to /hubfx.yaml.
 *
 *   schema_version: 2
 *   tracks:
 *     - channel: "Red beacon"          # matches PortMapping.label
 *       loop: true                     # phase-locked repeating pattern
 *       brightness_pct: 100            # optional per-track override
 *       events:
 *         - kind: fade_in
 *           duration_ms: 300
 *           brightness_pct: 100
 *         - kind: fade_out
 *           duration_ms: 300
 *         - kind: "off"
 *           duration_ms: 400
 *   landing_bindings:                  # optional, same as v1
 *     - { id: 0, state: "on" }
 *
 * ─── Schema v1 (legacy, accepted on load) ────────────────────────────
 *
 * Inline port + events per channel.  Still parsed for backward
 * compatibility with on-device YAMLs that haven't been re-saved by
 * Studio yet.  Studio Save always emits v2.  v1 ignores the
 * hub-config port table (no name lookup needed — port is inline).
 *
 *   schema_version: 1                  # or absent (default v1)
 *   channels:
 *     - port: { kind: pwm, idx: 0 }
 *       brightness_pct: 100
 *       events: [ … same event shape as v2 … ]
 */

#ifndef HUBFX_LIGHTFX_PROGRAM_LOADER_H
#define HUBFX_LIGHTFX_PROGRAM_LOADER_H

#include <cstdint>
#include <cstring>

#include <config/yaml_parser.h>
#include <serial/diag_log.h>
#include <serial/ports.h>                       // PortKind::*

#include "../effects/effect_id.h"               // PortRef
#include "../effects/lightfx/led_program.h"     // Program / LedChannelSpec
#include "../effects/lightfx/light_event.h"     // LightEvent / LightEventKind
#include "port_ref_yaml.h"                      // portRefFromNode
#include "hubfx_config.h"                       // HubFxConfig, PortMapping

namespace hubfx::config {

/// Per-program YAML pool — events arrays push the node count up but
/// the top-level structure is still small.
struct LightFxProgramYamlPool {
    static constexpr size_t MAX_NODES        = 512;
    static constexpr size_t STRING_POOL_SIZE = 4096;
    static constexpr size_t MAX_DEPTH        = 16;
};

/// Map snake-case kind names to the firmware-side enum.  Returns
/// `LightEventKind::On` on unknown names + logs a WARN (silent
/// fallback so a typo doesn't break the whole program).
inline hubfx::effects::lightfx::LightEventKind eventKindFromName(const char* name) {
    using K = hubfx::effects::lightfx::LightEventKind;
    if (!name || !name[0])                      return K::On;
    if (std::strcmp(name, "on")        == 0)    return K::On;
    if (std::strcmp(name, "off")       == 0)    return K::Off;
    if (std::strcmp(name, "flash")     == 0)    return K::Flash;
    if (std::strcmp(name, "fade_in")   == 0)    return K::FadeIn;
    if (std::strcmp(name, "fade_out")  == 0)    return K::FadeOut;
    if (std::strcmp(name, "fading")    == 0)    return K::Fading;
    if (std::strcmp(name, "beacon")    == 0)    return K::Beacon;
    SFX_LOG_WARN("[lightfx-program] unknown event kind '%s' — using 'on'", name);
    return K::On;
}

/// Resolve a v2 channel NAME against /hubfx.yaml's port table.  Only
/// LedAnimator-roled ports participate — picking a name that's wired
/// to e.g. a Heater would silently steer light events at a heater.
/// Returns true + writes `out` on match; false on miss (caller WARNs
/// + skips the track).  Linear scan — N ≤ 48 (kMaxPortMappings), and
/// program load is a one-shot at boot / config reload.
inline bool resolveLightFxChannel(const HubFxConfig& hub, const char* name,
                                  hubfx::effects::PortRef& out) {
    if (!name || !name[0]) return false;
    for (uint8_t i = 0; i < hub.numPorts; ++i) {
        const PortMapping& m = hub.ports[i];
        if (m.role != RoleKind::LedAnimator) continue;
        if (std::strcmp(m.label, name) != 0) continue;
        out = m.guid[0]
            ? hubfx::effects::PortRef::remote(m.guid, m.kind, m.idx)
            : hubfx::effects::PortRef::local(m.kind, m.idx);
        return true;
    }
    return false;
}

/// Parse one event-list YAML node (the `events:` sequence on a channel
/// or track) into `spec`.  Shared by v1 and v2 paths — the event shape
/// is identical between the two schemas.  Returns the number of events
/// written (capped at `kMaxEventsPerChannel`).  Not templated — YamlNode
/// is a plain type (the parser is the template; the node tree it
/// produces is shared).
inline uint8_t parseLightFxEvents(const YamlNode* evNode,
                                  hubfx::effects::lightfx::LedChannelSpec& spec) {
    using namespace hubfx::effects::lightfx;
    spec.numEvents = 0;
    if (!evNode || evNode->type != YamlNode::Sequence) return 0;
    const int numEv = evNode->childCount();
    for (int j = 0; j < numEv && spec.numEvents < kMaxEventsPerChannel; ++j) {
        const auto* e = evNode->childAt(j);
        if (!e) continue;
        LightEvent& lev = spec.events[spec.numEvents];
        lev.kind          = eventKindFromName(e->template childAs<const char*>("kind", "on"));
        lev.durationMs    = (uint16_t)e->template childAs<int32_t>("duration_ms",   0);
        lev.cycleMs       = (uint16_t)e->template childAs<int32_t>("cycle_ms",      0);
        lev.brightnessPct = (uint8_t) e->template childAs<int32_t>("brightness_pct", 100);
        lev.minPct        = (uint8_t) e->template childAs<int32_t>("min_pct",        0);
        lev.maxPct        = (uint8_t) e->template childAs<int32_t>("max_pct",      100);
        lev.flashPct      = (uint8_t) e->template childAs<int32_t>("flash_pct",     50);
        lev.flags         = 0;
        spec.numEvents++;
    }
    return spec.numEvents;
}

/// Parse a single program YAML.  The program's `name` field is set
/// from `programName` (filename minus `.yaml`).  Returns true on
/// successful parse + non-empty channels list.
///
/// `hub` is the live /hubfx.yaml store — required for v2 channel-name
/// resolution.  Passing nullptr (or a hub with zero LedAnimator
/// ports) restricts the parser to v1 (channels[]) mode; v2 tracks
/// silently skip with a WARN per unresolved name.
template <typename TPool>
bool loadLightFxProgram(const char* yaml, size_t len, const char* programName,
                        const HubFxConfig* hub,
                        hubfx::effects::lightfx::Program& out) {
    using namespace hubfx::effects::lightfx;
    using hubfx::effects::PortRef;
    using hubfx::config::portRefFromNode;

    YamlParser<TPool> parser;
    if (!parser.parse(yaml, len)) {
        SFX_LOG_WARN("[lightfx-program] %s: YAML parse failed (%s)",
                     programName, parser.error());
        return false;
    }

    // ── name ────────────────────────────────────────────────────────
    std::memset(out.name, 0, sizeof(out.name));
    std::strncpy(out.name, programName, sizeof(out.name) - 1);

    // ── Schema-version routing ──────────────────────────────────────
    // Default to v1 when the field is absent (legacy on-device files
    // predate the schema_version: 2 emit).  An explicit `tracks:` key
    // forces v2 even without the version field — defensive against a
    // hand-edited file missing the header.
    const auto* root = parser.root();
    const int32_t version = root ? root->template childAs<int32_t>("schema_version", 1) : 1;
    const bool hasTracks  = root && root->child("tracks") != nullptr;
    const bool useV2      = (version >= 2) || hasTracks;

    out.numChannels = 0;

    if (useV2) {
        // ── v2: tracks[] keyed by channel NAME, resolved via /hubfx.yaml ──
        const auto* tracksNode = root ? root->child("tracks") : nullptr;
        if (!tracksNode || tracksNode->type != YamlNode::Sequence) {
            SFX_LOG_WARN("[lightfx-program] %s (v2): missing or non-sequence `tracks`", programName);
            return false;
        }
        const int numTracks = tracksNode->childCount();
        for (int i = 0; i < numTracks && out.numChannels < kMaxChannelsPerProgram; ++i) {
            const auto* trkNode = tracksNode->childAt(i);
            if (!trkNode) continue;

            const char* chanName = trkNode->template childAs<const char*>("channel", "");
            if (!chanName || !chanName[0]) {
                SFX_LOG_WARN("[lightfx-program] %s tr[%u]: missing `channel` name", programName, (unsigned)i);
                continue;
            }
            LedChannelSpec& spec = out.channels[out.numChannels];

            // Resolve name → PortRef via /hubfx.yaml LedAnimator ports.
            // No match → operator hasn't wired this channel yet; skip
            // the track but keep the rest of the program intact.
            if (!hub || !resolveLightFxChannel(*hub, chanName, spec.addr)) {
                SFX_LOG_WARN("[lightfx-program] %s: channel '%s' not found among LedAnimator ports in /hubfx.yaml — skipping track",
                             programName, chanName);
                continue;
            }

            spec.perChannelBrightnessPct =
                (uint8_t)trkNode->template childAs<int32_t>("brightness_pct", 100);

            parseLightFxEvents(trkNode->child("events"), spec);
            if (trkNode->template childAs<bool>("loop", false) && spec.numEvents > 0) {
                spec.events[0].flags |= LightEventFlags::Loop;
            }
            if (spec.numEvents == 0) {
                spec.events[0] = LightEvent::on(spec.perChannelBrightnessPct, 0);
                spec.numEvents = 1;
            }
            SFX_LOG_INFO("[lightfx-program] %s tr[%u] channel='%s' → port={%u,%u} bright=%u%% events=%u ev0.kind=%u",
                         programName, (unsigned)i, chanName,
                         (unsigned)spec.addr.portKind, (unsigned)spec.addr.portIdx,
                         (unsigned)spec.perChannelBrightnessPct,
                         (unsigned)spec.numEvents,
                         (unsigned)spec.events[0].kind);
            out.numChannels++;
        }
    } else {
        // ── v1: channels[] with inline port refs ────────────────────
        const auto* channelsNode = root ? root->child("channels") : nullptr;
        if (!channelsNode || channelsNode->type != YamlNode::Sequence) {
            SFX_LOG_WARN("[lightfx-program] %s (v1): missing or non-sequence `channels`", programName);
            return false;
        }
        const int numCh = channelsNode->childCount();
        for (int i = 0; i < numCh && out.numChannels < kMaxChannelsPerProgram; ++i) {
            const auto* chNode = channelsNode->childAt(i);
            if (!chNode) continue;
            LedChannelSpec& spec = out.channels[out.numChannels];

            spec.addr = portRefFromNode(chNode->child("port"));
            if (spec.addr.portKind == 0) {
                SFX_LOG_WARN("[lightfx-program] %s ch[%u]: missing or invalid `port`",
                             programName, (unsigned)i);
                continue;
            }
            spec.perChannelBrightnessPct =
                (uint8_t)chNode->template childAs<int32_t>("brightness_pct", 100);

            parseLightFxEvents(chNode->child("events"), spec);
            if (chNode->template childAs<bool>("loop", false) && spec.numEvents > 0) {
                spec.events[0].flags |= LightEventFlags::Loop;
            }
            if (spec.numEvents == 0) {
                spec.events[0] = LightEvent::on(spec.perChannelBrightnessPct, 0);
                spec.numEvents = 1;
            }
            SFX_LOG_INFO("[lightfx-program] %s ch[%u] port={%u,%u} bright=%u%% events=%u ev0.kind=%u",
                         programName, (unsigned)i,
                         (unsigned)spec.addr.portKind, (unsigned)spec.addr.portIdx,
                         (unsigned)spec.perChannelBrightnessPct,
                         (unsigned)spec.numEvents,
                         (unsigned)spec.events[0].kind);
            out.numChannels++;
        }
    }

    // ── landing_bindings[] (optional, identical shape v1 and v2) ────
    out.numLandings = 0;
    const auto* lbNode = root ? root->child("landing_bindings") : nullptr;
    if (lbNode && lbNode->type == YamlNode::Sequence) {
        const int n = lbNode->childCount();
        for (int i = 0; i < n && out.numLandings < kMaxLandingBindingsPerProgram; ++i) {
            const auto* item = lbNode->childAt(i);
            if (!item) continue;
            LandingLightBinding& b = out.landings[out.numLandings];
            b.id    = (uint8_t)item->template childAs<int32_t>("id", 0);
            const char* state = item->template childAs<const char*>("state", "off");
            b.state = (state && std::strcmp(state, "on") == 0) ? 1 : 0;
            out.numLandings++;
        }
    }

    if (out.numChannels == 0) {
        SFX_LOG_WARN("[lightfx-program] %s: parsed zero channels — skipping", programName);
        return false;
    }
    return true;
}

}  // namespace hubfx::config

#endif  // HUBFX_LIGHTFX_PROGRAM_LOADER_H
