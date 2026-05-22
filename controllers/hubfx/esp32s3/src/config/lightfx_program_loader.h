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
 * YAML format (leaf objects may use the flow form `{ … }`; the events
 * list stays block since each event carries several fields):
 *
 *   schema_version: 1
 *   channels:
 *     - port: { kind: pwm, idx: 0 }       # required
 *       brightness_pct: 100               # optional (default 100)
 *       events:                           # required, ≥1 entry
 *         - kind: "on"                    # enum name (lower-case)
 *           brightness_pct: 100           # for on / flash / fade_in / fade_out
 *           duration_ms: 0                # for off / fade_* — 0 = terminal
 *           cycle_ms:    0                # for flash / fading / beacon
 *           min_pct:     0                # for fading / beacon
 *           max_pct:     100              # for fading / beacon
 *           flash_pct:   50               # for flash / beacon
 *   landing_bindings:                     # optional
 *     - { id: 0, state: "on" }            # state: on|off
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

/// Parse a single program YAML.  The program's `name` field is set
/// from `programName` (filename minus `.yaml`).  Returns true on
/// successful parse + non-empty channels list.
template <typename TPool>
bool loadLightFxProgram(const char* yaml, size_t len, const char* programName,
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

    // ── channels[] ──────────────────────────────────────────────────
    const auto* root = parser.root();
    const auto* channelsNode = root ? root->child("channels") : nullptr;
    out.numChannels = 0;
    if (!channelsNode || channelsNode->type != YamlNode::Sequence) {
        SFX_LOG_WARN("[lightfx-program] %s: missing or non-sequence `channels`", programName);
        return false;
    }
    const int numCh = channelsNode->childCount();
    for (int i = 0; i < numCh && out.numChannels < kMaxChannelsPerProgram; ++i) {
        const auto* chNode = channelsNode->childAt(i);
        if (!chNode) continue;
        LedChannelSpec& spec = out.channels[out.numChannels];

        // port: {kind, idx, guid?}
        spec.addr = portRefFromNode(chNode->child("port"));
        if (spec.addr.portKind == 0) {
            SFX_LOG_WARN("[lightfx-program] %s ch[%u]: missing or invalid `port`",
                         programName, (unsigned)i);
            continue;
        }

        spec.perChannelBrightnessPct =
            (uint8_t)chNode->template childAs<int32_t>("brightness_pct", 100);

        // events[]
        const auto* evNode = chNode->child("events");
        spec.numEvents = 0;
        if (evNode && evNode->type == YamlNode::Sequence) {
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
        }
        // `loop: true` → the events form a phase-locked repeating pattern
        // (period = Σ duration_ms).  The flag rides on event[0] (the
        // animator reads it there).  Used for the airliner-style
        // single/double-flash patterns where channels share a period but
        // pulse at non-overlapping offsets.
        if (chNode->template childAs<bool>("loop", false) && spec.numEvents > 0) {
            spec.events[0].flags |= LightEventFlags::Loop;
        }

        if (spec.numEvents == 0) {
            // Fallback: a single ON event at the channel's brightness.
            spec.events[0] = LightEvent::on(spec.perChannelBrightnessPct, 0);
            spec.numEvents = 1;
        }

        // INSTRUMENTATION: dump the parsed channel so a mis-parsed event
        // kind (everything defaulting to On, etc.) is visible in the boot
        // / config-reload diag.  ev0.kind: 0=on 1=off 2=flash 3=fadeIn
        // 4=fadeOut 5=fading 6=beacon.
        SFX_LOG_INFO("[lightfx-program] %s ch[%u] port={%u,%u} bright=%u%% events=%u ev0.kind=%u",
                     programName, (unsigned)i,
                     (unsigned)spec.addr.portKind, (unsigned)spec.addr.portIdx,
                     (unsigned)spec.perChannelBrightnessPct,
                     (unsigned)spec.numEvents,
                     (unsigned)spec.events[0].kind);

        out.numChannels++;
    }

    // ── landing_bindings[] (optional) ───────────────────────────────
    out.numLandings = 0;
    const auto* lbNode = root->child("landing_bindings");
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
