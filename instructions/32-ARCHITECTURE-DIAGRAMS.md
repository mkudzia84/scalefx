# 32 — Architecture Diagrams

> **Status:** active reference &middot; **Read when:** you need a visual map of storage / audio / ports-roles-topology / effects data flow. &middot; **TL;DR:** Mermaid diagrams of the four core subsystems reflecting the post-decomposition `BoardServer<...>` codebase and Rule 58 transparent expander roles.

Visual reference for the four core subsystems. Diagrams are Mermaid (GitHub /
VS Code render them inline). They reflect the post-decomposition codebase
(2026-06-09): `BoardServer<...UserPolicies>` framework, the decomposed
storage / audio / role layers, and Rule 58 transparent expander roles.

Cross-cutting conventions used below:
- **⚠️ boundary** = a core boundary (dual-core) or a hub↔expander (USB-CDC) boundary.
- A `*ServicePolicy` is a `SystemServicePolicy` composed into a board's
  `BoardServer<...>` pack (`kCapabilityBits / begin / ownsType / handle / update`).

---

## 1. Storage

`StorageServicePolicy<TPolicy>` is the filesystem-query + dispatch core;
`UploadEngine<TPolicy>` is the exclusive upload state machine pulled out of it
(reaches the shared buffer + platform policy via a back-reference). Stateless
helpers live in `storage_path_util.h`. The platform `TPolicy`
(`Esp32StoragePolicy` / `PicoStoragePolicy`) is chosen at compile time.

Files: [storage_service.h/.ipp](../controllers/lib/sfx_storage/server/storage_service.h),
[storage_upload_engine.h/.ipp](../controllers/lib/sfx_storage/server/storage_upload_engine.h),
[storage_path_util.h](../controllers/lib/sfx_storage/server/storage_path_util.h),
[esp32/](../controllers/lib/sfx_storage/server/esp32/) · [pico/](../controllers/lib/sfx_storage/server/pico/).

```mermaid
flowchart TB
    wire["COBS wire (0x93..0xA5, 0xA9, 0xB0)"] --> dispatch

    subgraph SSP["StorageServicePolicy&lt;TPolicy&gt; — SystemServicePolicy (cap: FLASH | SD)"]
        dispatch["handle() / ownsType()"]
        fileops["file-ops:\nsd-init · sd/flash-status\nlist · tree · info\ndelete · mkdir · download"]
        shared["StorageSharedState\nfill buffer + uploadFile + flushUploadBuffer()"]
        policy["TPolicy _policy\n(platform backend)"]
        upload["UploadEngine&lt;TPolicy&gt;\n(owns by value; back-ref _svc)"]
        dispatch -->|"file ops"| fileops
        dispatch -->|"0xA0..0xA5"| upload
    end

    pathutil["sfx_storage:: free fns\ntargetName · mapStorageError\nisValidPath · extractPathAndTarget"]
    fileops -.uses.-> pathutil
    upload -.uses.-> pathutil
    upload -->|"_svc._shared / _svc._policy"| shared
    upload --> policy

    subgraph UENG["UploadEngine modes"]
        sync["WINDOWED / sync\nper-chunk seq + CRC-16 + ACK"]
        batch["BATCH / stream\nraw bytes (COBS bypassed)\nsegment ACK = flow control"]
    end
    upload --> sync
    upload --> batch

    policy --> flash["FlashModule (LittleFS)"]
    policy --> sd["SdCardModule"]
    fileops --> flash
    fileops --> sd

    subgraph BK["Backends (StorageFile alias)"]
        esp["ESP32: native ESP-IDF VFS\nesp_vfs_fat_sdmmc / esp_littlefs\nNativeFile (POSIX, Core-0 inline write)"]
        pico["Pico: SdFat + Arduino LittleFS"]
    end
    flash --> esp
    sd --> esp
    flash --> pico
```

**Batch (stream) upload — the wire-critical path (Rules 53/54/56/57):**

```mermaid
sequenceDiagram
    participant C as Client (CLI/Studio)
    participant U as UploadEngine (Core 0)
    participant S as SD / Flash
    C->>U: FILE_UPLOAD_BEGIN (size, path, mode=stream)
    U->>U: open + pre-alloc file, alloc 32KB fill buf
    U-->>C: ACK [segment_size][segment_count]
    Note over C,U: ⚠️ RAW mode — COBS bypassed, wire held exclusively<br/>(keepalive gated off, no concurrent commands)
    loop per 16 KB segment
        C->>U: raw segment bytes (blast, no per-chunk ACK)
        U->>U: fill buf → MD5 → flush at boundary
        U->>S: synchronous inline write (Core 0)
        U-->>C: FILE_UPLOAD_PROGRESS (TAG_ASYNC) — back-pressure window
    end
    C->>U: FILE_UPLOAD_END (COBS)
    U->>S: final flush + close
    U-->>C: ACK [md5:16B]  (hashed over the WIRE stream)
```

Exclusivity: `onUploadStart`/`onUploadEnd` suspend audio + other subsystems for
the duration; `wireUploadExclusivity<Mixer>(storage)` wires the audio side.

---

## 2. Audio + audio pipeline

`AudioMixer<TI2S,TCodec>` decomposed into one class per responsibility:
`WavState` (per-channel SPSC decode ring + seq_cst teardown), `MixKernel`
(Q15/float mix), `DecoderWorker` (Core-0 decode task), `SoundQueue` (per-channel
FIFO). The pipeline is dual-core: **Core 0 decodes, Core 1 mixes + plays.**

Files: [audio_mixer.h](../controllers/lib/sfx_audio/audio/audio_mixer.h) (+ `audio_mixer_{wavstate,mixkernel,decoder}.ipp`),
[audio_source.h](../controllers/lib/sfx_audio/audio/audio_source.h),
[esp_dual_core_audio.h](../controllers/lib/sfx_audio/audio/esp_dual_core_audio.h).

```mermaid
flowchart LR
    subgraph C0["⚠️ Core 0 — protocol + decode"]
        cmd["playAsync / stopAsync\n(protocol/command)"]
        cq["command queue\n_cmdQueue (PSRAM, mutex + atomics)"]
        dec["DecoderWorker (task, 5ms)\nsource→readFrames (float)\nfloat→int16 Q15"]
        cmd --> cq
    end

    subgraph SRC["IAudioSource (placement-new in Channel::sourceStorage)"]
        wstream["WavStreamSource (SD streaming)"]
        wpre["WavPreloadSource (PSRAM)"]
        mp3["Mp3PsramSource (libhelix + AssetCache)"]
    end
    dec --> wstream
    dec --> wpre
    dec --> mp3

    subgraph C1["⚠️ Core 1 — mix + output"]
        prod["Producer task\ndrain cmd queue → MixKernel"]
        mk["MixKernel\nproduceBlock (Q15+esp-dsp) /\nproduceBlockFloat\nresample (Q15 linear interp)"]
        oring["mixer output ring\n(RING_FRAMES = 4096 stereo)"]
        cons["Consumer (consume())"]
        prod --> mk --> oring --> cons
    end

    cq -->|dequeue| prod
    prod -->|"placement-new source,\nactive=release, notifyDecoder()"| dec

    subgraph CH["per-channel WavState (×8) — SPSC ring"]
        ring["bufL/bufR int16\nwriteIdx (dec→prod, release/acquire)\nreadIdx (prod→dec)\nactive · decoderBusy(seq_cst) · sourceExhausted"]
    end
    dec -->|"commitWrite (writer)"| ring
    ring -->|"readSample / commitRead (reader)"| mk

    cons --> i2s["EspI2SOutput (TI2S)\ni2s_std DMA (internal SRAM)"]
    i2s --> codec["TAS5825PCodec (TCodec)"]
    codec --> spk(["🔊 speakers (L/R)"])
```

Teardown safety (Rule 15): a rapid stop/start frees the source on the
producer/command side while the decoder may be mid-refill — guarded by the
`decoderBusy` **seq_cst** handshake (`destroySafe()` clears `active`, waits
`decoderBusy` false, then runs the dtor). Skipping it = `LoadProhibited@0`.

---

## 3. Ports, roles & topology

A board declares its ports; `BoardOf<>` sizes a `PortRegistry` and composes the
policy pack. `RoleServicePolicy` attaches a role into each port's
`std::variant` slot and dispatches drive/query packets to per-family handlers.
On the hub, `TopologyService` makes an expander's roles behave hub-locally
(Rule 58) via opaque, `PortRef`-addressed transport over USB-CDC.

Files: [board_of.h](../controllers/lib/sfx_board/server/board_of.h) · [board_server.h](../controllers/lib/sfx_board/server/board_server.h) · [port_registry.h](../controllers/lib/sfx_board/server/port_registry.h) · [port_bindings.h](../controllers/lib/sfx_board/server/port_bindings.h) · [role_service.h/.cpp](../controllers/lib/sfx_board/server/role_service.h) · `role_*_handler.*` · [role_event_emitter.h](../controllers/lib/sfx_board/server/role_event_emitter.h) · [role_registry.h](../controllers/lib/sfx_board/server/role_registry.h) · [topology_service](../controllers/hubfx/esp32s3/src/topology/) · [expander_service](../controllers/hubfx/esp32s3/src/expanders/).

```mermaid
flowchart TB
    board["Board : BoardOf&lt;TBoard, TStream, PortCapacity&lt;NS,NP,NH,NI&gt;, ...ExtraPolicies&gt;"]
    board --> bs["BoardServer&lt;TStream, ...&gt; — auto-prepends:\nBoardServicePolicy → IndicatorServicePolicy →\nPortServicePolicy → RoleServicePolicy → (user policies)"]

    subgraph REG["PortRegistry&lt;NS,NP,NH,NI&gt; — std::variant role slots"]
        sv["ServoBinding → ServoActuatorRole"]
        pw["PwmBinding → LedAnimator | DcMotorRole | HeaterRole"]
        hb["HBridgeBinding → BiDcMotorRole"]
        inp["InputBinding → RcPwm | Sbus | JetiEx | JetiExTelemetry"]
    end
    bs --> REG

    subgraph RSP["RoleServicePolicy (0x40..0x7F) — dispatch + per-loop tick"]
        rdisp["handle() switch"]
        servoH["ServoRoleHandler"]
        ledH["LedRoleHandler"]
        motorH["DcMotorRoleHandler"]
        bimotorH["BiMotorRoleHandler"]
        heaterH["HeaterRoleHandler"]
        inH["RcPwm/Sbus/Jeti InputHandler"]
        attach["applyAttach / handleBulkAttach (ROLE_BULK_ATTACH 0x57)"]
        emit["RoleEventEmitter (async out + local listener)"]
        rdisp --> servoH & ledH & motorH & bimotorH & heaterH & inH
    end
    bs --> RSP
    attach -->|"emplace role"| REG
    servoH & ledH & motorH & bimotorH & heaterH & inH -->|"std::get_if&lt;Role&gt;"| REG
    servoH & ledH & motorH & bimotorH & heaterH & inH -.callbacks.-> emit
    reg2["role_registry.h: roleKindFor&lt;T&gt;() / forEachAttachedRole\n(single enumeration map — Rule 58)"]
    attach -.-> reg2

    subgraph HUB["HubFX only — TopologyService (Rule 58)"]
        topo["addr = PortRef{guid, kind, idx}\nisLocalTarget(guid)?"]
        fwd["FORWARD 0x8F (cmd) · QUERY 0xA6→RESP 0xA7\nEVENT 0x8E (telemetry) · BULK_ATTACH 0x57"]
        topo --> fwd
    end
    emit -.local async.-> topo
    topo -->|"local: capture mode"| RSP
    topo -->|"remote: guid≠''"| exp

    subgraph EXP["⚠️ Expander over USB-CDC"]
        usbhost["ExpanderService (USB-OTG host)\nGUID slots · per-slot BusClient\nonReady → BULK_ATTACH the role set"]
        exprsp["Expander RoleServicePolicy\n(same lib — drives its own ports)"]
        usbhost --> exprsp
    end
    topo -. CDC .-> usbhost
    exp[" "]:::hidden --- EXP

    go["Go client.RoleTarget (c.Role(guid))\nONE role-I/O path: local vs forward, same roles.CmdXxx"]
    go -. USB .-> bs
    classDef hidden fill:transparent,stroke:transparent;
```

Every role exposes itself by adding its **codec + one line in `roleKindFor<>()` +
a `RoleTarget` wrapper** — never a per-role `switch` in the transport, forward,
query, event, or Go dispatch.

---

## 4. Effects → ports flow

Hub effect services never call `RoleServicePolicy` directly and never branch on
hub-vs-expander. Each builds a role-command payload and calls
`_topo->sendRoleCommand(PortRef, innerType, payload)` — the single transparent
router. Inputs flow back the other way: an RC input role's frame → emitter →
topology fan-out → `InputDispatcher` → `TriggerInput` gate → the effect.

Files: [effects/](../controllers/hubfx/esp32s3/src/effects/) (`gunfx`, `enginefx`, `gearcontrol`, `lightfx`, `landing_lights`, `input/`) · [role_command.h](../controllers/hubfx/esp32s3/src/effects/role_command.h) · [apply_hubfx_config.h](../controllers/hubfx/esp32s3/src/config/apply_hubfx_config.h).

```mermaid
flowchart TB
    subgraph CFG["Config apply (/hubfx.yaml)"]
        named["inputs: named channels → findInputByName()\n→ resolved PortRef + channelIdx (Rule 43)"]
        ports["ports: → portRefOf() (hub='' / expander alias→GUID)\nroles attached: local at boot, expander via BULK_ATTACH"]
    end

    subgraph FX["Hub effect services (BoardOf pack)"]
        gun["GunFxService"]
        eng["EngineFxService"]
        gear["GearControlService"]
        land["LandingLightService"]
        light["LightFxEffectService"]
    end
    named -.configure.-> FX
    ports -.claim PortRef.-> FX

    FX -->|"claim(PortRef, EffectId, RoleKind)\nsendRoleCommand(PortRef, RolePacket::X, payload)\ne.g. LED_SET_BRIGHTNESS · SERVO_RECOIL · MOTOR_SET_DUTY"| router

    router{"TopologyService.sendRoleCommand\nPortRef.guid == '' ?"}
    router -->|"yes (local)"| localrsp["RoleServicePolicy.handle()\n(capture mode — ACK never hits wire)"]
    router -->|"no (expander)"| fwd["forwardToExpander → BusClient → ⚠️ CDC\n→ expander RoleServicePolicy.handle()"]
    localrsp --> drv["port driver (servo/LED/motor/heater)"]
    fwd --> drv2["expander port driver"]

    gun -.shot audio.-> mixer["AudioMixer.playAsync (local)"]

    subgraph IN["Input path (RC → effect)"]
        rcrole["RC input role (RcPwm/Sbus/JetiEx)\non InputPort"]
        emit2["RoleEventEmitter → local-async listener"]
        disp["InputDispatcher.onRoleEvent\nrouting gate (Rule 25) + sourceMatches"]
        trig["TriggerInput: µs → typed value\n(hysteresis/deadband/failsafe)\nfires effect callback on CHANGE"]
        rcrole --> emit2 --> topo2["TopologyService fan-out"] --> disp --> trig
    end
    trig -.gated trigger.-> FX
```

**Single-router invariant:** effects (C++) and Studio/CLI (Go `RoleTarget`) each
have exactly one role-I/O path. The same effect code drives a hub-local servo
and an expander-hosted servo — only `PortRef.guid` differs.

---

## Maintaining these diagrams

These are **docs-as-code** (Rule 0). When a decomposition or data-flow changes,
update the matching diagram in the same commit. Each subsystem's deep doc links
here: storage → [27-WIRE-ASYNC-AND-UPLOAD.md](27-WIRE-ASYNC-AND-UPLOAD.md) ·
audio → [sfx_audio/README](../controllers/lib/sfx_audio/README.md) ·
ports/roles/topology → [16-EXPANDER-BOARD-DESIGN.md](16-EXPANDER-BOARD-DESIGN.md) + [sfx_board/README](../controllers/lib/sfx_board/README.md) ·
effects → [21-STUDIO-ENGINEFX-PANEL.md](21-STUDIO-ENGINEFX-PANEL.md).
