/*
 * apply_hubfx_config.h — fan parsed configs out to the matching service.
 *
 *   One apply function per sub-config file.  Each one is the onLoaded
 *   callback of its store (and also runs as the schema-defaults fallback
 *   when the file is missing).  /hubfx.yaml only carries the master
 *   enable matrix + audio.codec_supply — everything else lives in its
 *   own dedicated YAML.
 *
 *   File graph (canonical paths — each ConfigStore returns its own
 *   `defaultPath()`):
 *
 *     /hubfx.yaml      → applyHubFxConfig    (features matrix + audio)
 *     /alerts.yaml     → applyAlertsConfig   (severity → AlertSound)
 *     /enginefx.yaml   → applyEngineFxConfig (engine wiring + sounds)
 *     /landing.yaml    → applyLandingConfig  (landing-light defs)
 *     /lightfx.yaml    → applyLightFxConfig  (brightness + program paths)
 *                          ↳ loads /lightfx/programs/<n>.yaml × N
 */

#ifndef HUBFX_APPLY_CONFIG_H
#define HUBFX_APPLY_CONFIG_H

#include <cstdio>
#include <cstring>
#include <memory>

#include <platform/sfx_platform.h>   // sfxPsramMalloc / sfxPsramFree
#include <serial/diag_log.h>
#include <serial/core/core.h>        // SfxWire::putU16LE for the servo profile cfg payload
#include <motion/servo_profile_wire.h>  // ServoProfileWire::pack — the one servo-profile codec

#include "hubfx_config.h"
#include "alerts_config.h"
#include "enginefx_config.h"
#include "gunfx_config.h"
#include "landing_config.h"
#include "gearcontrol_config.h"
#include "lightfx_config.h"
#include "lightfx_program_loader.h"
#include "../effects/lightfx/led_program.h"
#include "../effects/alerts/alert_sound.h"   // alertSoundPath()

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)
#include <audio/audio_asset_cache.h>
#endif

namespace hubfx::config {

/// Static buffer for the parsed program list — lives outside the
/// service so apply-time reloads can rebuild without reallocating.
inline hubfx::effects::lightfx::Program  kHubFxLoadedPrograms[kMaxProgramRefs];

/// Extract the program name from a full LittleFS path.  Strips
/// directory components (everything up to and including the last `/`)
/// and a trailing `.yaml`.  e.g.
///   `/lightfx/programs/helicopter_off.yaml`  →  "helicopter_off"
inline void deriveProgramName(const char* path, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
    const char* slash = std::strrchr(path, '/');
    const char* base  = slash ? slash + 1 : path;
    size_t n = std::strlen(base);
    if (n >= 5 && std::strcmp(base + n - 5, ".yaml") == 0) n -= 5;
    else if (n >= 4 && std::strcmp(base + n - 4, ".yml") == 0) n -= 4;
    if (n >= outLen) n = outLen - 1;
    std::memcpy(out, base, n);
    out[n] = '\0';
}

/// Load every program file referenced by `cfg.programPaths[]`, parse
/// into `kHubFxLoadedPrograms[]`, and return how many parsed
/// successfully.  Missing files are WARN'd and skipped — the rest still
/// load.  Reads from the supplied file-reader (typically
/// `storageReadFile<FlashModule>`).
///
/// `hub` is the /hubfx.yaml store (required for v2 channel-name
/// resolution — each program's tracks[] entries resolve channel names
/// against `hub.ports[]` LedAnimator labels).  Pre-v2 programs ignore
/// it.  Pass nullptr only when /hubfx.yaml hasn't loaded yet (would
/// cause every v2 track to skip with a WARN).
inline uint8_t loadLightFxProgramCatalog(const LightFxYamlConfig& cfg,
                                          const HubFxConfig* hub,
                                          int (*reader)(const char*, char*, size_t)) {
    using namespace hubfx::effects::lightfx;
    if (!reader) {
        SFX_LOG_WARN("[lightfx-program] no file reader bound — skipping catalog load");
        return 0;
    }
    // Whole-file read buffer in PSRAM.  The ESP32-S3 has 8 MB PSRAM, so a
    // 64 KB buffer is free — and it removes the file-size ceiling for good.
    // The old 2 KB DRAM buffer silently truncated programs ≥ 2 KB mid-file
    // (helicopter_flight is 2157 B): the tail channels lost their events
    // and fell back to a single ON event (lights stuck on).  Allocated per
    // call + freed; the apply path runs only at boot / config-reload.
    // (Pico has no PSRAM — sfxPsramMalloc falls back to malloc there, but
    // this catalog loader is HubFX-only.)
    constexpr size_t kReadBufSize = 64 * 1024;
    char* yamlBuf = static_cast<char*>(sfxPsramMalloc(kReadBufSize));
    if (!yamlBuf) {
        SFX_LOG_WARN("[lightfx-program] read buffer alloc failed (%u B) — catalog skipped",
                     (unsigned)kReadBufSize);
        return 0;
    }

    uint8_t loaded = 0;
    char name[32];
    for (uint8_t i = 0; i < cfg.numPrograms && loaded < kMaxProgramRefs; ++i) {
        const char* path = cfg.programPaths[i];
        deriveProgramName(path, name, sizeof(name));
        int n = reader(path, yamlBuf, kReadBufSize);
        if (n <= 0) {
            SFX_LOG_WARN("[lightfx-program] file not found: %s", path);
            continue;
        }
        kHubFxLoadedPrograms[loaded] = Program{};
        if (loadLightFxProgram<LightFxProgramYamlPool>(yamlBuf, (size_t)n, name,
                                                       hub,
                                                       cfg.channels, cfg.numChannels,
                                                       kHubFxLoadedPrograms[loaded])) {
            SFX_LOG_INFO("[lightfx-program] loaded %s from %s (%d bytes, %u channels)",
                         name, path, n,
                         (unsigned)kHubFxLoadedPrograms[loaded].numChannels);
            loaded++;
        }
    }
    sfxPsramFree(yamlBuf);
    return loaded;
}

/// Build the PortRef for a mapping entry — hub-local when `guid[0] == 0`,
/// otherwise the expander addressed by the stamped GUID.
inline hubfx::effects::PortRef portRefOf(const PortMapping& m) {
    return m.guid[0] ? hubfx::effects::PortRef::remote(m.guid, m.kind, m.idx)
                     : hubfx::effects::PortRef::local(m.kind, m.idx);
}

/// Serialise a port mapping's role-attach config payload into `cfgBuf`
/// (>= ServoProfileWire::kSize bytes) and return its length.  The ONE place
/// that knows which roles carry config: a servo profile (Rule 42) or the
/// JetiEX attach flags.  Shared by the per-port attach path AND the bulk
/// block builder so the two can never drift.
inline uint8_t packRoleCfg(const PortMapping& m, uint8_t* cfgBuf) {
    if (m.profileSet && m.role == RoleKind::ServoActuator) {
        return (uint8_t)sfx_core::ServoProfileWire::pack(cfgBuf, m.profile);
    }
    if (m.role == RoleKind::EscTelemetry) {
        // [protocol][baudKHi][baudKLo][ratioHi][ratioLo] — baud 0 = protocol
        // default; ratio = RPM divider ×100 (0 = 1.00).
        cfgBuf[0] = m.escProtocol;
        cfgBuf[1] = 0; cfgBuf[2] = 0;
        cfgBuf[3] = (uint8_t)(m.escRpmRatioX100 >> 8);
        cfgBuf[4] = (uint8_t)(m.escRpmRatioX100 & 0xFF);
        return 5;
    }
    if (m.role == RoleKind::JetiExInput) {
        // [broadcastHz][baudHi][baudLo][downstream] — 0,0,0 = no auto-broadcast
        // + default 125000 baud; byte 3 is RESERVED (the IN_2 telemetry monitor
        // is autodetect now — the attach handler ignores this byte).
        cfgBuf[0] = 0; cfgBuf[1] = 0; cfgBuf[2] = 0;
        cfgBuf[3] = 0;
        return 4;
    }
    return 0;
}

/// Serialise the role config for one expander `guid` into a ROLE_BULK_ATTACH
/// payload: `[count:u8] × [portKind][portIdx][roleKind][cfgLen][cfg]`.  Returns
/// the byte length written (always >= 1).  count==0 (just the length byte) is
/// VALID — a board with no /hubfx.yaml roles (a fresh/unconfigured expander)
/// produces an empty block, and the caller simply skips the push.  This is the
/// DECLARATIVE bringup path that replaces N per-port forwards.
inline size_t buildRoleBlockForGuid(const HubFxConfig& cfg, const char* guid,
                                    uint8_t* out, size_t maxLen) {
    if (maxLen < 1 || !guid || !guid[0]) { if (maxLen) out[0] = 0; return maxLen ? 1 : 0; }
    size_t  off   = 1;   // out[0] = count, written at the end
    uint8_t count = 0;
    for (uint8_t i = 0; i < cfg.numPorts; ++i) {
        const auto& m = cfg.ports[i];
        if (m.role == RoleKind::None || m.guid[0] == 0) continue;
        if (std::strncmp(m.guid, guid, sizeof(m.guid)) != 0) continue;
        uint8_t cfgBuf[sfx_core::ServoProfileWire::kSize];
        const uint8_t cfgLen = packRoleCfg(m, cfgBuf);
        if (off + 4 + cfgLen > maxLen) {
            SFX_LOG_WARN("[hubfx-config] role block for %s overflow at %u entries",
                         guid, (unsigned)count);
            break;
        }
        out[off++] = m.kind;
        out[off++] = m.idx;
        out[off++] = m.role;
        out[off++] = cfgLen;
        for (uint8_t b = 0; b < cfgLen; ++b) out[off++] = cfgBuf[b];
        ++count;
    }
    out[0] = count;
    return off;
}

/// Attach roles for every `ports[]` entry whose GUID matches `guid`
/// (pass "" for the hub-local subset).  Returns the count attached.
/// Idempotent — re-attaching the same role is a no-op on topology, so
/// this is safe to call repeatedly (boot + each expander connect).
template <typename TTopology>
uint8_t attachPortRolesForGuid(TTopology& topo, const HubFxConfig& cfg,
                               const char* guid) {
    const bool wantHub = (!guid || !guid[0]);
    uint8_t attached = 0;
    for (uint8_t i = 0; i < cfg.numPorts; ++i) {
        const auto& m = cfg.ports[i];
        if (m.role == RoleKind::None) continue;
        const bool isHub = (m.guid[0] == 0);
        if (wantHub != isHub) continue;
        if (!wantHub && std::strncmp(m.guid, guid, sizeof(m.guid)) != 0) continue;
        // Servo profile (Rule 42) / JetiEX flags serialised by the shared
        // packRoleCfg — same codec the bulk block uses, so they can't drift.
        uint8_t cfgBuf[sfx_core::ServoProfileWire::kSize];  // 13; widest cfg payload here
        const uint8_t cfgLen = packRoleCfg(m, cfgBuf);
        if (topo.attachRole(portRefOf(m), m.role, cfgLen ? cfgBuf : nullptr, cfgLen)) {
            ++attached;
            SFX_LOG_INFO("[hubfx-config] %s:{%s, %u} → %s%s%s",
                         m.guid[0] ? m.guid : "hub",
                         PortKind::getName(m.kind), (unsigned)m.idx,
                         RoleKind::getName(m.role),
                         m.label[0] ? "  // " : "",
                         m.label[0] ? m.label : "");
        } else {
            // Remote board not yet mounted ⇒ deferred (re-applied from the
            // expander connect callback).  Hub-local failures are real.
            if (m.guid[0]) {
                SFX_LOG_INFO("[hubfx-config] %s:{%s, %u} → %s  deferred (board offline)",
                             m.guid, PortKind::getName(m.kind), (unsigned)m.idx,
                             RoleKind::getName(m.role));
            } else {
                SFX_LOG_WARN("[hubfx-config] hub:{%s, %u} → %s  FAILED",
                             PortKind::getName(m.kind), (unsigned)m.idx,
                             RoleKind::getName(m.role));
            }
        }
    }
    return attached;
}

/// Attach every `ports[]` entry's RoleKind to its PortRef (hub-local AND
/// HUB-LOCAL ports only.  Expander roles are NOT attached here — they're
/// pushed declaratively to each board at connect via the ROLE_BULK_ATTACH
/// bringup path (ExpanderService onReady → applyRoleConfig), so the bolt-on
/// per-expander loop that used to live here is gone (it forwarded N single
/// attaches that raced on reconnect AND never updated the hub's roster).
/// If the table is empty (no `/hubfx.yaml`, or `ports:` missing), fall back to
/// attaching `LedAnimator` on every hub-local PWM port — the common "out of
/// the box" HubFX configuration.  Idempotent.
template <typename TBoard, typename TTopology>
void applyPortRoles(TBoard& board, const HubFxConfig& cfg) {
    using hubfx::effects::PortRef;
    auto& topo = board.template policy<TTopology>();

    if (cfg.numPorts == 0) {
        uint8_t attached = 0;
        for (uint8_t ch = 0; ch < 8; ++ch) {
            if (topo.attachRole(PortRef::local(PortKind::Pwm, ch),
                                RoleKind::LedAnimator)) {
                ++attached;
            }
        }
        SFX_LOG_INFO("[hubfx-config] no ports[] in /hubfx.yaml — fallback: "
                     "attached LedAnimator to %u/8 hub PWM ports",
                     (unsigned)attached);
        return;
    }

    const uint8_t attached = attachPortRolesForGuid(topo, cfg, "");  // hub-local ""
    SFX_LOG_INFO("[hubfx-config] hub-local ports: %u attached (%u expander board(s) "
                 "declared — pushed at connect)",
                 (unsigned)attached, (unsigned)cfg.numExpanders);
}

// NOTE (2026-07-26): `applyHubFxConfig` (the `/hubfx.yaml` `features:`
// master-enable matrix) was RETIRED.  Each effect's enable now lives ONLY in
// its own sub-config (`enginefx.yaml` `enabled:` …), applied by that store's
// callback; hubfx.yaml is pure port/input/audio mapping.  The runtime
// capability mask is refreshed by `board.recomputeEnabledCapabilities()` in
// `reassertHubFxFeatures`, which runs LAST on every load/reload.

/// Apply `/lightfx.yaml` — load the program catalog from the explicit
/// paths in `cfg.programPaths[]`, then configure the service.  Master
/// brightness + local enable land here.
///
/// `hub` is required for v2 channel-name resolution — programs
/// reference LED channels by NAME against /hubfx.yaml's LedAnimator
/// port labels (mirrors Rule 43's pattern for inputs).  The sketch's
/// callback passes `kHubFx.data()`.
template <typename TBoard, typename TLightFxService>
void applyLightFxConfig(TBoard& board, const LightFxYamlConfig& cfg,
                        const HubFxConfig& hub,
                        int (*programReader)(const char*, char*, size_t)) {
    const uint8_t loaded = loadLightFxProgramCatalog(cfg, &hub, programReader);
    auto& svc = board.template policy<TLightFxService>();
    svc.setEnabled(cfg.enabled);
    svc.controller().setMasterBrightness(cfg.masterBrightnessPct);
    svc.configure(kHubFxLoadedPrograms, loaded);
    board.recomputeEnabledCapabilities();
    SFX_LOG_INFO("[lightfx-config] applied — enabled=%s brightness=%u%% programs=%u/%u",
                 cfg.enabled ? "on" : "off",
                 (unsigned)cfg.masterBrightnessPct,
                 (unsigned)loaded, (unsigned)cfg.numPrograms);
}

/// Apply `/alerts.yaml` — full severity catalog → AlertService.
/// Preloads every referenced sound into the AudioAssetCache via the
/// owner-token API (owner = the AlertService instance).  On reload,
/// paths that disappeared from the config get released; new paths get
/// queued for background load.  See AudioAssetCache::setPreloadsForOwner.
template <typename TBoard, typename TAlertService>
void applyAlertsConfig(TBoard& board, const AlertsConfig& cfg) {
    auto& svc = board.template policy<TAlertService>();
    svc.configure(toAlertServiceConfig(cfg));
    board.recomputeEnabledCapabilities();
    SFX_LOG_INFO("[alerts-config] applied — enabled=%s  info=%s warning=%s error=%s critical=%s",
                 cfg.enabled ? "on" : "off",
                 cfg.info.soundName,    cfg.warning.soundName,
                 cfg.error.soundName,   cfg.critical.soundName);

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)
    // Resolve each severity's snake_name → AlertSound → on-disk path
    // (`/sounds/sys/<name>.mp3`).  Skip slots with no sound bound and
    // any names that don't match an AlertSound enum.
    using namespace hubfx::effects::alerts;
    const AlertsConfig::Severity* slots[] = {
        &cfg.info, &cfg.warning, &cfg.error, &cfg.critical,
    };
    const char* paths[8];
    int n = 0;
    for (const auto* s : slots) {
        if (!s || !s->soundName[0]) continue;
        const AlertSound id = alertSoundFromName(s->soundName);
        if (id == AlertSound::None) continue;
        const char* p = alertSoundPath(id);
        if (p && p[0]) paths[n++] = p;
    }
    // Voltage alert lives in its own field with the same {soundName} shape.
    if (cfg.voltageAlert.soundName[0]) {
        const AlertSound id = alertSoundFromName(cfg.voltageAlert.soundName);
        if (id != AlertSound::None) {
            const char* p = alertSoundPath(id);
            if (p && p[0]) paths[n++] = p;
        }
    }
    AudioAssetCache::instance().setPreloadsForOwner(&svc, "alerts", paths, n);
#endif
}

/// Apply `/enginefx.yaml` — full wiring + sound pack → EngineFxService.
/// Resolves `toggle.input` against `/hubfx.yaml`'s `inputs[]` to get the
/// throttle source PortRef + channel + threshold.  `hub` is the parsed
/// /hubfx.yaml store; the sketch's callback passes it in.
template <typename TBoard, typename TEngineService>
void applyEngineFxConfig(TBoard& board, const EngineFxYamlConfig& cfg,
                          const HubFxConfig& hub) {
    auto& svc = board.template policy<TEngineService>();
    auto resolved = toEngineFxServiceConfig(cfg, hub);
    svc.configure(resolved);
    board.recomputeEnabledCapabilities();
    SFX_LOG_INFO("[enginefx-config] applied — enabled=%s type=%s "
                 "input='%s' (resolved {%s, %u} ch=%u thr=%uus)  "
                 "out=%s  starting=%s",
                 cfg.enabled ? "on" : "off", cfg.type,
                 cfg.toggle.input[0] ? cfg.toggle.input : "(none)",
                 PortKind::getName(resolved.rcInput.portKind),
                 (unsigned)resolved.rcInput.portIdx,
                 (unsigned)resolved.inputChannel,
                 (unsigned)resolved.thresholdUs,
                 audioOutputMaskName(cfg.outputMask),
                 cfg.sounds.starting);

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)
    // Preload the engine sound pack (start / loop / stop).
    const char* paths[3];
    int n = 0;
    if (cfg.sounds.starting[0]) paths[n++] = cfg.sounds.starting;
    if (cfg.sounds.running [0]) paths[n++] = cfg.sounds.running;
    if (cfg.sounds.stopping[0]) paths[n++] = cfg.sounds.stopping;
    AudioAssetCache::instance().setPreloadsForOwner(&svc, "enginefx", paths, n);
#endif
}

/// Apply `/gunfx.yaml` — full per-gun spec table → GunFxService
/// (Phase 2 + Rule 43 named-channel resolver).
///
/// Rule 43: each gun's `triggerInput`, `rofSelectorInput`,
/// `yaw.inputName`, `pitch.inputName` carries a NAME from
/// /hubfx.yaml's `inputs:` block.  We resolve those names against the
/// parsed HubFxConfig BEFORE handing the spec list to the service —
/// the service stores resolved PortRef + channel; it never sees the
/// name.  Unknown names log a WARN and leave the binding empty (the
/// service skips the dispatcher subscribe).
template <typename TBoard, typename TGunFxService>
void applyGunFxConfig(TBoard& board, const GunFxYamlConfig& cfg,
                      const HubFxConfig& hub) {
    // Walk a mutable copy of the spec table so we can write resolved
    // port + channel fields without touching the parsed YAML.
    //
    // PSRAM-allocate instead of stacking `auto resolved = cfg;` —
    // GunFxYamlConfig is ~kMaxGuns * sizeof(GunSpec) ≈ 4 KB, and the
    // CONFIG_RELOAD path runs UNDER board.process() (much deeper call
    // stack than setup() does at boot), tipping loopTask past its 8 KB
    // cap and triple-faulting the apply chain.  Boot's shallow call
    // depth hides the issue; reload reproduces it every time on builds
    // with smoke heater + fan + audio preloads configured.
    //
    // Use PSRAM (8 MB on HubFX) over DRAM heap (~111 KB free at boot
    // baseline) — same lesson as the ConfigStore read buffer + YAML
    // parser pools (Phase 4 polish 2026-05-27): keep the cramped
    // DRAM/DMA-cap pool for the things that actually need it (DMA
    // descriptors, ring buffers, BLE/Wi-Fi).  A bare `new` lands in
    // DRAM by default; `sfxPsramMalloc` + placement-new + custom
    // deleter is the canonical pattern (matches AudioAssetCache and
    // YamlParser::allocatePools).  Pico falls back to malloc.
    struct PsramDeleter {
        void operator()(GunFxYamlConfig* p) const noexcept {
            if (!p) return;
            p->~GunFxYamlConfig();
            sfxPsramFree(p);
        }
    };
    auto* resolvedRaw = static_cast<GunFxYamlConfig*>(
        sfxPsramMalloc(sizeof(GunFxYamlConfig)));
    if (!resolvedRaw) {
        SFX_LOG_ERROR("[gunfx-config] PSRAM alloc failed (%u B) — apply skipped",
                      (unsigned)sizeof(GunFxYamlConfig));
        return;
    }
    new (resolvedRaw) GunFxYamlConfig(cfg);
    std::unique_ptr<GunFxYamlConfig, PsramDeleter> resolvedOwner(resolvedRaw);
    auto& resolved = *resolvedOwner;
    auto resolveInput = [&hub](const char* name,
                               hubfx::effects::PortRef& port,
                               uint8_t& channel,
                               const char* context, uint8_t gunId) {
        if (!name || !name[0]) {
            port = {};
            channel = 0;
            return;
        }
        const InputBinding* b = findInputByName(hub, name);
        if (!b) {
            SFX_LOG_WARN("[gunfx-config] gun[%u].%s='%s' not in /hubfx.yaml "
                         "inputs[] — binding disabled",
                         (unsigned)gunId, context, name);
            port = {};
            channel = 0;
            return;
        }
        port    = b->port;
        channel = (b->channelId > 0) ? (uint8_t)(b->channelId - 1) : 0;
    };
    for (uint8_t i = 0; i < resolved.numGuns; ++i) {
        auto& g = resolved.guns[i];
        resolveInput(g.triggerInput,                g.triggerPort,                 g.triggerChannel,                "trigger.input",      g.id);
        resolveInput(g.rofSelectorInput,            g.rofSelectorPort,             g.rofSelectorChannel,            "rof.input",          g.id);
        resolveInput(g.yaw.inputName,               g.yaw.inputPort,               g.yaw.inputChannel,              "yaw.input",          g.id);
        resolveInput(g.pitch.inputName,             g.pitch.inputPort,             g.pitch.inputChannel,            "pitch.input",        g.id);
        // Phase 4 polish 2026-05-26: heater activation channel
        // (Rule 43).  When the YAML omits `smoke.heater.activation.input`
        // the field stays empty and the service skips the dispatcher
        // subscribe, leaving the heater permanently allowed.
        resolveInput(g.smoke.heaterActivationInput, g.smoke.heaterActivationPort,  g.smoke.heaterActivationChannel, "smoke.heater.activation.input", g.id);
    }
    auto& svc = board.template policy<TGunFxService>();
    svc.configure(resolved.guns, resolved.numGuns);
    svc.setEnabled(resolved.enabled);
    board.recomputeEnabledCapabilities();
    SFX_LOG_INFO("[gunfx-config] applied — enabled=%s, %u gun(s)",
                 resolved.enabled ? "on" : "off", (unsigned)resolved.numGuns);

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)
    // Preload every distinct per-shot sound across all guns × ROF bands.
    // kMaxGuns × kMaxRofItems = 4 × 8 = 32 slots is the worst case; the
    // table dedups by path so shared sounds (e.g. same WAV across two
    // bands) only pin one entry.
    using namespace hubfx::effects::gunfx;
    const char* paths[kMaxGuns * kMaxRofItems];
    int n = 0;
    auto already = [&](const char* p) {
        for (int i = 0; i < n; ++i) {
            if (std::strcmp(paths[i], p) == 0) return true;
        }
        return false;
    };
    for (uint8_t gi = 0; gi < resolved.numGuns; ++gi) {
        const auto& g = resolved.guns[gi];
        for (uint8_t ri = 0; ri < g.numRofItems; ++ri) {
            const char* p = g.rofItems[ri].soundPath;
            if (p && p[0] && !already(p)) paths[n++] = p;
        }
    }
    AudioAssetCache::instance().setPreloadsForOwner(&svc, "gunfx", paths, n);
#endif
}

/// Apply `/landing.yaml` — landing-light def table → LandingLightService.
template <typename TBoard, typename TLandingService>
void applyLandingConfig(TBoard& board, const LandingConfig& cfg) {
    board.template policy<TLandingService>().configure(cfg.lights, cfg.numLights);
    board.recomputeEnabledCapabilities();
    SFX_LOG_INFO("[landing-config] applied — %u landing light(s) configured",
                 (unsigned)cfg.numLights);
}

/// Apply `/gearcontrol.yaml` — gear def table → GearControlService.
template <typename TBoard, typename TGearService>
void applyGearControlConfig(TBoard& board, const GearControlConfig& cfg) {
    auto& svc = board.template policy<TGearService>();
    // Door servo roles are attached via /hubfx.yaml's ports block (ServoActuator);
    // the gear addresses them by PortRef.  Pass the gear table + coord mode through.
    svc.configure(cfg.gears, cfg.numGears, cfg.coordMode);
    svc.setSounds(cfg.sounds.deploy, cfg.sounds.retract, cfg.sounds.outputMask);
    svc.setEnabled(cfg.enabled);
    svc.setDeployOnConnectionLoss(cfg.deployOnConnectionLoss);  // item 6
    // The RC up/down channel (cfg.activation) is wired by the sketch's
    // GearActivationDriver re-install, not here — the driver owns the
    // dispatcher subscription (same split as the landing-activation driver).
    board.recomputeEnabledCapabilities();
    SFX_LOG_INFO("[gearcontrol-config] applied — enabled=%s, coord=%u, %u gear(s), input=%s, sounds=%s/%s",
                 cfg.enabled ? "on" : "off", (unsigned)cfg.coordMode, (unsigned)cfg.numGears,
                 cfg.activation.input[0] ? cfg.activation.input : "(manual)",
                 cfg.sounds.deploy[0]  ? "set" : "-",
                 cfg.sounds.retract[0] ? "set" : "-");
}

}  // namespace hubfx::config

#endif  // HUBFX_APPLY_CONFIG_H
