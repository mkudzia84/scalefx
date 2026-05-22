/*
 * gearcontrol_config.h — `/gearcontrol.yaml` schema.
 *
 * Gear definition table for the hub's GearControlService.  Each gear is
 * one H-bridge motor (BiDcMotor role) addressed by PortRef — typically
 * pointing at a GearControl expander board by GUID.  The motor's INA226
 * current sensor on the expander drives MOTOR_STALL_EVENT → the hub's
 * per-gear timeout/stall state machine.
 *
 * Status LEDs are NOT configured here — the GearControl expander lights
 * its own small per-motor direction LEDs locally from its H-bridge state.
 *
 * `/hubfx.yaml`'s `features.gears:` flag is the master kill-switch
 * (overrides the local `enabled:` here).
 *
 * YAML shape (the motor PortRef may use the flow form `{ … }`):
 *
 *   schema_version: 1
 *   enabled: true
 *   gears:
 *     - id: 0
 *       name: nose
 *       motor: { guid: "AB12", kind: hbridge, idx: 0 }
 *       deploy_duty:  20000      # signed H-bridge duty for "going down"
 *       retract_duty: -20000     # signed duty for "going up"
 *       timeout_ms:   4000       # full-travel watchdog
 *
 * The `guid` is the expander's 4-hex deviceName suffix (e.g.
 * "GearCtrl-AB12" → "AB12"); omit it for a hub-local port.  Direct
 * YamlNode traversal (the declarative DSL doesn't compose the gears
 * sequence with the nested motor map).
 */

#ifndef HUBFX_GEARCONTROL_CONFIG_H
#define HUBFX_GEARCONTROL_CONFIG_H

#include <cstdint>
#include <cstring>

#include <config/yaml_parser.h>
#include <serial/diag_log.h>
#include <serial/ports.h>

#include "../effects/effect_id.h"
#include "../effects/gearcontrol/gear.h"
#include "../effects/gearcontrol/gearcontrol_service.h"   // kMaxGears
#include "port_ref_yaml.h"

struct GearControlYamlPool {
    static constexpr size_t MAX_NODES        = 256;
    static constexpr size_t STRING_POOL_SIZE = 3072;
    static constexpr size_t MAX_DEPTH        = 12;
};

/// Parsed form of `/gearcontrol.yaml` — a thin wrapper around the
/// firmware-side `GearDef[]` so apply can hand the array straight to
/// `GearControlServicePolicy::configure()`.
struct GearControlConfig {
    static constexpr uint8_t kSchemaVersion = 1;

    bool enabled = true;
    hubfx::effects::gearctrl::GearDef
        gears[hubfx::effects::gearctrl::kMaxGears] = {};
    uint8_t numGears = 0;
};

// ─── ConfigStore adapter ────────────────────────────────────────────

struct GearControlConfigSchema {
    using DataType = GearControlConfig;

    template <typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        using namespace hubfx::effects::gearctrl;
        using hubfx::config::portRefFromNode;

        d.numGears = 0;
        const auto* root = p.root();
        d.enabled = root ? root->template childAs<bool>("enabled", true) : true;

        const auto* gearsNode = root ? root->child("gears") : nullptr;
        if (!gearsNode || gearsNode->type != YamlNode::Sequence) {
            return true;   // empty table — service stays inert
        }
        const int n = gearsNode->childCount();
        for (int i = 0; i < n && d.numGears < kMaxGears; ++i) {
            const auto* g = gearsNode->childAt(i);
            if (!g) continue;
            GearDef& def = d.gears[d.numGears];

            def.id = (uint8_t)g->template childAs<int32_t>("id", 0);
            const char* nm = g->template childAs<const char*>("name", "");
            std::memset(def.name, 0, sizeof(def.name));
            if (nm && nm[0]) std::strncpy(def.name, nm, sizeof(def.name) - 1);

            // motor: { guid?, kind: hbridge, idx }
            def.motor = portRefFromNode(g->child("motor"));
            if (def.motor.portKind == 0) {
                SFX_LOG_WARN("[gearcontrol-config] gears[%d] (id=%u): missing/invalid `motor`",
                             i, (unsigned)def.id);
                continue;
            }

            def.deployDuty  = (int16_t) g->template childAs<int32_t>("deploy_duty",   20000);
            def.retractDuty = (int16_t) g->template childAs<int32_t>("retract_duty", -20000);
            def.timeoutMs   = (uint32_t)g->template childAs<int32_t>("timeout_ms",     4000);
            d.numGears++;
        }
        return true;
    }

    static bool validate(const DataType& /*d*/, char* /*err*/, size_t /*errLen*/) {
        return true;
    }

    static const char* defaultPath() { return "/gearcontrol.yaml"; }
};

#endif  // HUBFX_GEARCONTROL_CONFIG_H
