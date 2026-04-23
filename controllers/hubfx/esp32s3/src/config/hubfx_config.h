/*
 * HubFX Configuration — Per-Domain YAML Files (Rule 26)
 *
 * The hub stores configuration in FOUR independent files on its flash, one
 * per domain. The SD card is reserved for audio samples — it never holds
 * config. Each file is owned by its own ConfigStore<TSchema>; a
 * MultiConfigServer routes CONFIG_RELOAD/SAVE/STATUS by path so the host
 * (Studio / CLI) can target one file at a time.
 *
 *   /hubfx.yaml      — HubFxSettings       (hub-only: codec, input mappings, …)
 *   /enginefx.yaml   — EngineConfig        (engine FX state machine)
 *   /gunfx.yaml      — GunFxHubConfig      (hub-side gun audio routing)
 *   /lightfx.yaml    — LightProgramConfig  (master copy fanned to LightFX slaves)
 *
 * The three slave-board files mirror the same shapes the slave firmware
 * loads from /enginefx.yaml, /gunfx.yaml, /lightfx.yaml on its own flash —
 * Studio writes the same YAML to either side. Battery monitoring is NOT
 * in YAML on the hub: the INA226 rail channel is hardcoded and chemistry
 * / cell count come from the runtime BATTERY_CONFIG (0xEE) packet.
 *
 * Why per-file split?
 *   - Each domain reloads independently — `config.reload /lightfx.yaml`
 *     does not re-parse engine + gun + audio.
 *   - Studio edits one tab at a time, so the wire payload stays small.
 *   - Slave-board configs are interchangeable hub<->slave (same schema +
 *     same default path), simplifying push-down / pull-up flows.
 *
 * NOT a `HubFxConfig` composite struct anymore — there is no master
 * document. See instructions/12-LIGHTFX-MODERNIZATION.md.
 */

#ifndef HUBFX_CONFIG_H
#define HUBFX_CONFIG_H

#include "hubfx_settings.h"
#include "engine_config.h"
#include "gunfx_hub_config.h"
#include "lightfx_hub_config.h"

#include <config/config_store.h>

// ============================================================================
// YAML Parser Pool — shared across all per-domain stores
// ============================================================================
//
// Each ConfigStore<TSchema, TPool> instantiates a YamlParser<TPool> on the
// stack during loadFromString(); the parser heap-allocates its node + string
// pools, then frees them when loadFromString returns. So this preset only
// affects the brief peak during config.reload / boot.
//
// /lightfx.yaml is the largest file (programs × channels × events, landing
// groups, servo bindings) — it dominates the sizing. The other three files
// are small enough that they could use DefaultYamlPool, but sharing keeps
// the .ino single-line per-store and gives every file the same headroom.
//
// Sizing (2048 nodes / 64 KB strings / depth 16):
//   - Nodes:   2048 × ~24 B  = ~48 KB
//   - Strings:                  64 KB
//   - Peak:                   ~112 KB transient during parse
//
// ESP32-S3 has 512 KB SRAM + 8 MB PSRAM; YAML parser allocations land in
// internal SRAM via `new[]` (PSRAM allocator is opt-in). 112 KB peak is
// well inside the available internal-heap budget.
struct HubFxYamlPool {
    static constexpr size_t MAX_NODES        = 2048;
    static constexpr size_t STRING_POOL_SIZE = 65536;
    static constexpr size_t MAX_DEPTH        = 16;
};

// ============================================================================
// ConfigStore Type Aliases — one per YAML file
// ============================================================================

using HubFxSettingsStore = ConfigStore<HubFxSettingsSchema,    HubFxYamlPool>;
using EngineConfigStore  = ConfigStore<EngineConfigSchema,     HubFxYamlPool>;
using GunFxConfigStore   = ConfigStore<GunFxHubConfigSchema,   HubFxYamlPool>;
using LightFxConfigStore = ConfigStore<HubLightFxConfigSchema, HubFxYamlPool>;

#endif // HUBFX_CONFIG_H
