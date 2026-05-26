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

#include <platform/sfx_platform.h>   // sfxPsramMalloc / sfxPsramFree
#include <serial/diag_log.h>
#include <serial/core/core.h>        // SfxWire::putU16LE for the servo profile cfg payload

#include "hubfx_config.h"
#include "alerts_config.h"
#include "enginefx_config.h"
#include "gunfx_config.h"
#include "landing_config.h"
#include "gearcontrol_config.h"
#include "lightfx_config.h"
#include "lightfx_program_loader.h"
#include "../effects/lightfx/led_program.h"

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
        // Rule 42 storage + Rule 44 editing-surface: when the port has
        // a servo profile attached (set by Studio's GunFx panel into
        // /hubfx.yaml), serialise it into the role-attach payload so
        // `RoleServicePolicy::attachServoActuator` applies it directly.
        // Layout matches the cfg parser in role_service.cpp.
        uint8_t cfgBuf[13];
        uint8_t cfgLen = 0;
        if (m.profileSet && m.role == RoleKind::ServoActuator) {
            SfxWire::putU16LE(&cfgBuf[0],  m.profile.minUs);
            SfxWire::putU16LE(&cfgBuf[2],  m.profile.maxUs);
            SfxWire::putU16LE(&cfgBuf[4],  m.profile.maxSpeedUsPerSec);
            cfgBuf[6]                    = m.profile.inverted ? 1 : 0;
            SfxWire::putU16LE(&cfgBuf[7],  m.profile.centerUs);
            SfxWire::putU16LE(&cfgBuf[9],  m.profile.maxAccelUsPerSec2);
            SfxWire::putU16LE(&cfgBuf[11], m.profile.maxJerkUsPerSec3);
            cfgLen = 13;
        }
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
/// any currently-mounted expander port).  Expander ports whose board is
/// offline are skipped here and (re)attached from the ExpanderService
/// connect callback via `attachPortRolesForGuid`.  If the table is empty
/// (no `/hubfx.yaml`, or `ports:` block missing), fall back to attaching
/// `LedAnimator` on every hub-local PWM port — the common "out of the
/// box" HubFX configuration.  Idempotent.
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

    // Hub-local first (always succeeds), then each declared expander
    // (succeeds only for boards already mounted; the rest defer).
    uint8_t attached = attachPortRolesForGuid(topo, cfg, "");
    for (uint8_t e = 0; e < cfg.numExpanders; ++e) {
        attached += attachPortRolesForGuid(topo, cfg, cfg.expanders[e].guid);
    }
    SFX_LOG_INFO("[hubfx-config] ports: %u/%u attached (%u expander board(s) declared)",
                 (unsigned)attached, (unsigned)cfg.numPorts,
                 (unsigned)cfg.numExpanders);
}

/// Apply `/hubfx.yaml` — flip every service's `setEnabled()` to match
/// the master matrix.  Idempotent: called once after /hubfx.yaml loads
/// (so the audio.codec_supply caller in the sketch fires early) and
/// again after every sub-file has loaded (so the master matrix has the
/// last word over each sub-file's local `enabled:` flag).
template <typename TBoard,
          typename TAlertService,
          typename TEngineService,
          typename TLightFxService,
          typename TLandingService,
          typename TGearService,
          typename TGunFxService>
void applyHubFxConfig(TBoard& board, const HubFxConfig& cfg) {
    const auto& f = cfg.features;
    board.template policy<TAlertService>()  .setEnabled(f.alerts);
    board.template policy<TEngineService>() .setEnabled(f.enginefx);
    board.template policy<TLightFxService>().setEnabled(f.lightfx);
    board.template policy<TLandingService>().setEnabled(f.landingLights);
    board.template policy<TGearService>()   .setEnabled(f.gears);
    board.template policy<TGunFxService>()  .setEnabled(f.gunfx);
    board.recomputeEnabledCapabilities();

    SFX_LOG_INFO("[hubfx-config] features: alerts=%s enginefx=%s lightfx=%s "
                 "landing=%s gears=%s gunfx=%s",
                 f.alerts        ? "on" : "off",
                 f.enginefx      ? "on" : "off",
                 f.lightfx       ? "on" : "off",
                 f.landingLights ? "on" : "off",
                 f.gears         ? "on" : "off",
                 f.gunfx         ? "on" : "off");
}

/// Apply `/lightfx.yaml` — load the program catalog from the explicit
/// paths in `cfg.programPaths[]`, then configure the service.  Master
/// brightness + local enable land here; /hubfx.yaml's master kill-
/// switch re-applies after via `applyHubFxConfig`.
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
template <typename TBoard, typename TAlertService>
void applyAlertsConfig(TBoard& board, const AlertsConfig& cfg) {
    board.template policy<TAlertService>().configure(toAlertServiceConfig(cfg));
    board.recomputeEnabledCapabilities();
    SFX_LOG_INFO("[alerts-config] applied — enabled=%s  info=%s warning=%s error=%s critical=%s",
                 cfg.enabled ? "on" : "off",
                 cfg.info.soundName,    cfg.warning.soundName,
                 cfg.error.soundName,   cfg.critical.soundName);
}

/// Apply `/enginefx.yaml` — full wiring + sound pack → EngineFxService.
/// Resolves `toggle.input` against `/hubfx.yaml`'s `inputs[]` to get the
/// throttle source PortRef + channel + threshold.  `hub` is the parsed
/// /hubfx.yaml store; the sketch's callback passes it in.
template <typename TBoard, typename TEngineService>
void applyEngineFxConfig(TBoard& board, const EngineFxYamlConfig& cfg,
                          const HubFxConfig& hub) {
    auto resolved = toEngineFxServiceConfig(cfg, hub);
    board.template policy<TEngineService>().configure(resolved);
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
    auto resolved = cfg;
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
    svc.configure(cfg.gears, cfg.numGears);
    svc.setEnabled(cfg.enabled);
    board.recomputeEnabledCapabilities();
    SFX_LOG_INFO("[gearcontrol-config] applied — enabled=%s, %u gear(s) configured",
                 cfg.enabled ? "on" : "off", (unsigned)cfg.numGears);
}

}  // namespace hubfx::config

#endif  // HUBFX_APPLY_CONFIG_H
