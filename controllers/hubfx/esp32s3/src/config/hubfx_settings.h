/*
 * HubFX Settings — Data Struct + Schema for /hubfx.yaml
 *
 * Per Rule 26, hub-only settings live in `/hubfx.yaml`. This file holds
 * everything that belongs to the hub itself (audio codec, RC input routing,
 * indicator brightness, …) — NOT settings that the hub fans out to a slave
 * (those live in /enginefx.yaml, /gunfx.yaml, /lightfx.yaml).
 *
 * Slave-board configs (engine_fx, gun_fx, light_fx) live in their own
 * top-level YAML files on the hub's flash — see [enginefx|gunfx|lightfx]
 * config headers. SD card is reserved for audio samples.
 *
 * INPUT ABSTRACTION (Phase 0)
 * ----------------------------------------------------------------------------
 * The hub exposes seven RC-input pins (I1..I7 = GPIO 5,6,7,10,11,12,13) and
 * supports four reader modes — one active per binary, picked at boot from
 * `inputs.mode`:
 *
 *   gpio  — each I-pin is an individual PWM input (1..7 channels)
 *   ppm   — single I-pin carries a multiplexed PPM sum (1..16 channels)
 *   sbus  — Futaba S.Bus on a UART-capable I-pin            [stub]
 *   jeti  — Jeti EX Bus on a UART-capable I-pin             [stub]
 *   none  — disable all input dispatch
 *
 * Channel numbers in the feature mappings are 1-based and refer to:
 *   - The I-pin index (1..7) when mode=gpio
 *   - The PPM/SBUS/Jeti channel number (1..16) when mode=ppm/sbus/jeti
 *
 * A channel of 0 disables the feature.
 *
 * YAML structure (at root of /hubfx.yaml):
 *   codec_supply_voltage: "12v"    # 12v | 15v | 20v | 24v
 *   inputs:
 *     mode: gpio                   # gpio | ppm | sbus | jeti | none
 *     ppm_pin: 1                   # I-pin (1..7) carrying PPM/SBUS/Jeti signal
 *     engine_on_off:
 *       channel: 1                 # 0=disabled, else 1..7 (gpio) or 1..16 (ppm)
 *       threshold_us: 1500         # PWM crossing threshold (engine on/off edge)
 *     light_program_select:
 *       channel: 0                 # 0=disabled
 *       program_count: 4           # PWM µs range divided into N program slots
 */

#ifndef HUBFX_SETTINGS_H
#define HUBFX_SETTINGS_H

#include <cstdint>
#include <cstring>
#include <config/yaml_schema.h>

// ============================================================================
// Data Struct
// ============================================================================

/// Reader mode identifiers. Stored as a 6-byte string so the YAML enum value
/// can be parsed and validated declaratively.
struct HubFxInputMode {
    static constexpr const char* NONE = "none";
    static constexpr const char* GPIO = "gpio";
    static constexpr const char* PPM  = "ppm";
    static constexpr const char* SBUS = "sbus";
    static constexpr const char* JETI = "jeti";
};

struct HubFxInputs {
    /// Reader mode: "none" | "gpio" | "ppm" | "sbus" | "jeti".
    /// Default disables all input dispatch.
    char mode[6] = "none";

    /// I-pin (1..7) carrying the multiplexed signal in ppm/sbus/jeti modes.
    /// Ignored when mode=gpio (each I-pin is its own channel).
    uint8_t ppmPin = 1;

    /// Engine on/off feature mapping.
    struct EngineOnOff {
        uint8_t  channel = 0;          ///< 0=disabled
        uint16_t threshold_us = 1500;  ///< PWM crossing threshold
    } engineOnOff;

    /// Light program selector — divides PWM range into program_count slots.
    struct LightProgramSelect {
        uint8_t channel = 0;           ///< 0=disabled
        uint8_t programCount = 1;      ///< Number of selectable programs (>=1)
    } lightProgramSelect;
};

struct HubFxSettings {
    /// TAS5825M supply voltage: "12v", "15v", "20v", "24v"
    /// Must match the physical PVDD supply — controls analog gain (output swing).
    /// Default: "12v" (3S LiPo). Changing this at runtime reconfigures analog gain.
    char codecSupplyVoltage[4] = "12v";

    /// RC input dispatch — see HubFxInputs above.
    HubFxInputs inputs;
};

// ============================================================================
// Declarative Schema (fields at YAML root of /hubfx.yaml)
// ============================================================================

namespace hubfx_settings {

using namespace sfx;
using I  = HubFxInputs;
using IE = HubFxInputs::EngineOnOff;
using IL = HubFxInputs::LightProgramSelect;

inline const auto fields = schema<HubFxSettings>(
    prop<&HubFxSettings::codecSupplyVoltage>("codec_supply_voltage", "12v"),

    group<&HubFxSettings::inputs>("inputs",
        prop<&I::mode>  ("mode",    "none"),
        prop<&I::ppmPin>("ppm_pin", uint8_t(1)).range(1, 7),

        group<&I::engineOnOff>("engine_on_off",
            prop<&IE::channel>     ("channel",      uint8_t(0)).range(0, 16),
            prop<&IE::threshold_us>("threshold_us", uint16_t(1500)).range(800, 2200)
        ),

        group<&I::lightProgramSelect>("light_program_select",
            prop<&IL::channel>     ("channel",       uint8_t(0)).range(0, 16),
            prop<&IL::programCount>("program_count", uint8_t(1)).range(1, 32)
        )
    )
);

} // namespace hubfx_settings

// ============================================================================
// ConfigStore Schema Adapter — owns /hubfx.yaml
// ============================================================================

struct HubFxSettingsSchema {
    using DataType = HubFxSettings;

    template<typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        return hubfx_settings::fields.populate(d, p.root());
    }

    static bool validate(const DataType& d, char* err, size_t errLen) {
        return hubfx_settings::fields.validate(d, err, errLen);
    }

    static const char* defaultPath() { return "/hubfx.yaml"; }
};

#endif // HUBFX_SETTINGS_H
