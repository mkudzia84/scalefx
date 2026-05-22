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

#include <serial/diag_log.h>

#include "hubfx_config.h"
#include "alerts_config.h"
#include "enginefx_config.h"
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
inline uint8_t loadLightFxProgramCatalog(const LightFxYamlConfig& cfg,
                                          int (*reader)(const char*, char*, size_t)) {
    using namespace hubfx::effects::lightfx;
    if (!reader) {
        SFX_LOG_WARN("[lightfx-program] no file reader bound — skipping catalog load");
        return 0;
    }
    uint8_t loaded = 0;
    char name[32];
    char yamlBuf[2048];
    for (uint8_t i = 0; i < cfg.numPrograms && loaded < kMaxProgramRefs; ++i) {
        const char* path = cfg.programPaths[i];
        deriveProgramName(path, name, sizeof(name));
        int n = reader(path, yamlBuf, sizeof(yamlBuf));
        if (n <= 0) {
            SFX_LOG_WARN("[lightfx-program] file not found: %s", path);
            continue;
        }
        kHubFxLoadedPrograms[loaded] = Program{};
        if (loadLightFxProgram<LightFxProgramYamlPool>(yamlBuf, (size_t)n, name,
                                                       kHubFxLoadedPrograms[loaded])) {
            SFX_LOG_INFO("[lightfx-program] loaded %s from %s (%d bytes, %u channels)",
                         name, path, n,
                         (unsigned)kHubFxLoadedPrograms[loaded].numChannels);
            loaded++;
        }
    }
    return loaded;
}

/// Attach each `ports[]` entry's RoleKind to its hub-local PortRef.
/// If the table is empty (no `/hubfx.yaml` on flash, or `ports:` block
/// missing), fall back to attaching `LedAnimator` on every hub-local
/// PWM port + `ServoActuator` on every servo port — the most common
/// "out of the box" HubFX configuration.  Idempotent — re-attaching
/// the same role is a no-op on the topology service.
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

    uint8_t attached = 0;
    for (uint8_t i = 0; i < cfg.numPorts; ++i) {
        const auto& m = cfg.ports[i];
        if (m.role == RoleKind::None) continue;
        if (topo.attachRole(PortRef::local(m.kind, m.idx), m.role)) {
            ++attached;
            SFX_LOG_INFO("[hubfx-config] port {%s, %u} → %s%s%s",
                         PortKind::getName(m.kind), (unsigned)m.idx,
                         RoleKind::getName(m.role),
                         m.label[0] ? "  // " : "",
                         m.label[0] ? m.label : "");
        } else {
            SFX_LOG_WARN("[hubfx-config] port {%s, %u} → %s  FAILED",
                         PortKind::getName(m.kind), (unsigned)m.idx,
                         RoleKind::getName(m.role));
        }
    }
    SFX_LOG_INFO("[hubfx-config] ports: %u/%u attached", (unsigned)attached,
                 (unsigned)cfg.numPorts);
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
template <typename TBoard, typename TLightFxService>
void applyLightFxConfig(TBoard& board, const LightFxYamlConfig& cfg,
                        int (*programReader)(const char*, char*, size_t)) {
    const uint8_t loaded = loadLightFxProgramCatalog(cfg, programReader);
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
