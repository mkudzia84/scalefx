/*
 * gunfx_config.h — `/gunfx.yaml` schema (Phase 2 of GunFX rollout,
 *                  instructions/22).
 *
 *   Full per-gun wiring: trigger + ROF selector + ROF item table +
 *   muzzle flash + recoil + smoke (heater + fan) + yaw / pitch.  Each
 *   port is a `PortRef` (board alias + kind + idx) addressed against
 *   /hubfx.yaml's `inputs[]` / `expanders[]` resolver tables.
 *
 *   Element voltage + scaling for the heater + fan elements live ON
 *   THE ROLE LAYER (HeaterRole / DcMotorRole, configured via
 *   /hubfx.yaml's `ports[]` role-attach config) — NOT here.  The gun
 *   only sees the intent layer ("heat to T °C", "puff for X ms").
 *
 *   YAML shape (greenfield — no Rule 11 compat for this file):
 *
 *     schema_version: 1
 *     enabled: true
 *     guns:
 *       - id: 0
 *         name: "main"
 *         trigger:
 *           input: gun_trigger          # NAMED channel from /hubfx.yaml inputs[] (Rule 43)
 *           threshold_us: 1500
 *           hysteresis_us: 25
 *         rof:
 *           input: gun_rof              # NAMED channel from /hubfx.yaml inputs[]
 *           items:
 *             - { name: burst,  band: [900, 1200],  rpm: 120, sound: /sounds/gun/burst.mp3 }
 *             - { name: normal, band: [1200, 1600], rpm: 600, sound: /sounds/gun/fire.mp3  }
 *             - { name: rapid,  band: [1600, 2000], rpm: 900, sound: /sounds/gun/rapid.mp3 }
 *         muzzle_flash:
 *           port: { kind: pwm, idx: 0 }
 *           duration_ms: 30
 *           brightness:  100
 *         recoil:
 *           port: { kind: servo, idx: 0 }
 *           center_us: 1500
 *           jerk_us:   200
 *           hold_ms:   80
 *           # servo slew (min/max/speed/accel/jerk) lives on the
 *           # ServoActuatorRole — declare it in /hubfx.yaml ports[]
 *         smoke:
 *           heater:
 *             port: { kind: pwm, idx: 1 }
 *             element_mv: 6000    # rated element voltage (default 6 V)
 *             mode: continuous    # continuous | cycle
 *             cycle_on_ms:  5000  # cycle: heater on-phase duration
 *             cycle_off_ms: 3000  # cycle: heater off-phase duration
 *             activation:         # Rule 43 — optional named-channel gate
 *               input: smoke_arm
 *               threshold_us:  1500
 *               hysteresis_us: 25
 *           fan:
 *             port: { kind: pwm, idx: 2 }
 *             element_mv: 6000    # rated motor voltage (default 6 V)
 *             mode: pulse         # continuous | pulse (sinusoidal envelope, 50% base → 100% peak per shot)
 *             pulse_duration_ms: 100  # one sinusoid lasts this long; default 100 ms = 600 RPM period
 *         yaw:
 *           enabled: true
 *           servo_port: { kind: servo, idx: 1 }
 *           input:      gun_yaw          # NAMED channel from /hubfx.yaml inputs[] (Rule 43)
 *           neutral_us: 1500             # commanded when no input bound
 *           # servo motion shape (clamp/speed/accel/jerk) lives on the
 *           # ServoActuatorRole — declare it in /hubfx.yaml ports[]
 *         pitch: { … same shape as yaw … }
 *
 *   Nested seqs (guns[].rof.items[]) — direct YamlNode traversal,
 *   same pattern as landing_config.h.
 */

// NOTE: distinct include guard from
// controllers/hubfx/esp32s3/src/effects/gunfx/gunfx_config.h — both
// files used to share HUBFX_GUNFX_CONFIG_H, which silently dropped this
// file's body when the firmware-side header was included first.
#ifndef HUBFX_GUNFX_CONFIG_YAML_H
#define HUBFX_GUNFX_CONFIG_YAML_H

#include <cstdint>
#include <cstring>

#include <config/yaml_parser.h>
#include <serial/diag_log.h>
#include <serial/ports.h>

#include "../effects/effect_id.h"
#include "../effects/gunfx/gunfx_config.h"
#include "../effects/gunfx/gunfx_service.h"   // kMaxGuns
#include "port_ref_yaml.h"                    // portRefFromNode

struct GunFxYamlPool {
    static constexpr size_t MAX_NODES        = 768;     // 4 guns × ~190 nodes worst-case
    static constexpr size_t STRING_POOL_SIZE = 4096;
    static constexpr size_t MAX_DEPTH        = 12;
};

/// Parsed form of `/gunfx.yaml`. The arrays use the firmware-side
/// types verbatim (no DTO indirection) so apply is a single
/// `service.configure(g.guns, g.numGuns)` call.
struct GunFxYamlConfig {
    static constexpr uint8_t kSchemaVersion = 1;

    bool     enabled = false;
    uint8_t  numGuns = 0;
    hubfx::effects::gunfx::GunSpec guns[hubfx::effects::gunfx::kMaxGuns] = {};
};

// ─── Enum string-mapping helpers ────────────────────────────────────

inline uint8_t gunfxHeaterModeFromName(const char* s) {
    using namespace hubfx::effects::gunfx;
    if (!s || !s[0])                          return SmokeConfig::HM_CONTINUOUS;
    if (std::strcmp(s, "continuous")  == 0)   return SmokeConfig::HM_CONTINUOUS;
    if (std::strcmp(s, "cycle")       == 0)   return SmokeConfig::HM_CYCLE;
    SFX_LOG_WARN("[gunfx-config] heater mode '%s' unknown — defaulting to continuous", s);
    return SmokeConfig::HM_CONTINUOUS;
}

inline uint8_t gunfxFanModeFromName(const char* s) {
    using namespace hubfx::effects::gunfx;
    if (!s || !s[0])                                  return SmokeConfig::FN_PULSE;
    if (std::strcmp(s, "continuous")          == 0)   return SmokeConfig::FN_CONTINUOUS;
    if (std::strcmp(s, "pulse")               == 0)   return SmokeConfig::FN_PULSE;
    SFX_LOG_WARN("[gunfx-config] fan mode '%s' unknown — defaulting to pulse", s);
    return SmokeConfig::FN_PULSE;
}

// ─── Helpers ────────────────────────────────────────────────────────
//
// Note (Phase 2.9 cleanup): no parseServoMotionProfile here.  The
// servo motion shape (min/max/center/speed/accel/jerk) lives on
// ServoActuatorRole, declared in /hubfx.yaml's `ports[]` block at
// Servo motion profile is stored in /hubfx.yaml's ports[] block per
// Rule 42 (storage) + Rule 44 (UI is feature-inline); the gun never
// carries profile fields in /gunfx.yaml.

inline void parseGunAxis(const YamlNode* node, hubfx::effects::gunfx::GunAxis& axis) {
    using hubfx::config::portRefFromNode;
    if (!node) return;
    axis.enabled       =           node->template childAs<bool>   ("enabled", axis.enabled);
    axis.servoPort     = portRefFromNode(node->child("servo_port"));
    // Named-channel input (Rule 43). Resolution to port+channel happens
    // in applyGunFxConfig against /hubfx.yaml's inputs[].
    const char* axInputName = node->childAs<const char*>("input", "");
    std::memset(axis.inputName, 0, sizeof(axis.inputName));
    if (axInputName && axInputName[0]) {
        std::strncpy(axis.inputName, axInputName, sizeof(axis.inputName) - 1);
    }
    axis.neutralUs     = (uint16_t)node->template childAs<int32_t>("neutral_us", axis.neutralUs);
}

inline bool parseGunSpec(const YamlNode* gn, hubfx::effects::gunfx::GunSpec& g) {
    using namespace hubfx::effects::gunfx;
    using hubfx::config::portRefFromNode;
    if (!gn) return false;

    g.id = (uint8_t)gn->childAs<int32_t>("id", 0);
    const char* nm = gn->childAs<const char*>("name", "");
    std::memset(g.name, 0, sizeof(g.name));
    if (nm && nm[0]) std::strncpy(g.name, nm, sizeof(g.name) - 1);

    // trigger: Rule 43 — named-channel reference into /hubfx.yaml inputs[]
    if (const auto* tn = gn->child("trigger")) {
        const char* tinp = tn->childAs<const char*>("input", "");
        std::memset(g.triggerInput, 0, sizeof(g.triggerInput));
        if (tinp && tinp[0]) std::strncpy(g.triggerInput, tinp, sizeof(g.triggerInput) - 1);
        g.triggerThresholdUs  = (uint16_t)tn->childAs<int32_t>("threshold_us",  1500);
        g.triggerHysteresisUs = (uint16_t)tn->childAs<int32_t>("hysteresis_us", 25);
    }

    // rof: Rule 43 — named-channel selector
    if (const auto* rn = gn->child("rof")) {
        const char* rinp = rn->childAs<const char*>("input", "");
        std::memset(g.rofSelectorInput, 0, sizeof(g.rofSelectorInput));
        if (rinp && rinp[0]) std::strncpy(g.rofSelectorInput, rinp, sizeof(g.rofSelectorInput) - 1);
        g.numRofItems        = 0;
        const auto* itemsNode = rn->child("items");
        if (itemsNode && itemsNode->type == YamlNode::Sequence) {
            const int m = itemsNode->childCount();
            for (int j = 0; j < m && g.numRofItems < kMaxRofItems; ++j) {
                const auto* it = itemsNode->childAt(j);
                if (!it) continue;
                RofItem& r = g.rofItems[g.numRofItems];
                const char* itn = it->childAs<const char*>("name", "");
                std::memset(r.name, 0, sizeof(r.name));
                if (itn && itn[0]) std::strncpy(r.name, itn, sizeof(r.name) - 1);
                // band: [lo, hi] — accept inline-flow seq too.
                const auto* bandNode = it->child("band");
                if (bandNode && bandNode->type == YamlNode::Sequence
                        && bandNode->childCount() >= 2) {
                    r.bandLoUs = (uint16_t)bandNode->childAt(0)->as<int32_t>(0);
                    r.bandHiUs = (uint16_t)bandNode->childAt(1)->as<int32_t>(0);
                } else {
                    r.bandLoUs = (uint16_t)it->childAs<int32_t>("band_lo_us", 0);
                    r.bandHiUs = (uint16_t)it->childAs<int32_t>("band_hi_us", 0);
                }
                r.rpm = (uint16_t)it->childAs<int32_t>("rpm", 600);
                const char* sp = it->childAs<const char*>("sound", "");
                std::memset(r.soundPath, 0, sizeof(r.soundPath));
                if (sp && sp[0]) std::strncpy(r.soundPath, sp, sizeof(r.soundPath) - 1);
                // Stereo routing (Rule 11 append-only — defaults to both
                // when the YAML omits the field).
                r.outputMask = (uint8_t)it->template childAs<int32_t>("output_mask", 0x03);
                if (r.outputMask == 0) r.outputMask = 0x03;
                g.numRofItems++;
            }
        }
    }

    // muzzle_flash:
    if (const auto* mn = gn->child("muzzle_flash")) {
        g.muzzleFlashPort   = portRefFromNode(mn->child("port"));
        g.flashDurationMs   = (uint16_t)mn->childAs<int32_t>("duration_ms", 30);
        g.flashBrightness   = (uint8_t) mn->childAs<int32_t>("brightness",  100);
    }

    // recoil: turret behaviour, no dedicated port.  Instant random jerk on
    // BOTH axes (2026-06) — `jerk_us` is the max |offset|, `hold_ms` the dwell
    // before snap-back.  The old `axis` picker was dropped (both axes kick);
    // a legacy `axis:` key in an on-device YAML is simply ignored.
    if (const auto* rcn = gn->child("recoil")) {
        g.recoilEnabled = rcn->childAs<bool>("enabled", true);
        g.recoilJerkUs = (uint16_t)rcn->childAs<int32_t>("jerk_us", g.recoilJerkUs);
        g.recoilHoldMs = (uint16_t)rcn->childAs<int32_t>("hold_ms", g.recoilHoldMs);
    }

    // smoke:
    if (const auto* sn = gn->child("smoke")) {
        if (const auto* hn = sn->child("heater")) {
            g.smoke.heaterPort       = portRefFromNode(hn->child("port"));
            // Element voltage (Phase 4 polish 2026-05-26 — Rule 42):
            // the gun spec carries the heating-element rated voltage so
            // the role's scaleDuty() can translate "100 % intent" into
            // a safe port-native duty against the port rail.
            g.smoke.heaterElementMv  = (uint16_t)hn->childAs<int32_t>("element_mv", 6000);
            // Heater mode + cycle timing (Phase 4 polish 2026-05-26).
            // Mode defaults to continuous; cycle ON/OFF only matter
            // when mode = cycle.
            g.smoke.heaterMode        = gunfxHeaterModeFromName(
                hn->childAs<const char*>("mode", "continuous"));
            g.smoke.heaterCycleOnMs   = (uint16_t)hn->childAs<int32_t>("cycle_on_ms",  5000);
            g.smoke.heaterCycleOffMs  = (uint16_t)hn->childAs<int32_t>("cycle_off_ms", 3000);
            // Heater activation channel (Rule 43, NEW 2026-05-26):
            // optional NAMED channel that gates the heater behind an RC
            // input.  Resolution to port+channel happens in
            // applyGunFxConfig.  Empty/missing block = "no gating".
            if (const auto* an = hn->child("activation")) {
                const char* ainp = an->childAs<const char*>("input", "");
                std::memset(g.smoke.heaterActivationInput, 0,
                            sizeof(g.smoke.heaterActivationInput));
                if (ainp && ainp[0]) {
                    std::strncpy(g.smoke.heaterActivationInput, ainp,
                                 sizeof(g.smoke.heaterActivationInput) - 1);
                }
                g.smoke.heaterActivationThresholdUs  = (uint16_t)an->childAs<int32_t>("threshold_us",  1500);
                g.smoke.heaterActivationHysteresisUs = (uint16_t)an->childAs<int32_t>("hysteresis_us", 25);
            }
        }
        if (const auto* fn = sn->child("fan")) {
            g.smoke.fanPort  = portRefFromNode(fn->child("port"));
            g.smoke.fanMode  = gunfxFanModeFromName(
                fn->childAs<const char*>("mode", "pulse"));
            // FN_PULSE sinusoid envelope length per shot.  Default
            // 100 ms matches the 600 RPM ROF default firing rate so
            // the sinusoid completes once per shot at the default
            // cadence (operator dials in pulse_duration_ms to match
            // a custom ROF).
            g.smoke.fanPulseDurationMs = (uint16_t)fn->childAs<int32_t>("pulse_duration_ms", 100);
            // Element voltage for the fan motor (same intent as heater
            // above — gun owns the spec because the smoke harness
            // travels with the gun, not the port).
            g.smoke.fanElementMv = (uint16_t)fn->childAs<int32_t>("element_mv", 6000);
        }
    }

    // yaw / pitch:
    parseGunAxis(gn->child("yaw"),   g.yaw);
    parseGunAxis(gn->child("pitch"), g.pitch);

    return true;
}

// ─── ConfigStore adapter ────────────────────────────────────────────

struct GunFxConfigSchema {
    using DataType = GunFxYamlConfig;

    template <typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        const auto* root = p.root();
        if (!root) return true;     // empty file → defaults
        d.enabled = root->template childAs<bool>("enabled", false);
        d.numGuns = 0;
        const auto* gunsNode = root->child("guns");
        if (!gunsNode || gunsNode->type != YamlNode::Sequence) {
            return true;
        }
        const int n = gunsNode->childCount();
        for (int i = 0; i < n && d.numGuns < hubfx::effects::gunfx::kMaxGuns; ++i) {
            if (parseGunSpec(gunsNode->childAt(i), d.guns[d.numGuns])) {
                d.numGuns++;
            }
        }
        return true;
    }

    static bool validate(const DataType& /*d*/, char* /*err*/, size_t /*errLen*/) {
        return true;
    }

    static const char* defaultPath() { return "/gunfx.yaml"; }
};

#endif  // HUBFX_GUNFX_CONFIG_YAML_H
