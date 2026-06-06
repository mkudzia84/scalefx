/*
 * landing_config.h — `/landing.yaml` schema.
 *
 * Landing-light deployment table.  One landing light per entry; each
 * couples a single servo (deploys / stows the searchlight) with one
 * or more PWM LEDs (the bulbs).  Loaded into a `LandingLightDef[]`
 * passed to `LandingLightServicePolicy::configure()` at apply time.
 *
 * YAML shape:
 *
 *   schema_version: 1
 *   lights:
 *     - id: 0
 *       name: "nose"
 *       owner: lightfx                  # lightfx | gunfx | gearcontrol | landing-light
 *       servo:
 *         port: { kind: servo, idx: 0 }
 *         open_us:  1900                # DIRECTION ONLY (see note below)
 *         close_us: 1100
 *       leds:
 *         - { port: { kind: pwm, idx: 5 }, brightness_pct: 100 }
 *       fade_in_ms: 400               # LED soft-start after servo deploys
 *       activation:                   # OPTIONAL RC-channel auto-deploy
 *         input: landing_deploy       # named channel from /hubfx.yaml inputs[]
 *         threshold_us: 1500          # ≥ threshold ⇒ deploy, < ⇒ retract
 *         hysteresis_us: 50
 *
 * NOTE (2026-06-06): `open_us` / `close_us` encode only the DEPLOY
 * DIRECTION (which mechanical end is "deployed"), NOT the literal target.
 * `LandingLight::deploy()` / `retract()` command each servo's ROLE to its
 * own calibrated endpoint via an out-of-range sentinel (the role clamps to
 * its profile min/max — Rule 42/44), so the servo always travels the full
 * calibrated throw regardless of the stored µs.  `open_us >= close_us` ⇒
 * deploy drives to the calibrated MAX end (else the MIN end).  The travel
 * limits live in the servo's `/hubfx.yaml` ports[].profile, never here.
 *
 * `activation:` drives the light from an RC channel (resolved + wired by
 * the sketch's LandingActivationDriver, mirroring the EngineFx toggle).
 * Program-attach ("deploy while program X is active") is NOT in this
 * block — Studio injects a `landing_bindings: [{id, state}]` into the
 * target program YAML, so the existing program→landing path drives it.
 *
 * Direct YamlNode traversal (the declarative DSL doesn't compose
 * nested sequences — lights[].leds[] is a sequence inside a sequence).
 */

#ifndef HUBFX_LANDING_CONFIG_H
#define HUBFX_LANDING_CONFIG_H

#include <cstdint>
#include <cstring>

#include <config/yaml_parser.h>
#include <serial/diag_log.h>
#include <serial/ports.h>

#include "../effects/effect_id.h"
#include "../effects/landing_lights/landing_light.h"
#include "../effects/landing_lights/landing_light_service.h"   // kMaxLandingLights
#include "port_ref_yaml.h"                                     // portRefFromNode

struct LandingYamlPool {
    static constexpr size_t MAX_NODES        = 256;
    static constexpr size_t STRING_POOL_SIZE = 3072;
    static constexpr size_t MAX_DEPTH        = 12;
};

/// RC-channel auto-deploy binding for one landing light (parallel to
/// `LandingConfig::lights[]`).  Empty `input` ⇒ no channel activation
/// (the light is manual / program-driven).  Resolved to a PortRef +
/// channel by the sketch's LandingActivationDriver at apply time
/// (mirrors EngineFx's `toggle:` block).
struct LandingActivation {
    char     input[24]    = {};      ///< named channel from /hubfx.yaml inputs[]
    uint16_t thresholdUs  = 1500;    ///< ≥ ⇒ deploy, < ⇒ retract
    uint16_t hysteresisUs = 50;
};

/// Parsed form of `/landing.yaml`.  Wraps the firmware-side
/// `LandingLightDef[]` (handed straight to
/// `LandingLightServicePolicy::configure()`) plus a parallel
/// `activations[]` the sketch's LandingActivationDriver consumes.
struct LandingConfig {
    static constexpr uint8_t kSchemaVersion = 1;

    hubfx::effects::landing::LandingLightDef
        lights[hubfx::effects::landing::kMaxLandingLights] = {};
    LandingActivation
        activations[hubfx::effects::landing::kMaxLandingLights] = {};
    uint8_t numLights = 0;
};

/// Map a snake-case effect-family name to the EffectId enum.  Unknown
/// names log a WARN and default to `LightFx` (the most common owner).
inline hubfx::effects::EffectId effectIdFromName(const char* name) {
    using hubfx::effects::EffectId;
    if (!name || !name[0])                                return EffectId::LightFx;
    if (std::strcmp(name, "none")         == 0)           return EffectId::None;
    if (std::strcmp(name, "lightfx")      == 0)           return EffectId::LightFx;
    if (std::strcmp(name, "gunfx")        == 0)           return EffectId::GunFx;
    if (std::strcmp(name, "gearcontrol")  == 0)           return EffectId::GearCtrl;
    if (std::strcmp(name, "landing-light") == 0)          return EffectId::LandingLight;
    if (std::strcmp(name, "landing_light") == 0)          return EffectId::LandingLight;
    SFX_LOG_WARN("[landing-config] unknown owner '%s' — defaulting to lightfx", name);
    return EffectId::LightFx;
}

// ─── ConfigStore adapter ────────────────────────────────────────────

struct LandingConfigSchema {
    using DataType = LandingConfig;

    template <typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        using namespace hubfx::effects::landing;
        using hubfx::config::portRefFromNode;
        d.numLights = 0;
        const auto* root = p.root();
        const auto* lightsNode = root ? root->child("lights") : nullptr;
        if (!lightsNode || lightsNode->type != YamlNode::Sequence) {
            // Empty / missing — leave the table empty.  Caller's
            // configure() call gets 0 defs and the service stays inert.
            return true;
        }
        const int n = lightsNode->childCount();
        for (int i = 0; i < n && d.numLights < kMaxLandingLights; ++i) {
            const auto* ll = lightsNode->childAt(i);
            if (!ll) continue;
            LandingLightDef& def = d.lights[d.numLights];
            def.id            = (uint8_t)ll->template childAs<int32_t>("id", 0);
            const char* nm    = ll->template childAs<const char*>("name", "");
            std::memset(def.name, 0, sizeof(def.name));
            if (nm && nm[0]) std::strncpy(def.name, nm, sizeof(def.name) - 1);
            def.owner = effectIdFromName(ll->template childAs<const char*>("owner", "lightfx"));

            // Servo parsing (2026-05-24): two accepted forms —
            //
            //   1. NEW (preferred, multi-servo):
            //        servos:
            //          - { port: {kind: servo, idx: 0} }
            //          - { port: {kind: servo, idx: 1} }
            //        open_us: 1900            # group-wide
            //        close_us: 1100
            //
            //   2. LEGACY (single-servo, back-compat with /landing.yaml
            //      files written before multi-servo landed):
            //        servo:
            //          port: { kind: servo, idx: 0 }
            //          open_us: 1900
            //          close_us: 1100
            //
            // The legacy form keeps open_us / close_us nested under
            // `servo:`; the new form lifts them to the landing-light
            // level so they're not duplicated per servo entry.
            def.numServos = 0;
            const auto* servosNode = ll->child("servos");
            if (servosNode && servosNode->type == YamlNode::Sequence) {
                // New multi-servo form.
                const int sN = servosNode->childCount();
                for (int j = 0; j < sN && def.numServos < kMaxServosPerLanding; ++j) {
                    const auto* s = servosNode->childAt(j);
                    if (!s) continue;
                    def.servos[def.numServos++] = portRefFromNode(s->child("port"));
                }
                def.openUs  = (uint16_t)ll->template childAs<int32_t>("open_us",  1900);
                def.closeUs = (uint16_t)ll->template childAs<int32_t>("close_us", 1100);
            } else {
                // Legacy single-servo form — kept indefinitely so
                // operator-authored /landing.yaml files don't have to
                // be rewritten when the firmware ships.
                const auto* servoNode = ll->child("servo");
                if (servoNode) {
                    def.servos[def.numServos++] = portRefFromNode(servoNode->child("port"));
                    def.openUs  = (uint16_t)servoNode->template childAs<int32_t>("open_us",  1900);
                    def.closeUs = (uint16_t)servoNode->template childAs<int32_t>("close_us", 1100);
                }
            }

            // leds: [ {port, brightness_pct}, ... ]
            def.numLeds = 0;
            const auto* ledsNode = ll->child("leds");
            if (ledsNode && ledsNode->type == YamlNode::Sequence) {
                const int m = ledsNode->childCount();
                for (int j = 0; j < m && def.numLeds < kMaxLedsPerLanding; ++j) {
                    const auto* led = ledsNode->childAt(j);
                    if (!led) continue;
                    def.leds[def.numLeds] = portRefFromNode(led->child("port"));
                    // brightness_pct on the FIRST led wins (single per-LL knob).
                    if (def.numLeds == 0) {
                        def.brightnessPct =
                            (uint8_t)led->template childAs<int32_t>("brightness_pct", 100);
                    }
                    def.numLeds++;
                }
            }
            if (def.numLeds == 0) {
                SFX_LOG_WARN("[landing-config] lights[%u] (id=%u) has no LEDs — skipping",
                             (unsigned)i, (unsigned)def.id);
                continue;
            }

            // LED soft-start ramp (ms) applied after the servo deploys.
            def.fadeInMs = (uint16_t)ll->template childAs<int32_t>("fade_in_ms", 0);

            // Optional RC-channel auto-deploy binding (parallel array).
            LandingActivation& act = d.activations[d.numLights];
            act = LandingActivation{};
            const auto* actNode = ll->child("activation");
            if (actNode) {
                const char* inp = actNode->template childAs<const char*>("input", "");
                if (inp && inp[0]) std::strncpy(act.input, inp, sizeof(act.input) - 1);
                act.thresholdUs  = (uint16_t)actNode->template childAs<int32_t>("threshold_us",  1500);
                act.hysteresisUs = (uint16_t)actNode->template childAs<int32_t>("hysteresis_us",   50);
            }

            d.numLights++;
        }
        return true;
    }

    static bool validate(const DataType& /*d*/, char* /*err*/, size_t /*errLen*/) {
        return true;
    }

    static const char* defaultPath() { return "/landing.yaml"; }
};

#endif  // HUBFX_LANDING_CONFIG_H
