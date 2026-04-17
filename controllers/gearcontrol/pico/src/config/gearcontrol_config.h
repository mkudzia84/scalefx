/*
 * GearControl Configuration — Per-Pin Role Assignment Schema
 *
 * 7 reconfigurable pins can be assigned roles:
 *   door       — Door servo assigned to a retract channel (0-2)
 *   gear_input — PWM input for gear up/down signal (standalone mode)
 *   yaw_input  — PWM input for yaw steering signal (standalone mode)
 *   yaw_output — Yaw servo output (associated with a retract channel)
 *   unused     — Pin disabled
 *
 * Pin-to-GPIO mapping (fixed by hardware):
 *   pin1 → GP1    pin5 → GP7
 *   pin2 → GP2    pin6 → GP8
 *   pin3 → GP3    pin7 → GP9
 *   pin4 → GP6
 *
 * Dual-mode operation:
 *   Standalone — gear_input required; yaw_input required if yaw_output used
 *   Slave      — input pins ignored (HubFX sends commands); more pins for doors
 *
 * YAML structure:
 *   retracts:
 *     - channel: 0
 *       enabled: true
 *       stall_current_mA: 500
 *       timeout_ms: 60000
 *
 *   pins:
 *     - slot: pin1
 *       role: door
 *       channel: 0
 *       min_us: 500
 *       max_us: 2500
 *       speed: 4000
 *       reversed: false
 *     - slot: pin3
 *       role: gear_input
 *       threshold_us: 1500
 *     - slot: pin7
 *       role: yaw_output
 *       gear_id: 0
 *       neutral_us: 1500
 *       min_us: 1000
 *       max_us: 2000
 *
 *   door_modes:
 *     - channel: 0
 *       pre_deploy: dual_sync
 *       post_deploy: none
 *       delay_ms: 500
 *
 *   battery:
 *     enabled: false
 *     auto_deploy: false
 *     chemistry: lipo
 *     cell_count: 0
 */

#ifndef GEARCONTROL_CONFIG_H
#define GEARCONTROL_CONFIG_H

#include <cstdint>
#include <config/yaml_schema.h>

// ============================================================================
// Capacity Limits
// ============================================================================

namespace GearControlLimits {
    constexpr int MAX_RETRACTS  = 3;   // 3 retract motor channels (INA226)
    constexpr int MAX_PINS      = 7;   // 7 reconfigurable pins
    constexpr int MAX_DOOR_MODES = 3;  // 1 per retract channel
}

// ============================================================================
// Data Struct
// ============================================================================

struct GearControlConfig {

    // ---- Retract channel configuration (motor + stall detection) ----

    struct RetractConfig {
        uint8_t  channel          = 0;      // Retract channel index (0-2)
        bool     enabled          = true;   // Channel active
        uint16_t stallCurrent_mA  = 500;    // Stall detection threshold       // mA
        uint16_t timeout_ms       = 60000;  // Maximum motor run time          // ms
    };

    RetractConfig retracts[GearControlLimits::MAX_RETRACTS] = {};
    uint8_t       retractCount = 0;

    // ---- Per-pin role assignment ----
    // Flat union-style struct: role determines which fields are active.
    // Unused fields are ignored by firmware.

    struct PinConfig {
        char     slot[12]        = "";        // gear0_a..yaw — maps to GPIO
        char     role[12]        = "unused";  // door|gear_input|yaw_input|yaw_output|unused

        // Door role fields:
        uint8_t  channel         = 0;         // Retract channel (0-2)

        // Servo fields (door + yaw_output):
        uint16_t min_us          = 500;       // Servo min pulse width          // µs
        uint16_t max_us          = 2500;      // Servo max pulse width          // µs
        uint16_t speed           = 4000;      // Servo speed                    // µs/sec
        bool     reversed        = false;     // Invert servo direction

        // Input fields (gear_input):
        uint16_t threshold_us    = 1500;      // PWM threshold: above=deploy    // µs

        // Yaw output fields:
        uint8_t  gear_id         = 0;         // Associated retract channel (yaw active when deployed)
        uint16_t neutral_us      = 1500;      // Yaw center/neutral position    // µs
    };

    PinConfig pins[GearControlLimits::MAX_PINS] = {};
    uint8_t   pinCount = 0;

    // ---- Door sequencing per retract channel ----

    struct DoorModeConfig {
        uint8_t  channel         = 0;           // Retract channel (0-2)
        char     preDeploy[12]   = "dual_sync"; // none|single|dual_sync|dual_delay|dual_seq
        char     postDeploy[12]  = "none";      // none|single|dual_sync|dual_delay|dual_seq
        uint16_t delay_ms        = 500;         // Delay between doors (dual_delay only)  // ms
    };

    DoorModeConfig doorModes[GearControlLimits::MAX_DOOR_MODES] = {};
    uint8_t        doorModeCount = 0;

    // ---- Battery monitoring ----

    struct Battery {
        bool     enabled         = false;  // Enable voltage monitoring
        bool     autoDeploy      = false;  // Auto-deploy gear on low battery
        char     chemistry[8]    = "lipo"; // lipo|life|nicd|nimh
        uint8_t  cellCount       = 0;      // Number of cells (0 = disabled)
    } battery;
};

// ============================================================================
// Declarative Schema
// ============================================================================

namespace gearcontrol_config {

using namespace sfx;

using R  = GearControlConfig::RetractConfig;
using P  = GearControlConfig::PinConfig;
using DM = GearControlConfig::DoorModeConfig;
using B  = GearControlConfig::Battery;

/// Field-level schema — populate() reads YAML, validate() checks ranges.
inline const auto fields = schema<GearControlConfig>(

    // ---- Retract channels ----
    seq<&GearControlConfig::retracts, &GearControlConfig::retractCount>("retracts",
        prop<&R::channel>        ("channel",          uint8_t(0))    .range(0, 2),
        prop<&R::enabled>        ("enabled",          true),
        prop<&R::stallCurrent_mA>("stall_current_mA", uint16_t(500)) .range(50, 5000),
        prop<&R::timeout_ms>     ("timeout_ms",       uint16_t(60000)).range(500, 65000)
    ),

    // ---- Pin role assignments ----
    // All fields present in each entry; firmware ignores role-irrelevant fields.
    seq<&GearControlConfig::pins, &GearControlConfig::pinCount>("pins",
        prop<&P::slot>        ("slot",         ""),
        prop<&P::role>        ("role",         "unused"),
        prop<&P::channel>     ("channel",      uint8_t(0))    .range(0, 2),
        prop<&P::min_us>      ("min_us",       uint16_t(500)) .range(300, 2700),
        prop<&P::max_us>      ("max_us",       uint16_t(2500)).range(300, 2700),
        prop<&P::speed>       ("speed",        uint16_t(4000)),
        prop<&P::reversed>    ("reversed",     false),
        prop<&P::threshold_us>("threshold_us", uint16_t(1500)).range(800, 2200),
        prop<&P::gear_id>     ("gear_id",      uint8_t(0))    .range(0, 2),
        prop<&P::neutral_us>  ("neutral_us",   uint16_t(1500)).range(500, 2500)
    ),

    // ---- Door sequencing per retract channel ----
    seq<&GearControlConfig::doorModes, &GearControlConfig::doorModeCount>("door_modes",
        prop<&DM::channel>    ("channel",     uint8_t(0))  .range(0, 2),
        prop<&DM::preDeploy>  ("pre_deploy",  "dual_sync"),
        prop<&DM::postDeploy> ("post_deploy", "none"),
        prop<&DM::delay_ms>   ("delay_ms",    uint16_t(500)).range(0, 5000)
    ),

    // ---- Battery monitoring ----
    group<&GearControlConfig::battery>("battery",
        prop<&B::enabled>    ("enabled",     false),
        prop<&B::autoDeploy> ("auto_deploy", false),
        prop<&B::chemistry>  ("chemistry",   "lipo"),
        prop<&B::cellCount>  ("cell_count",  uint8_t(0)).range(0, 6)
    )
);

} // namespace gearcontrol_config

// ============================================================================
// ConfigStore Schema Adapter
// ============================================================================

struct GearControlConfigSchema {
    using DataType = GearControlConfig;

    static bool populate(DataType& d, const YamlParser<>& p) {
        return gearcontrol_config::fields.populate(d, p.root());
    }

    static bool validate(const DataType& d, char* err, size_t errLen) {
        return gearcontrol_config::fields.validate(d, err, errLen);
    }

    static const char* defaultPath() { return "/config.yaml"; }
};

#endif // GEARCONTROL_CONFIG_H
