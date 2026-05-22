# sfx_config — Configuration Management Library

## Overview

`sfx_config` provides a complete configuration pipeline for ScaleFX embedded controllers:

1. **YamlParser** — Lightweight YAML subset parser (maps, sequences, scalars)
2. **YamlScalarConvert\<T\>** — Extensible traits for string→type conversion
3. **YamlNode** — Tree node with `as<T>()`, `child()`, `childAs<T>()` member access
4. **YamlSchema** — Declarative schema DSL (`sfx::prop`, `sfx::group`, `sfx::seq`)
5. **ConfigStore** — Templatized config manager with schema-driven loading and validation
6. **ConfigServerT** — BusServer handler for single-store CONFIG_RELOAD / CONFIG_STATUS protocol commands (slaves)
7. **MultiConfigServer** — Path-routed BusServer handler for hubs that own multiple per-domain YAML files
8. **ConfigClient** — BusClient for sending config commands

The config format is **defined per board** via a Schema type — the library itself is generic.

## Architecture

```
config.yaml (on SD/Flash)
    │
    ▼
YamlParser<TPool>          ─── Parses YAML text → node tree
    │
    ▼
YamlNode tree              ─── node->as<T>(), node->childAs<T>(), node->child()
    │                          YamlScalarConvert<T> traits for type conversion
    ▼
YamlSchema (optional)      ─── sfx::schema / group / prop / seq
    │                          Auto-generates populate() + validate()
    ▼
ConfigStore<TSchema>       ─── TSchema::populate() maps tree → C++ struct
    │                          TSchema::validate() checks constraints
    ▼
ConfigServerT<TStore>      ─── BusServer handler (CONFIG_RELOAD/GET protocol)
    │
    ▼
ConfigClient               ─── BusClient for sending config commands
```

### Typed Access API

Three levels of typed access, from high-level to low-level:

```cpp
// 1. Parser path queries — dot-separated, most convenient
int rpm = parser.get<int32_t>("gun_fx.rates_of_fire[0].rpm", 200);
const char* name = parser.getString("engine_fx.type", "turbine");  // Named alias

// 2. Node member functions — for sequence item traversal
auto* item = parser.sequenceItem("rates_of_fire", 0);
int rpm    = item->childAs<int32_t>("rpm", 200);   // Find child + convert
const char* name = item->childAs<const char*>("name", "");

// 3. Direct node conversion — when you already have the node
auto* node = parser.find("engine_fx.threshold_us");
uint32_t val = node->as<uint32_t>(1500);
```

Extend with custom types by specializing `YamlScalarConvert<T>`:
```cpp
template<> struct YamlScalarConvert<MyEnum> {
    static MyEnum parse(const char* str, MyEnum def) {
        if (strcmp(str, "alpha") == 0) return MyEnum::Alpha;
        return def;
    }
};
// Now works: parser.get<MyEnum>("path", MyEnum::Alpha);
```

## Quick Start

### 1. Define Your Config Data Struct

```cpp
// In your controller's config header (e.g., hubfx_config.h)

struct HubFxConfigData {
    // engine_fx section
    struct EngineFx {
        bool enabled = true;
        char type[16] = "turbine";
        uint8_t inputChannel = 1;
        uint16_t threshold_us = 1500;
        char startSound[64]  = "/sounds/ka50/engine_start.wav";
        char runSound[64]    = "/sounds/ka50/engine_loop.wav";
        char stopSound[64]   = "/sounds/ka50/engine_stop.wav";
        uint32_t startingOffset_ms = 60000;
        uint32_t stoppingOffset_ms = 25000;
    } engineFx;

    // gun_fx section
    struct GunFx {
        uint8_t triggerChannel = 2;
        uint8_t heaterToggleChannel = 3;
        uint16_t heaterThreshold_us = 1500;
        uint32_t fanOffDelay_ms = 2000;

        static constexpr int MAX_RATES = 4;
        struct RateOfFire {
            char name[16] = {};
            uint16_t rpm = 200;
            uint16_t pwmThreshold_us = 1300;
            char soundFile[64] = {};
        };
        RateOfFire rates[MAX_RATES] = {};
        uint8_t rateCount = 0;
    } gunFx;
};
```

### 2. Define the Schema

Two approaches — choose based on complexity:

#### Option A: Declarative Schema (recommended)

Use `yaml_schema.h` for automatic populate/validate generation:

```cpp
#include <config/yaml_schema.h>

using namespace sfx;
using E = HubFxConfigData::EngineFx;
using G = HubFxConfigData::GunFx;
using R = G::RateOfFire;

inline const auto hubFxFields = schema<HubFxConfigData>(
    group<&HubFxConfigData::engineFx>("engine_fx",
        prop<&E::enabled>      ("enabled",       true),
        prop<&E::type>         ("type",          "turbine"),
        prop<&E::inputChannel> ("input_channel", uint8_t(1)).range(1, 10),
        prop<&E::threshold_us> ("threshold_us",  uint16_t(1500)),
        prop<&E::startSound>   ("start_sound",   "/sounds/ka50/engine_start.wav"),
        prop<&E::runSound>     ("run_sound",     "/sounds/ka50/engine_loop.wav"),
        prop<&E::stopSound>    ("stop_sound",    "/sounds/ka50/engine_stop.wav")
    ),
    group<&HubFxConfigData::gunFx>("gun_fx",
        prop<&G::triggerChannel>      ("trigger_channel",       uint8_t(2)).range(1, 10),
        prop<&G::heaterToggleChannel> ("heater_toggle_channel", uint8_t(3)).range(1, 10),
        prop<&G::heaterThreshold_us>  ("heater_threshold_us",   uint16_t(1500)),
        prop<&G::fanOffDelay_ms>      ("fan_off_delay_ms",      uint32_t(2000)),
        seq<&G::rates, &G::rateCount>("rates_of_fire",
            prop<&R::name>            ("name",             ""),
            prop<&R::rpm>             ("rpm",              uint16_t(200)).range(50, 3000),
            prop<&R::pwmThreshold_us> ("pwm_threshold_us", uint16_t(1300)),
            prop<&R::soundFile>       ("sound_file",       "")
        )
    )
);

struct HubFxConfigSchema {
    using DataType = HubFxConfigData;
    static bool populate(DataType& d, const YamlParser<>& p) {
        return hubFxFields.populate(d, p.root());
    }
    static bool validate(const DataType& d, char* e, size_t el) {
        return hubFxFields.validate(d, e, el);
    }
    static const char* defaultPath() { return "/hubfx.yaml"; }  // per-board name — Rule 26
};
```

> **Convention (Rule 26):** each controller's schema returns its own filename
> (`/gearcontrol.yaml`, `/lightfx.yaml`, `/hubfx.yaml`, `/gunfx.yaml`). The generic
> `/config.yaml` is only a fallback for unidentified boards. Studio's
> `configPathFor(ControllerType)` mirrors this mapping.

**Schema DSL primitives:**

| Function | Purpose | Example |
|----------|---------|--------|
| `sfx::prop<&S::field>(key, default)` | Scalar/string field | `prop<&E::rpm>("rpm", uint16_t(200))` |
| `.range(min, max)` | Add numeric validation | `.range(50, 3000)` |
| `sfx::group<&S::sub>(key, children...)` | Nested YAML map → sub-struct | `group<&Config::gunFx>("gun_fx", ...)` |
| `sfx::seq<&S::arr, &S::cnt>(key, children...)` | YAML sequence → array + count | `seq<&G::rates, &G::rateCount>("rates", ...)` |
| `sfx::schema<TData>(children...)` | Top-level schema container | `schema<MyConfig>(...)` |
| `sfx::embed<&S::sub>(key, subSchema)` | Embed existing schema as group | `embed<&Hub::engine>("engine_fx", engineFields)` |
| `schema.asGroup<&S::sub>(key)` | Method form of embed | `engineFields.asGroup<&Hub::engine>("engine_fx")` |

**Auto-generated capabilities:**
- `populate()` — reads every field from the YAML tree, applies defaults for missing keys
- `validate()` — checks `.range()` constraints, returns first failure with error message
- Sequence max capacity auto-deduced from the array extent (`std::extent_v`)
- Supports arbitrary nesting depth (groups inside groups)
- `char[N]` arrays handled automatically (strncpy + null termination)

**Schema Composition (embedding sub-schemas):**

When sections are defined in separate files, compose them into a parent schema
without duplicating field definitions. Use `asGroup()` or `sfx::embed()`:

```cpp
// engine_config.h — standalone section schema
struct EngineConfig { bool enabled = true; /* ... */ };

namespace engine_config {
inline const auto fields = schema<EngineConfig>(
    prop<&EngineConfig::enabled>("enabled", true)
    // ... all engine fields
);
}

// hubfx_config.h — top-level config composing sections
struct HubFxConfig {
    EngineConfig engineFx;
    // GunFxConfig gunFx;  (add later)
};

namespace hubfx_config {
inline const auto fields = schema<HubFxConfig>(
    // Method syntax:
    engine_config::fields.asGroup<&HubFxConfig::engineFx>("engine_fx")

    // Or free-function syntax (equivalent):
    // embed<&HubFxConfig::engineFx>("engine_fx", engine_config::fields)
);
}
```

The `asGroup<&Parent::member>(key)` method re-wraps the sub-schema's children
into a `YamlGroup`, so the YAML key mapping and field bindings stay in the
section file.  A `static_assert` ensures the member type matches the schema's
`DataType`.

#### Option B: Hand-Written Schema

For maximum control or unusual parsing logic:

```cpp
struct HubFxConfigSchema {
    using DataType = HubFxConfigData;

    static bool populate(DataType& d, const YamlParser<>& p) {
        // engine_fx
        d.engineFx.enabled = p.getBool("engine_fx.enabled", true);
        strncpy(d.engineFx.type,
                p.getString("engine_fx.type", "turbine"),
                sizeof(d.engineFx.type) - 1);
        d.engineFx.inputChannel = (uint8_t)p.getInt(
            "engine_fx.engine_toggle.input_channel", 1);
        d.engineFx.threshold_us = (uint16_t)p.getUInt(
            "engine_fx.engine_toggle.threshold_us", 1500);

        // Sequence: rates_of_fire
        int n = p.sequenceLength("gun_fx.rates_of_fire");
        d.gunFx.rateCount = (n > DataType::GunFx::MAX_RATES)
                          ? DataType::GunFx::MAX_RATES : (uint8_t)n;
        for (int i = 0; i < d.gunFx.rateCount; i++) {
            auto* item = p.sequenceItem("gun_fx.rates_of_fire", i);
            if (!item) continue;
            auto& rate = d.gunFx.rates[i];
            strncpy(rate.name,
                    item->childAs<const char*>("name", ""),
                    sizeof(rate.name) - 1);
            rate.rpm = (uint16_t)item->childAs<int32_t>("rpm", 200);
            rate.pwmThreshold_us = (uint16_t)item->childAs<int32_t>(
                "pwm_threshold_us", 1300);
            strncpy(rate.soundFile,
                    item->childAs<const char*>("sound_file", ""),
                    sizeof(rate.soundFile) - 1);
        }
        return true;
    }

    static bool validate(const DataType& d, char* err, size_t errLen) {
        if (d.engineFx.inputChannel < 1 || d.engineFx.inputChannel > 10) {
            snprintf(err, errLen, "engine_toggle.input_channel out of range (1-10)");
            return false;
        }
        if (d.gunFx.triggerChannel < 1 || d.gunFx.triggerChannel > 10) {
            snprintf(err, errLen, "trigger.input_channel out of range (1-10)");
            return false;
        }
        return true;
    }

    static const char* defaultPath() { return "/hubfx.yaml"; }
};
```

### 3. Wire It Up in Firmware (single-store / slave pattern)

```cpp
#include <config/config_store.h>
#include <server/config_server.h>
#include <storage/sd_card.h>

// Type aliases
using MyConfigStore  = ConfigStore<HubFxConfigSchema>;
using MyConfigServer = ConfigServerT<MyConfigStore>;

MyConfigServer configServer;

// File reader bridge
int readFromSd(const char* path, char* buf, size_t maxLen) {
    auto& sd = SdCardModule::instance();
    return sd.readFile(path, (uint8_t*)buf, maxLen);
}

void setup() {
    // ... SD init ...

    configServer.store().setFileReader(readFromSd);
    configServer.loadConfig();  // Loads the schema's defaultPath() — e.g. /hubfx.yaml

    // Access parsed config
    auto& cfg = configServer.store().data();
    if (cfg.engineFx.enabled) {
        // ... start engine with cfg.engineFx.type ...
    }

    // Register as protocol handler
    server.addModuleHandler(&configServer);
}
```

### 3b. Multi-Store Wiring (hub pattern, Rule 26)

Hubs split configuration across one YAML file per domain — Studio writes the
slave-board configs (`enginefx.yaml`, `gunfx.yaml`, `lightfx.yaml`) to hub
flash alongside the hub's own `hubfx.yaml`. Each file is owned by its own
`ConfigStore<TSchema>` with its own `defaultPath()` and its own typed
`onLoaded()` callback. `MultiConfigServer` routes the wire packets to the
right store by matching the path payload.

```cpp
#include <server/multi_config_server.h>
#include <storage/storage_config_bridge.h>

static HubFxSettingsStore hubConfig;       // /hubfx.yaml   (codec, input mappings, …)
static EngineConfigStore  engineConfig;    // /enginefx.yaml
static GunFxConfigStore   gunConfig;       // /gunfx.yaml
static LightFxConfigStore lightConfig;     // /lightfx.yaml

static ConfigStoreFacadeT<HubFxSettingsStore> hubFacade    (hubConfig);
static ConfigStoreFacadeT<EngineConfigStore>  engineFacade (engineConfig);
static ConfigStoreFacadeT<GunFxConfigStore>   gunFacade    (gunConfig);
static ConfigStoreFacadeT<LightFxConfigStore> lightFacade  (lightConfig);

MultiConfigServer configServer;

void setup() {
    wireConfigStore<FlashModule>(hubConfig);
    wireConfigStore<FlashModule>(engineConfig);
    wireConfigStore<FlashModule>(gunConfig);
    wireConfigStore<FlashModule>(lightConfig);

    hubConfig.onLoaded   ([](const HubFxSettings& c)     { /* apply */ });
    engineConfig.onLoaded([](const EngineConfig& c)      { /* apply */ });
    gunConfig.onLoaded   ([](const GunFxHubConfig& c)    { /* apply */ });
    lightConfig.onLoaded ([](const LightProgramConfig& c){ /* apply */ });

    configServer.addStore(hubFacade);
    configServer.addStore(engineFacade);
    configServer.addStore(gunFacade);
    configServer.addStore(lightFacade);
    configServer.loadAll();         // initial load of every YAML
    server.addModuleHandler(&configServer);
}
```

Wire-protocol semantics:

- `CONFIG_RELOAD` with path → reload that store; without path → reload ALL.
- `CONFIG_SAVE` always requires an explicit path (multi-store save is
  ambiguous without one).
- `CONFIG_STATUS` reports aggregate state: `loaded` is set iff every
  registered store is loaded; `fileSize` is the sum across all stores;
  `validOk` is the AND of every store's current `validate()`.

## YAML Subset Supported

| Feature | Supported | Example |
|---------|-----------|---------|
| Block mappings | ✓ | `key: value` |
| Nested maps | ✓ | `parent:\n  child: value` |
| Block sequences | ✓ | `- item` |
| Sequences of maps | ✓ | `- name: foo\n  rpm: 200` |
| Scalars: strings | ✓ | `plain`, `"quoted"`, `'single'` |
| Scalars: integers | ✓ | `42`, `-1`, `0` |
| Scalars: floats | ✓ | `3.14`, `-0.5` |
| Scalars: booleans | ✓ | `true/false`, `yes/no`, `on/off` |
| Comments | ✓ | `# comment`, `key: val  # inline` |
| Flow collections | ✗ | `{a: 1}`, `[1, 2]` |
| Multi-line scalars | ✗ | `\|`, `>` |
| Anchors/aliases | ✗ | `&ref`, `*ref` |
| Tags | ✗ | `!!int`, `!!str` |

### Canonical YAML Style (Rule 27)

All ScaleFX YAML — reference files in `controllers/*/pico/config.yaml`, files
produced by Studio's `generateGearControlYaml` / `generateLightYaml`, and the
round-tripped device files — uses **indented block sequences**:

```yaml
retracts:
  - channel: 0           # sequence items are 2 spaces under the parent key
    enabled: true        # continuation lines are 4 spaces under the parent key
    stall_current_mA: 500
    timeout_ms: 60000
  - channel: 1
    enabled: true
    stall_current_mA: 500
    timeout_ms: 60000
```

The parsers also accept the YAML-spec "compact" form (sequence items at the
**same** indent as the parent key) for backward compatibility with files
produced by older Studio builds:

```yaml
retracts:
- channel: 0             # legal YAML, still parsed by firmware + Go + TS parsers,
  enabled: true          # but no generator should emit this form any more.
```

### Flow collections (hand-authored convenience)

The firmware `YamlParser` (`parseFlowNode`) and the Studio TS parser
(`parseFlowValue`) both accept **single-line flow collections** — a compact map
or sequence for small leaf objects. A flow `{`/`[` must close on the same line;
flow and block nest freely:

```yaml
ports:
  - { kind: pwm, idx: 0, role: led_animator, label: "Beacon" }
channels:
  - port: { kind: pwm, idx: 0 }   # flow value, block events below
    events:
      - kind: "on"
        brightness_pct: 100
```

Flow is an **input** convenience only. Emitters (Studio generators, the
firmware load→save round-trip) always write the indented block form, so a Save
normalises a hand-authored flow file back to block. When authoring new YAML by
hand, either form parses; reach for flow when a leaf object is small enough that
a 3-line block hurts readability.

## Pool Configuration

| Pool | MAX_NODES | STRING_POOL | MAX_DEPTH | Use Case |
|------|-----------|-------------|-----------|----------|
| `DefaultYamlPool` | 128 | 4 KB | 10 | Typical 2-4 KB configs |
| `LargeYamlPool` | 512 | 16 KB | 16 | Complex configs on ESP32 PSRAM |

Custom pools:
```cpp
struct MyPool {
    static constexpr size_t MAX_NODES       = 256;
    static constexpr size_t STRING_POOL_SIZE = 8192;
    static constexpr size_t MAX_DEPTH       = 12;
};
ConfigStore<MySchema, MyPool> store;
```

## Protocol

Uses the existing HubFxPacket types (0x90-0x92):

| Command | Type | Payload (Request) | Response |
|---------|------|-------------------|----------|
| `CONFIG_RELOAD` | 0x90 | `[]` or `[pathLen:u8][path:str]` | ACK / NACK + CONFIG_ERROR |
| `CONFIG_STATUS` | 0x91 | `[]` | CONFIG_STATUS_RESP |
| `CONFIG_STATUS_RESP` | 0x92 | `[loaded:u8][fileSize:u16LE][validOk:u8]` | — |

## File Structure

```
sfx_config/
├── library.json
├── README.md
├── config/
│   ├── yaml_parser.h      — YamlParser<TPool> header
│   ├── yaml_parser.ipp    — YamlParser template implementation
│   ├── yaml_schema.h      — Declarative schema DSL (sfx::prop/group/seq/schema)
│   ├── config_store.h     — ConfigStore<TSchema, TPool> header
│   └── config_store.ipp   — ConfigStore template implementation
├── server/
│   ├── config_server.h    — ConfigServerT<TConfigStore> header
│   └── config_server.ipp  — ConfigServerT template implementation
└── client/
    ├── config_client.h    — ConfigClient header
    └── config_client.cpp  — ConfigClient implementation
```

## Dependencies

- `sfx_platform` — DiagLog, SFX_LOG macros
- `sfx_serial` — BusServer, BusClient, CoreProtocol, HubFxPacket, HubFxError
