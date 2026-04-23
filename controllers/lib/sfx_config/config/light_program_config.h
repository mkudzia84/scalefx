/*
 * Light Program Configuration — Shared Data Struct + Declarative Schema
 *
 * Defines channel programs, servo setup, landing light bindings, and
 * input-driven program selection. ONE canonical document — /lightfx.yaml —
 * lives on every board that participates in the light program (standalone
 * LightFX Pico and HubFX). The same struct is loaded on each side; each
 * side filters out the fields it cannot drive:
 *
 *   - LightFX (slave): consumes only board="lightfx" channels and
 *     `lightfx_channel_mask` for landing groups. Ignores hubfx fields.
 *   - HubFX (master): consumes board="hubfx" channels and
 *     `hubfx_channel_mask` locally, AND pushes the lightfx-side fields
 *     to the attached slave via pushLightFxConfigToSlave().
 *
 * /hubfx.yaml does NOT contain light_program (Rule 26: per-domain split).
 *
 * YAML structure (top-level keys in /lightfx.yaml):
 *
 *   master_brightness: 100          # 0-100%
 *
 *   servos:                         # Servo hardware configuration (LightFX-side; 1-3)
 *     - id: 1
 *       min_us: 500
 *       max_us: 2500
 *       default_us: 1500
 *       speed: 4000                 # µs/sec
 *       accel: 8000                 # µs/sec²
 *       decel: 8000                 # µs/sec²
 *       reversed: false
 *
 *   landing_groups:                 # Cross-board: servo from one board, channels from either
 *     - name: "Main Gear"
 *       servo_board: lightfx        # "lightfx" | "hubfx"
 *       servo_id: 1                 # Board-relative (1-3 lightfx, 1..N hubfx)
 *       lightfx_channels: 0x60      # Bit 0 → ch1 … bit 7 → ch8 (8 LightFX channels)
 *       hubfx_channels:   0x05      # Bit 0 → ch1 … bit 5 → ch6 (6 HubFX channels)
 *       brightness: 100
 *
 *   input:                          # RX input for program selection
 *     channel: 1                    # RX channel number (1-based)
 *     bands:                        # PWM band → program mapping
 *       - min_us: 900
 *         max_us: 1200
 *         program: 0
 *       - min_us: 1200
 *         max_us: 1800
 *         program: 1
 *
 *   programs:                       # Channel programs (LED sequences per mode)
 *     - name: "NAV"
 *       group_policies: [off]       # Group 0 retracted for this program
 *       channels:
 *         - board: lightfx          # Optional, default "lightfx"
 *           channel: 1              # Board-relative (1..8 lightfx, 1..6 hubfx)
 *           events:
 *             - type: beacon
 *               cycle_ms: 1000
 *               flash_pct: 10
 *               max_brightness: 100
 *               min_brightness: 5
 *         - board: hubfx
 *           channel: 2
 *           events:
 *             - type: flash
 *               cycle_ms: 500
 *               brightness: 80
 *               duty: 50
 *         - board: lightfx
 *           channel: 5
 *           group: 0                # Controlled by landing group 0
 *         - board: lightfx
 *           channel: 6
 *           group: 0
 *
 *     - name: "LANDING"
 *       group_policies: [on]        # Group 0 deployed
 *       channels:
 *         - board: lightfx
 *           channel: 1
 *           events:
 *             - type: on
 *               brightness: 100
 *         - board: lightfx
 *           channel: 5
 *           group: 0
 *         - board: lightfx
 *           channel: 6
 *           group: 0
 *
 * Event type → seqAdd parameter mapping:
 *
 *   | Type     | p1           | p2          | p3             | p4             | p5             |
 *   |----------|--------------|-------------|----------------|----------------|----------------|
 *   | on       | duration_ms  | pwm_duty    | brightness     | —              | —              |
 *   | off      | duration_ms  | —           | —              | —              | —              |
 *   | flash    | cycle_ms     | duration_ms | brightness     | duty           | —              |
 *   | fade_in  | duration_ms  | —           | brightness     | —              | —              |
 *   | fade_out | duration_ms  | —           | brightness     | —              | —              |
 *   | fading   | cycle_ms     | duration_ms | min_brightness | max_brightness | —              |
 *   | beacon   | cycle_ms     | duration_ms | flash_pct      | max_brightness | min_brightness |
 */

#ifndef LIGHT_PROGRAM_CONFIG_H
#define LIGHT_PROGRAM_CONFIG_H

#include <cstdint>
#include <config/yaml_schema.h>

// ============================================================================
// Capacity Limits
// ============================================================================

// NOTE: NOT named MAX_SERVOS — Arduino-Pico's Servo.h `#define MAX_SERVOS 8`
// macro-substitutes any matching identifier, even one inside a namespace, when
// this header is included after Servo.h (every controller .ino does so via
// servo/srv_control.h). The collision turned a constexpr into a numeric
// literal, then the array index broke unparseably. Keep the SERVOS_MAX style
// for any other Arduino-defined macros you might collide with later.
namespace LightProgramLimits {
    constexpr int SERVOS_MAX               = 3;
    constexpr int MAX_LANDING_GROUPS       = 3;
    constexpr int MAX_INPUT_BANDS          = 8;
    constexpr int MAX_PROGRAMS             = 4;
    // Cross-board program: a program can name up to 8 lightfx + 6 hubfx
    // channel-defs total. Standalone slave only consumes the 8 lightfx slots.
    constexpr int MAX_CHANNELS_PER_PROGRAM = 14;
    constexpr int MAX_EVENTS_PER_CHANNEL   = 8;
}

// Board enum constants — used for `servo_board` in landing groups and
// `board` on per-channel program defs. Strings live in YAML; firmware
// reads them via boardIsLightFx() / boardIsHubFx().
namespace LightProgramBoard {
    inline constexpr char LIGHTFX[] = "lightfx";
    inline constexpr char HUBFX[]   = "hubfx";

    inline bool isLightFx(const char* s) {
        // Default (empty / null) = lightfx, so older configs without the
        // field continue to apply on the slave (Rule 11 — append-only).
        return s == nullptr || s[0] == '\0' || s[0] == 'l' || s[0] == 'L';
    }
    inline bool isHubFx(const char* s) {
        return s != nullptr && (s[0] == 'h' || s[0] == 'H');
    }
}

// ============================================================================
// Data Struct
// ============================================================================

struct LightProgramConfig {

    uint8_t masterBrightness = 100;   // 0-100%

    // ---- Servo hardware configuration ----

    struct ServoSetup {
        uint8_t  id         = 0;      // Servo ID (1-3, 0 = unused)
        uint16_t min_us     = 500;    // Minimum pulse width
        uint16_t max_us     = 2500;   // Maximum pulse width
        uint16_t default_us = 1500;   // Initial/center position
        uint16_t speed      = 4000;   // Max speed (µs/sec)
        uint16_t accel      = 8000;   // Acceleration (µs/sec²)
        uint16_t decel      = 8000;   // Deceleration (µs/sec²)
        bool     reversed   = false;  // Invert open/close direction
    };

    ServoSetup servos[LightProgramLimits::SERVOS_MAX] = {};
    uint8_t    servoCount = 0;

    // ---- Landing light groups ----

    struct LandingGroup {
        char    name[16]           = {};        // Display name
        char    servoBoard[8]      = "lightfx"; // "lightfx" | "hubfx" — which board owns the servo
        uint8_t servoId            = 0;         // Board-relative servo ID (1-3 lightfx, 1..N hubfx, 0 = no servo)
        uint8_t lightfxChannelMask = 0;         // Bit 0 → lightfx ch1 … bit 7 → ch8 (8 channels)
        uint8_t hubfxChannelMask   = 0;         // Bit 0 → hubfx ch1 … bit 5 → ch6 (6 channels; bits 6-7 unused)
        uint8_t brightness         = 100;       // Light brightness when deployed (0-100)
    };

    LandingGroup landingGroups[LightProgramLimits::MAX_LANDING_GROUPS] = {};
    uint8_t      landingGroupCount = 0;

    // ---- RX input for program selection ----

    struct Input {
        uint8_t channel = 1;          // RX input channel (1-based)

        struct Band {
            uint16_t min_us  = 0;     // Band lower boundary (µs)
            uint16_t max_us  = 0;     // Band upper boundary (µs)
            uint8_t  program = 0;     // Program index to activate
        };

        Band    bands[LightProgramLimits::MAX_INPUT_BANDS] = {};
        uint8_t bandCount = 0;
    } input;

    // ---- LED event definition ----

    struct EventDef {
        char     type[12]       = "off";    // on/off/flash/fade_in/fade_out/fading/beacon
        uint16_t duration_ms    = 0;        // Duration (ON/OFF/FADE/FLASH/FADING/BEACON)
        uint16_t cycle_ms       = 0;        // Cycle time (FLASH/FADING/BEACON)
        uint8_t  brightness     = 100;      // Brightness (ON/FLASH/FADE_IN/FADE_OUT)
        uint8_t  pwm_duty       = 0;        // PWM power save (ON only, 0=off, 1-100)
        uint8_t  duty           = 50;       // Duty cycle % (FLASH only)
        uint8_t  flash_pct      = 15;       // Flash percentage (BEACON only)
        uint8_t  min_brightness = 0;        // Min brightness (FADING/BEACON)
        uint8_t  max_brightness = 100;      // Max brightness (FADING/BEACON)
    };

    // ---- Per-channel program definition ----

    struct ChannelDef {
        char     board[8]   = "lightfx";    // "lightfx" | "hubfx" — which board's LED channel
        uint8_t  channel    = 0;            // Board-relative LED channel (1..8 lightfx, 1..6 hubfx; 0 = unused)
        uint8_t  groupIndex = 0xFF;         // Landing group index (0xFF = events mode)
        EventDef events[LightProgramLimits::MAX_EVENTS_PER_CHANNEL] = {};
        uint8_t  eventCount = 0;
    };

    // ---- Group policy constants ----
    static constexpr uint8_t GROUP_POLICY_OFF  = 0;
    static constexpr uint8_t GROUP_POLICY_ON   = 1;
    static constexpr uint8_t GROUP_POLICY_GEAR = 2;

    // ---- Program definition ----

    struct Program {
        char       name[16]    = {};        // Display name
        uint8_t    groupPolicies[LightProgramLimits::MAX_LANDING_GROUPS] = {}; // 0=off, 1=on, 2=gear
        uint8_t    groupPolicyCount = 0;
        ChannelDef channels[LightProgramLimits::MAX_CHANNELS_PER_PROGRAM] = {};
        uint8_t    channelCount = 0;
    };

    Program programs[LightProgramLimits::MAX_PROGRAMS] = {};
    uint8_t programCount = 0;
};

// ============================================================================
// Declarative Schema
// ============================================================================

namespace light_program_config {

using namespace sfx;

using Sv = LightProgramConfig::ServoSetup;
using Lg = LightProgramConfig::LandingGroup;
using In = LightProgramConfig::Input;
using Bd = In::Band;
using Ev = LightProgramConfig::EventDef;
using Ch = LightProgramConfig::ChannelDef;
using Pg = LightProgramConfig::Program;

/// Field-level schema for the light program YAML section.
/// Provides auto-generated populate() and validate().
/// Usable standalone or embedded via asGroup() in a parent schema.
inline const auto fields = schema<LightProgramConfig>(

    prop<&LightProgramConfig::masterBrightness>("master_brightness", uint8_t(100))
        .range(0, 100),

    // ---- Servo hardware configuration ----
    seq<&LightProgramConfig::servos, &LightProgramConfig::servoCount>("servos",
        prop<&Sv::id>        ("id",         uint8_t(0))   .range(0, 3),
        prop<&Sv::min_us>    ("min_us",     uint16_t(500)).range(300, 2700),
        prop<&Sv::max_us>    ("max_us",     uint16_t(2500)).range(300, 2700),
        prop<&Sv::default_us>("default_us", uint16_t(1500)).range(300, 2700),
        prop<&Sv::speed>     ("speed",      uint16_t(4000)),
        prop<&Sv::accel>     ("accel",      uint16_t(8000)),
        prop<&Sv::decel>     ("decel",      uint16_t(8000)),
        prop<&Sv::reversed>  ("reversed",   false)
    ),

    // ---- Landing light groups ----
    seq<&LightProgramConfig::landingGroups, &LightProgramConfig::landingGroupCount>(
        "landing_groups",
        prop<&Lg::name>              ("name",             ""),
        prop<&Lg::servoBoard>        ("servo_board",      "lightfx"),
        prop<&Lg::servoId>           ("servo_id",         uint8_t(0))  .range(0, 8),
        prop<&Lg::lightfxChannelMask>("lightfx_channels", uint8_t(0)),
        prop<&Lg::hubfxChannelMask>  ("hubfx_channels",   uint8_t(0)),
        prop<&Lg::brightness>        ("brightness",       uint8_t(100)).range(0, 100)
    ),

    // ---- Input for program selection ----
    group<&LightProgramConfig::input>("input",
        prop<&In::channel>("channel", uint8_t(1)).range(1, 10),

        seq<&In::bands, &In::bandCount>("bands",
            prop<&Bd::min_us> ("min_us",  uint16_t(0)).range(800, 2200),
            prop<&Bd::max_us> ("max_us",  uint16_t(0)).range(800, 2200),
            prop<&Bd::program>("program", uint8_t(0)) .range(0, LightProgramLimits::MAX_PROGRAMS - 1)
        )
    ),

    // ---- Channel programs ----
    seq<&LightProgramConfig::programs, &LightProgramConfig::programCount>("programs",
        prop<&Pg::name>("name", ""),

        // NOTE: group_policies is a bare-scalar sequence (e.g. [0, 1, 2])
        // that the DSL cannot express.  It must be populated manually
        // in firmware config-loading code after schema populate().

        seq<&Pg::channels, &Pg::channelCount>("channels",
            prop<&Ch::board>     ("board",       "lightfx"),
            prop<&Ch::channel>   ("channel",     uint8_t(0)).range(0, 8),
            prop<&Ch::groupIndex>("group",       uint8_t(0xFF)),

            seq<&Ch::events, &Ch::eventCount>("events",
                prop<&Ev::type>          ("type",            "off"),
                prop<&Ev::duration_ms>   ("duration_ms",     uint16_t(0)),
                prop<&Ev::cycle_ms>      ("cycle_ms",        uint16_t(0)),
                prop<&Ev::brightness>    ("brightness",      uint8_t(100)).range(0, 100),
                prop<&Ev::pwm_duty>      ("pwm_duty",        uint8_t(0))  .range(0, 100),
                prop<&Ev::duty>          ("duty",            uint8_t(50)) .range(0, 100),
                prop<&Ev::flash_pct>     ("flash_pct",       uint8_t(15)) .range(0, 100),
                prop<&Ev::min_brightness>("min_brightness",  uint8_t(0))  .range(0, 100),
                prop<&Ev::max_brightness>("max_brightness",  uint8_t(100)).range(0, 100)
            )
        )
    )
);

} // namespace light_program_config

#endif // LIGHT_PROGRAM_CONFIG_H
