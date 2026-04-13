# HubFX ESP32-S3

Central hub controller for the ScaleFX system, targeting the **ESP32-S3** (dual Xtensa LX7 @ 240 MHz).

Migrating from [HubFX Pico](../pico/) (RP2350). The Pico version is being superseded.

## Architecture

### Dual-Core Design

| Core | Responsibilities |
|------|------------------|
| **Core 0** | Main loop — serial protocol, storage operations, slave management, effects state machines |
| **Core 1** | Audio consumer task (I2S DMA output, priority MAX-1), Audio producer task (WAV decode + SD reads + mixing, priority MAX-2), Storage writer task (ring→SD drain, priority MAX-2, on-demand during stream uploads), USB Host polling |

Core 1 runs two persistent FreeRTOS tasks pinned via `xTaskCreatePinnedToCore()`. The consumer blocks on `i2s_channel_write()` when DMA is full, yielding CPU to the lower-priority producer task. This gives the producer ~10ms of uncontested CPU time per DMA batch to decode WAV files and mix audio, while freeing Core 0 entirely for protocol handling.

All playback commands from Core 0 go through the async command queue (`playAsync()`, `stopAsync()`, etc.) — the producer task drains and executes them. SD file I/O in the mixer is mutex-protected for cross-core safety with storage operations.

During **stream file uploads**, a third FreeRTOS task (storage writer) is created on Core 1 to drain the PSRAM ring buffer to SD card. To prevent priority starvation, both audio tasks (consumer + producer) are **suspended** for the duration of the upload via `onStreamStart`/`onStreamEnd` lifecycle callbacks. The writer task runs at priority MAX-2 (23) and self-deletes when the upload completes. See [Stream Upload Architecture](#stream-upload-architecture) for details.

### Cross-Core Communication

All cross-core variables use `std::atomic<T>` with explicit memory ordering (same pattern as HubFX Pico — see Rule 15).

### Platform Differences from Pico

| Feature | HubFX Pico (RP2350) | HubFX ESP32-S3 |
|---------|---------------------|----------------|
| CPU | Dual Cortex-M33, 120 MHz | Dual Xtensa LX7, 240 MHz |
| RAM | 520 KB SRAM | 512 KB SRAM + optional PSRAM |
| Flash | External SPI (16 MB) | External SPI (16 MB) |
| I2S | PIO-based (software) | Hardware I2S peripheral |
| USB Host | PIO-USB (software) | Native USB-OTG |
| RTOS | Bare-metal (cooperative) | FreeRTOS (preemptive) |
| Delays | `busy_wait_ms()` / `busy_wait_us_32()` | `vTaskDelay()` / `esp_rom_delay_us()` |
| Mutex | `mutex_t` (Pico SDK) | `xSemaphoreCreateMutex()` |
| Bootloader | BOOTSEL (UF2 drag-drop) | esptool / USB-DFU / OTA |

---

## Migration Checklist

> Track progress as each item is implemented and verified.
> Pico source reference: `controllers/hubfx/pico/src/`

### Phase 0 — Foundation (skeleton exists)

- [x] Project structure (`platformio.ini`, `src/hubfx_esp32s3.ino`)
- [x] `SfxServer` integration (`server.begin()`, connection timeout disabled)
- [x] Core 1 FreeRTOS task scaffold (`xTaskCreatePinnedToCore`)
- [x] Cross-core atomics (`audioInitialized`, `core1Ready`, `loop1Count`)
- [x] DiagLog over UART (SFX_LOG_* macros, DIAG_HISTORY command)
- [x] Core protocol (INIT, SHUTDOWN, REBOOT, STATUS, KEEPALIVE, I2C_SCAN)
- [x] STATUS callback (flags, slave mask placeholder, loop1Count)
- [x] Periodic diagnostic logging (uptime, heap, core 1 stats)
- [x] Pin definitions — I2S (GPIO 41/42/14), I2C (GPIO 8/9), SD (GPIO 38/39/40)
- [ ] Verify shared lib builds (split libraries compile for ESP32-S3)

### Phase 1 — Local Module Files (copy + adapt from Pico)

These files live in the controller's `src/` directory (not shared lib).
Most can be copied verbatim; platform-specific items noted.

| Status | File | Pico Source | Notes |
|--------|------|-------------|-------|
| [ ] | `hubfx_log.h` | `hubfx_log.h` | Copy as-is (DiagLog macros, no platform deps) |
| [x] | `hubfx_audio.h` | `audio/audio_channels.h` | Rewritten — `Mixer` type alias + `HubFxChannel` namespace (SYSTEM=0, ENGINE_A=1, ENGINE_B=2, GUN=3) |
| [ ] | `audio/system_sounds.h` | `audio/system_sounds.h` | Copy as-is (file paths, playback config) |
| [ ] | `board_manager/hubfx_protocol.h` | `board_manager/hubfx_protocol.h` | Copy as-is (just a redirect to shared lib `serial/hubfx/hubfx.h`) |
| [ ] | `board_manager/slave_registry.h` | `board_manager/slave_registry.h` | Copy as-is (singleton, no platform deps) |
| [ ] | `board_manager/slave_server.h/.cpp` | `board_manager/slave_server.h/.cpp` | Copy; verify `UsbHost` API compatibility (see Phase 3) |
| [x] | `config/engine_config.h` | `effects/effects_config.h` | Rewritten — `EngineConfig` struct + declarative YAML schema DSL via `sfx_config`. Includes `audio_output` field (stereo/left/right) |
| [x] | `config/gunfx_hub_config.h` | N/A (new) | Hub-local gun FX audio config — `audio_output` field for gun sound channel routing |
| [x] | `effects/engine_fx.h/.cpp` | `effects/engine_fx.h/.cpp` | Rewritten — singleton, uses `HubFxChannel` enum, `mixer()` accessor to `Mixer::instance()`, config-driven audio output routing |
| [x] | `effects/engine_server.h/.cpp` | `effects/engine_server.h/.cpp` | Rewritten — `EngineServer` BusServer, singleton accessor to `EngineFX::instance()` |
| [ ] | `effects/gun_fx.h/.cpp` | `effects/gun_fx.h/.cpp` | Review for platform calls |
| [ ] | `storage/storage_config.h` | `storage/storage_config.h` | Copy as-is (constants) |
| [ ] | `storage/config_reader.h/.cpp` | `storage/config_reader.h/.cpp` | **Needs work** — uses `SdFat File32` + `LittleFS`. See Phase 4 |

### Phase 2 — Audio System

The shared `AudioMixer` already has ESP32-S3 conditionals (`SFX_PLATFORM_ESP32` guards in `audio_config.h`, `audio_mixer.h` for buffer sizes). The main work is I2S output.

| Status | Item | Details |
|--------|------|---------|
| [x] | **I2S driver** | ESP-IDF v5.x standard-mode via `driver/i2s_std.h`. `EspI2SOutput` singleton wraps `i2s_channel_write()` with DMA auto-clear on underrun. Bit depth derived from `AUDIO_BIT_DEPTH` config. |
| [x] | **Codec init** | TAS5825Codec singleton — `begin(Wire, 8, 9, AUDIO_SAMPLE_RATE, TAS5825M_12V)`. Supply voltage configurable. All codec defaults now use `AUDIO_SAMPLE_RATE`. |
| [x] | **Audio mixer begin** | `mixer.begin(data, bclk, lrclk)` — Phase 1 on Core 0, Phase 2 (`beginI2S()`) on Core 1. PSRAM buffers: 24000-frame WAV decode per channel, 4096-frame SPSC ring buffer. |
| [x] | **Audio producer** | Dedicated FreeRTOS task on Core 1 (priority MAX-2) via `mixer.startProducerTask()`. Runs `produce(RING_FRAMES)` in a tight loop, yielding 2ms when ring is full or no channels playing. SD file I/O mutex-protected for cross-core safety. |
| [x] | **Audio consumer** | FreeRTOS task on Core 1 (priority MAX-1) — reads ring buffer, batch-writes to I2S via 512-frame internal SRAM buffer. Blocks on DMA full, yielding CPU to producer task. |
| [ ] | **MCLK support** | ESP32-S3 has native MCLK output (Pico does not). Some DACs need it. Add optional MCLK pin config. |

### Phase 3 — USB Host (Slave Management)

This is the most significant platform difference. Pico uses PIO-USB (software USB via PIO state machine). ESP32-S3 has native USB-OTG hardware.

| Status | Item | Details |
|--------|------|---------|
| [x] | **USB Host driver** | `EspUsbHost` (HW USB-OTG, ESP-IDF USB Host Library + CDC-ACM class driver) — implemented in `sfx_usb`. Fixed GPIO19 (D-) / GPIO20 (D+). Event-driven via FreeRTOS tasks. |
| [x] | **CDC-ACM component** | Vendored `espressif/usb_host_cdc_acm` v2.3.0 from `esp-usb` repo as PlatformIO library in `controllers/lib/esp_cdc_acm/`. PlatformIO Arduino does NOT process `idf_component.yml` — vendoring was required. |
| [x] | **CDC device enumeration** | `UsbHost::cdcDeviceCount()`, `getCdcDevice()` — base class shared implementation, works with `EspUsbHost`. |
| [x] | **Mount/unmount callbacks** | `onMount()`, `onUnmount()` callbacks registered in firmware. Logged via DiagLog. |
| [x] | **USB Host init in firmware** | `UsbHost::instance().begin()` + `init()` called from `setup()`. STATUS flag bit 3 reports USB host ready state. **Verified on hardware** — Build 15 shows bit 3 = TRUE in STATUS response. |
| [x] | **USB bus recovery** | `resetBus()` power-cycles root port via `usb_host_lib_set_root_port_power()`. Auto-recovery timer (5s after disconnect, 10s cooldown). CLI: `hub.usb.reset`, packet: `USB_RESET_BUS (0xAD)`. |
| [ ] | **SerialBus adaptation** | `SerialBus` in `lib/sfx_serial/serial/client/bus.h` depends on `UsbHost` class. Need to verify ESP32 `cdcRead/cdcWrite` works with COBS framing. |
| [ ] | **Slave discovery** | `tryInitSlave()`, `scanAndInitSlaves()` — logic is portable, but depends on `UsbHost` / `BusClient` API. |
| [ ] | **Slave clients** | `GunFxClient`, `LightFxClient`, `GearControlClient` — shared code extending `BusClient` → depends on `SerialBus` → depends on `UsbHost`. |
| [ ] | **Keepalive / polling** | Slave poll interval (100ms), discovery scan (5s) — logic is portable. |

### Phase 4 — Storage

| Status | Item | Details |
|--------|------|---------|
| [x] | **SD card library** | ESP32 Arduino `SD_MMC.h` via `EspSdio1BitPolicy` template in `sfx_storage`. SDMMC 1-bit SDIO at `SDMMC_FREQ_HIGHSPEED` (40 MHz). `SdCardModule` singleton API preserved — `AudioMixer` and `StorageServer` use it transparently. |
| [x] | **SD init with fallback** | `SdCardModule::instance().begin()` → `EspSdio1BitPolicy::mount()` with configurable pins. Boot log reports "SDIO 1-bit HS". |
| [ ] | **LittleFS flash** | Both platforms support LittleFS via Arduino framework. `FlashModule` singleton should work. Verify ESP32 partition table includes a LittleFS partition. |
| [x] | **ConfigReader** | Replaced by `sfx_config` library — `ConfigStore<HubFxConfigSchema>` with declarative YAML schema DSL. No longer uses `SdFat File32`. |
| [x] | **File operations** | `StorageServerT<Esp32StoragePolicy>` handles all file protocol commands (list, tree, upload, download, delete, mkdir, info). Stream upload mode with PSRAM ring buffer + Core 1 writer task. Windowed upload mode with server-controlled flow. See [Stream Upload Architecture](#stream-upload-architecture). |

### Phase 5 — Main Firmware Integration

| Status | Item | Details |
|--------|------|---------|
| [ ] | **Codec selection** | ~~`#define USE_WAVESHARE_PICOAUDIO`~~ Done — using `TAS5825Codec` singleton via `AudioMixer<EspI2SOutput, TAS5825Codec>`. |
| [ ] | **SD card init** | `initSdCard()` with fallback speed pattern. |
| [x] | **Config loading** | `ConfigServerT<HubFxConfigStore>` handles CONFIG_RELOAD (0x90), CONFIG_STATUS (0x91), CONFIG_SAVE (0xAC). Loads from LittleFS flash via `FlashModule`, fires `onLoaded()` callback. Schema-validated YAML with defaults. |
| [ ] | **Audio init** | `initAudio()` — codec begin + mixer begin. |
| [ ] | **USB Host init** | `usbHost.begin()` + mount/unmount callbacks. |
| [ ] | **Slave pre-registration** | `slaveRegistry.registerSlave()` for GunFX, LightFX, GearControl. |
| [ ] | **Log message relay** | `*Client.onLogMessage()` → `DiagLog::instance().ingest()`. |
| [ ] | **Domain handlers** | Wire up `SlaveServer`, `HubFxAudioServer`, `EngineServer`, `HubFxStorageServer` — set dependencies, register with `server.addModuleHandler()`. |
| [ ] | **STATUS callback** | `server.core().onStatusData()` — 14-byte hub status (slave mask, SD, audio, PC serial, loop1Count, ring stats). **6-byte skeleton implemented** (flags + slave mask + loop1Count). |
| [ ] | **PC serial detection** | Pico uses `(bool)Serial` → `tud_cdc_connected()`. ESP32-S3 USB CDC: check `Serial` / `USBSerial` operator bool or DTR state (`Serial.dtr()`). Behavior may differ. |
| [ ] | **Main loop** | Serial processing (conditional on PC connected), slave polling, engine FX, audio producer, 5-second diagnostic logging. |
| [ ] | **Init sound** | Optional startup chime on system sounds channel. |

### Phase 6 — Platform-Specific Details

| Status | Item | Details |
|--------|------|---------|
| [ ] | **Free heap** | Pico: `rp2040.getFreeHeap()`. ESP32: `SFX_FREE_HEAP()` → `esp_get_free_heap_size()`. Already in `sfx_platform.h`. |
| [x] | **PSRAM** | N16R8 has 8MB OPI PSRAM. Configured via `board_build.arduino.memory_type = qio_opi`. Heap reports ~8.3MB with PSRAM enabled. Consider using for audio buffers (`WAV_BUF_FRAMES`, ring buffer). |
| [ ] | **Watchdog** | ESP32 FreeRTOS has task watchdog (`esp_task_wdt`). Core 1 audio task must feed it or be excluded. |
| [x] | **Partition table** | Custom `partitions.csv` with app (3.9MB), LittleFS (512KB), SPIFFS (3.4MB), coredump (64KB). All within 8MB bootloader limit. See Partition Table section. |
| [ ] | **USB VID/PID** | Pico uses `0x2e8a:0x0181`. ESP32-S3 needs its own USB descriptor config (via `build_flags` or `sdkconfig`). |
| [ ] | **OTA updates** | ESP32-S3 supports OTA firmware updates (no BOOTSEL equivalent). Consider implementing `sfxRebootToBootloader()` as OTA trigger or USB-DFU. |

---

## Shared Library Compatibility Notes

These components from `controllers/lib/` already have ESP32-S3 support via `sfx_platform.h`:

| Component | ESP32 Status | Notes |
|-----------|-------------|-------|
| `platform/sfx_platform.h` | ✅ Ready | Mutex, delay, heap, reboot, GPIO, servo, interrupt macros |
| `serial/*` (protocol) | ✅ Ready | Pure C++ protocol handling, no platform deps |
| `server/sfx_server` | ✅ Ready | Uses `sfx_platform.h` for delays, heap, reboot |
| `audio/audio_config.h` | ✅ Ready | Has `ARDUINO_ARCH_ESP32` conditionals for buffer sizes |
| `audio/audio_mixer.h` | ⚠️ Partial | ESP32 buffer sizes defined. **I2S output path needs ESP32 driver** (biggest gap). `SdCardModule` API must match. |
| `audio/audio_ring_buffer.h` | ✅ Ready | Lock-free SPSC, uses `std::atomic` (portable) |
| `audio/tas5825_codec` | ✅ Ready | I2C-based, uses standard Wire API |
| `audio/simple_i2s_codec` | ✅ Ready | No hardware deps (config-only codec) |
| `led/led_control` | ✅ Ready | Uses `digitalWrite()` (safe on ESP32) |
| `power/i2c_device` | ✅ Ready | Uses standard Wire API |
| `storage/sd_card` | ⚠️ Needs work | Uses SdFat (Adafruit Fork) `SdFat32` / `File32`. May not compile on ESP32 without library port. |
| `storage/flash` | ⚠️ Verify | Uses LittleFS — should work on ESP32 Arduino, verify partition setup. |
| `serial/client/bus.h` | ✅ Ready | `SerialBus` depends on `UsbHost` abstract class — `EspUsbHost` backend implemented in `sfx_usb`. Include path: `usb/sfx_usb_host.h` (renamed from `usb_host.h` to avoid ESP-IDF header collision). |
| `usb/sfx_usb_host.*` | ✅ Ready | Abstract `UsbHost` base class. Renamed from `usb_host.*` to avoid collision with ESP-IDF's `usb/usb_host.h` (C API). |
| `usb/esp_usb_host.*` | ✅ Ready | ESP32-S3 USB-OTG backend using vendored `esp_cdc_acm` library. Feature-gated: `ESP_USB_HAS_CDC_ACM=1` when `usb/cdc_acm_host.h` is available. |

---

## Stream Upload Architecture

Stream mode (mode=3) sends raw binary data in **512 KB segments** without COBS framing, maximizing throughput for large file uploads. The firmware uses a multi-stage pipeline to decouple USB/UART reception from SD card writes.

### Data Flow Pipeline

```
Core 0 (protocol):
  Serial RX → processStream() → fill buffer (64 KB) → SPSC ring buffer (2 MB, PSRAM)
                                                              ↓
Core 1 (writer task):
  Ring buffer → staging (256 KB, PSRAM) → SD card (SDMMC 1-bit HS, 40 MHz)
```

### Buffer Architecture

| Buffer | Size | Memory | Purpose |
|--------|------|--------|---------|
| UART RX | 128 KB | Internal SRAM | `Serial.setRxBufferSize(131072)` — absorbs UART burst |
| Fill buffer | 64 KB | PSRAM | Batch reads from serial into ring buffer |
| SPSC ring | 2 MB | PSRAM | Lock-free producer-consumer decoupling Core 0 ↔ Core 1 |
| Staging | 256 KB | PSRAM | Writer task drains ring in 256 KB batches for SD write |

**Total PSRAM usage:** ~2.4 MB during stream uploads (ring + staging + fill buffer).

### Writer Task Lifecycle

The storage writer task is **created on-demand** for each stream upload and self-deletes on completion:

1. `handleUploadBegin()` → `startWriterTask()` → `xTaskCreatePinnedToCore(writerTaskFunc, Core 1, prio 23)`
2. Writer runs in a tight loop: drain ring → write staging → SD card
3. On upload end/cancel: `_drainRequested = true` → writer flushes remaining data → signals `_writerDone` → self-deletes

Writer stats are exposed as atomics for Core 0 to read: `bytesWritten`, `writeCount`, `maxWriteLatency_ms`, `totalStallTime_ms`.

### Audio Suspend During Uploads

Both audio tasks on Core 1 (consumer at prio MAX-1, producer at prio MAX-2) are **suspended** during stream uploads to prevent priority starvation of the writer task:

```cpp
storageServer.onStreamStart([]() {
    mixer.stopAll(Immediate);
    mixer.stopProducerTask();
    vTaskSuspend(core1TaskHandle);   // Suspend audio consumer
});

storageServer.onStreamEnd([]() {
    vTaskResume(core1TaskHandle);    // Resume audio consumer
    mixer.startProducerTask(1, configMAX_PRIORITIES - 2, 8192);
});
```

The `onStreamStart`/`onStreamEnd` callbacks are guarded by `_streamSuspended` to ensure exactly-once semantics (one `onStreamEnd` per `onStreamStart`).

### Client-Side Flow Control

Both CLIs parse the `ring_fill_pct` byte from segment ACK payloads and throttle sending when the ring buffer exceeds 50% full:

**Segment ACK payload:** `[seg_idx:u16LE][bytes_received:u32LE][ring_fill_pct:u8]` (7 bytes)

| Ring Fill | Action | Delay |
|-----------|--------|-------|
| ≤ 50% | No throttle | 0 ms |
| 51-75% | Proportional delay | 60-1500 ms |
| 76-100% | Heavy throttle | 1560-3000 ms |

Formula: `delay_ms = (ring_fill_pct - 50) * 60`

This prevents the ring buffer from filling to 100% even when SD write latency spikes occur.

### SD Card Performance

The SD card is mounted via SDMMC 1-bit SDIO at `SDMMC_FREQ_HIGHSPEED` (40 MHz bus clock). This doubles throughput compared to the default 20 MHz clock, achieving sustained write speeds of ~300-500 KB/s depending on card quality. Combined with client-side flow control, this ensures the writer task can keep up with incoming data.

---

## Hardware

### Current Development Board

**diymore ESP32-S3 DevKitC-1 N16R8**

| Spec | Value |
|------|-------|
| MCU | ESP32-S3 (QFN56, revision v0.2) |
| Flash | 16 MB QIO (3.3V) |
| PSRAM | 8 MB OPI (AP_3v3, Octal SPI) |
| Crystal | 40 MHz |
| CPU | Dual Xtensa LX7 @ 240 MHz |
| MAC | ac:a7:04:13:44:98 |
| USB-UART bridge | CH340 or CP2102N (UART0) |
| USB-OTG | Native ESP32-S3 USB peripheral |

- **DAC**: TAS5825M (stereo Class-D amplifier with I2C control via I2S)
- **Storage**: MicroSD card (SPI interface)
- **USB Host**: Native USB-OTG (ESP32-S3 has built-in USB host support)

### N16R8 Configuration Requirements

The N16R8 module has **Octal (OPI) PSRAM**, not Quad. The PlatformIO `esp32-s3-devkitc-1` board definition defaults to 8MB flash and no PSRAM, so the following overrides are required in `platformio.ini`:

```ini
; 16MB flash + OPI PSRAM (N16R8)
board_build.flash_size = 16MB
board_build.arduino.memory_type = qio_opi  ; QIO flash + OPI PSRAM
```

The `memory_type` selects the correct Arduino ESP32 SDK variant. Available variants:
`dio_opi`, `dio_qspi`, `opi_opi`, `opi_qspi`, `qio_opi`, `qio_qspi`

With `qio_opi` and PSRAM enabled, `esp_get_free_heap_size()` reports ~8.3 MB (includes PSRAM).

---

## Partition Table

### 8 MB Bootloader Constraint (CRITICAL)

> **The `esp32-s3-devkitc-1` board definition's prebuilt bootloader only supports partition tables within the first 8 MB of flash (0x000000–0x7FFFFF).** Using a partition table with entries beyond 8 MB (e.g., `default_16MB.csv`) causes an infinite boot loop crash.

This was discovered when the firmware entered a crash loop showing:
```
rst:0x3 (RTC_SW_SYS_RST)
Saved PC:0x403cdb0a
```

The crash address decoded to `bootloader_reset()` in `bootloader_utility.c:846` — the bootloader could not validate or load the application image because it cannot address partitions beyond 8 MB.

**Root cause isolation:** A minimal test firmware (just `Serial.begin()` + `Serial.println()`) also crashed, proving the issue was in `platformio.ini` configuration, not application code. Incremental testing revealed:

| Configuration | Result |
|---------------|--------|
| All defaults (no overrides) | Boots |
| `memory_type=qio_opi` only | Boots |
| `flash_size=16MB` + `qio_opi` (no partition override) | Boots |
| `flash_size=16MB` + `default_16MB.csv` + `qio_opi` | **Crash loop** |
| `flash_size=16MB` + custom `partitions.csv` (beyond 8MB) | **Crash loop** |
| `flash_size=16MB` + `default_8MB.csv` + `qio_opi` | Boots |
| `flash_size=16MB` + custom `partitions.csv` (within 8MB) + `qio_opi` | **Boots** |

**Fix:** Use a custom `partitions.csv` where all entries end before `0x800000`. The remaining 8 MB of flash is physically present but not addressable by the partition table with this bootloader.

### Current Partition Layout

See [partitions.csv](partitions.csv) for the actual file.

| Partition | Type | Offset | Size | Purpose |
|-----------|------|--------|------|---------|
| nvs | NVS | 0x009000 | 20 KB | WiFi credentials, preferences |
| otadata | OTA | 0x00E000 | 8 KB | Boot selection |
| app0 | App (OTA_0) | 0x010000 | 3.9 MB | Firmware image |
| littlefs | Data (SPIFFS) | 0x400000 | 512 KB | Config files (YAML, JSON) |
| spiffs | Data (SPIFFS) | 0x480000 | 3.4 MB | WAV files, assets |
| coredump | Coredump | 0x7F0000 | 64 KB | Crash diagnostics |

Total used: ~7.9 MB of 16 MB. All partitions end before `0x800000`.

### Partition Modification Rules

1. **All partitions MUST end before `0x800000`** (8 MB boundary)
2. After modifying `partitions.csv`, erase flash before uploading: `esptool.py --port COMx erase_flash`
3. Verify boot after any partition change — a bad table causes silent crash loops
4. To use the full 16 MB, a custom bootloader build would be needed (not currently implemented)

---

## Troubleshooting

### Boot Loop / Crash Loop

**Symptom:** ESP32-S3 repeatedly resets with `rst:0x3 (RTC_SW_SYS_RST)` at 115200 baud ROM output. No application output visible (setup() never runs).

**Diagnosis steps:**
1. Read ROM boot output at 115200 baud (not 1Mbps — ROM uses 115200)
2. Check for `rst:0x3` — this means software/watchdog reset, not power-on
3. Look for `Saved PC:0x403cdb0a` — this is `bootloader_reset()`, meaning the bootloader itself is failing
4. If crash happens before `setup()`, it's a `platformio.ini` or partition table issue, not a code bug

**Common causes:**
- Partition table entries extending beyond 8 MB (see section above)
- PSRAM mode mismatch (`quad` vs `opi`) — may not crash but PSRAM won't initialize
- Flash mode/size mismatch with actual hardware
- Using `opi_opi` SDK variant on a board with QIO flash (see below)

**Recovery:**
```bash
# Full erase + reflash (fixes partition table issues)
esptool.py --port COM15 erase_flash
python -m platformio run -e esp32s3 -t upload
```

### Flash Mode / Memory Type Crash Loop (N16R8 Board)

**Symptom:** Device enters crash loop immediately after flash. No response to IDENTIFY. Serial reads all zeros at 1Mbps can sometimes be observed.

**Root cause:** Incorrect `flash_mode` + `memory_type` combination in `platformio.ini`. The "N16R8" designation is ambiguous — it means 16MB flash + 8MB PSRAM, but does NOT specify the flash *interface*. Different N16R8 boards may use QIO or OPI flash.

**What was tested on the diymore N16R8 (ESP32-S3-WROOM-1 N16R8):**

| `flash_mode` | `memory_type` | Result |
|--------------|---------------|--------|
| `qio` | `qio_opi` | **WORKS** — QIO flash + OPI PSRAM, ~8.3 MB free RAM |
| `opi` | `opi_opi` | **CRASH** — esptool doesn't recognize `opi` as a valid flash mode |
| `dout` | `opi_opi` | **CRASH** — OPI flash SDK variant, but board has QIO flash |
| `qio` | (none) | Boots but no PSRAM (~300 KB free RAM only) |

**Correct settings for this board:**
```ini
board_build.flash_mode = qio
board_build.flash_size = 16MB
board_build.f_flash = 80000000L
board_build.arduino.memory_type = qio_opi
```

**How to determine your board's flash type:**
- esptool reports `Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)` — this confirms OPI PSRAM but says nothing about flash interface
- The SDK variant name format is `<flash>_<psram>`: `qio_opi` = QIO flash + OPI PSRAM
- If the board works with `qio_opi` but crashes with `opi_opi`, the flash is QIO (Quad SPI), not Octal
- Some online guides recommend "OPI flash" for all N16R8 boards — this is incorrect for boards with QIO flash chips

**Key insight:** The `memory_type` selects the SDK variant (pre-compiled libraries with specific flash and PSRAM drivers). The `flash_mode` tells esptool how to write the bootloader's flash control bits. Both must match the actual hardware. There is no auto-detection.

### No Serial Output After Boot

The firmware uses 6Mbps baud (`Serial.begin(6000000)`) with binary COBS protocol. There is no human-readable output at 6Mbps — use the CLI:
```bash
python -m tests.cli.interactive --port COM15
```

ROM boot messages are at 115200 baud and only appear during reset.

### PSRAM Shows 0 Bytes

If `esp_get_free_heap_size()` returns ~300–370 KB instead of ~8 MB:
- Verify `board_build.arduino.memory_type = qio_opi` is set
- The sdkconfig may need `CONFIG_SPIRAM_BOOT_INIT=y` (enabled by the qio_opi SDK variant)
- Full erase + reflash may be needed after changing memory_type

## Pin Assignments

| Pin | GPIO | Function | Connected To |
|-----|------|----------|-------------|
| I2S DOUT | 1 | I2S serial data | TAS5825M SDIN |
| I2S BCLK | 4 | I2S bit clock | TAS5825M SCK |
| I2S LRCLK | 3 | I2S word select | TAS5825M FSYNC |
| I2C SDA | 8 | I2C data | TAS5825M SDA |
| I2C SCL | 9 | I2C clock | TAS5825M SCL |
| SD CMD | 38 | SD_MMC command | MicroSD CMD |
| SD CLK | 39 | SD_MMC clock | MicroSD CLK |
| SD D0 | 40 | SD_MMC data 0 | MicroSD DAT0 |
| PPM IN | 5 | RC receiver PPM input | IN_1 header |
| LED (conn) | 48 | Onboard RGB LED | (built-in) |
| USB D- | 19 | USB Host (fixed) | USB hub/device |
| USB D+ | 20 | USB Host (fixed) | USB hub/device |

### RC Receiver Input (PPM)

HubFX reads a composite PPM signal from the RC receiver on **GPIO5** (IN_1 header). This provides up to 12 RC channels on a single wire, used for program switching, throttle input (engine sound), and other real-time control.

**Signal:** Standard composite PPM — each channel encoded as the time between consecutive falling edges (1000–2000µs typical), with a sync gap (>3ms) between frames. 50Hz frame rate typical.

**Y-harness supported:** PPM is unidirectional (receiver drives, devices listen). A passive Y-harness can split the signal to HubFX and another device (e.g., flight controller) — both inputs are high-impedance.

#### RC Receiver Signal Voltage Compatibility

| Manufacturer | Signal Voltage | Direct to ESP32-S3 | Notes |
|-------------|---------------|--------------------|---------|
| **Jeti** (REX, RSat, R-series) | 3.3V | Yes — no level shifter needed | Internal 3.3V logic regardless of supply voltage |
| **Futaba** (S.Bus / PPM) | 3.3V | Yes — no level shifter needed | 3.3V signaling, supply can be 3.8–8.4V |
| **Spektrum** (DSMX / PPM) | 3.3V | Yes — no level shifter needed | Satellite and standard receivers, 3.3V logic |
| **FrSky** (X/R/TD series) | 3.3V | Yes — no level shifter needed | May use inverted PPM polarity (set `PPM_INVERT = true`) |
| **TBS Crossfire / ELRS** | 3.3V | Yes — no level shifter needed | CRSF is serial not PPM; PPM output is 3.3V |
| **Older FM/72MHz receivers** | 5V | **No** — needs voltage divider or level shifter | Rare in modern setups |

> **All modern RC receivers (post-2010) use 3.3V logic internally** regardless of the servo power rail voltage (4.8–8.4V). The ESP32-S3 GPIO is 3.3V native — direct connection is safe for all current production receivers. When in doubt, verify with a multimeter or oscilloscope before connecting.

### TAS5825M Codec Wiring

The TAS5825M is a stereo closed-loop Class-D audio amplifier with I2C control.
It receives audio data over I2S and is configured/controlled via I2C.

```
  ESP32-S3 DevKitC-1                TAS5825M Breakout
  ═══════════════════                ═════════════════
  GPIO  1 (I2S DOUT) ──────────── SDIN   (serial audio data)
  GPIO  4 (I2S BCLK) ──────────── SCK    (bit clock)
  GPIO  3 (I2S LRCLK) ─────────── FSYNC  (frame sync / word select)
  GPIO  8 (I2C SDA)  ──────────── SDA    (I2C data)
  GPIO  9 (I2C SCL)  ──────────── SCL    (I2C clock)
  3.3V ────────────────────────── DVDD   (digital supply, 3.3V)
  3.3V ─────── [4.7kΩ] ─────────── SDA    (I2C pull-up)
  3.3V ─────── [4.7kΩ] ─────────── SCL    (I2C pull-up)
  GND ─────────────────────────── GND    (common ground)
                                   PVDD ← External 12-24V supply
                                   PDN  ← 3.3V (or GPIO for power control)
                                   ADDR ← GND (I2C address 0x4C)
```

**Power supply notes:**
- **DVDD** (digital): 3.3V from ESP32-S3 DevKitC-1 3V3 pin (max ~500mA available)
- **PVDD** (amplifier): External supply 4.5V-26.4V. Output power depends on PVDD:
  - 12V (3S LiPo ~11.1V): ~15W per channel into 8 ohm
  - 15V (4S LiPo ~14.8V): ~22W per channel into 8 ohm
  - 24V (bench supply): ~30W per channel into 8 ohm
- **ADDR pin**: Tied to GND = I2C address 0x4C (default in firmware)
- **PDN (Power Down)**: Tie to 3.3V for always-on, or connect to a GPIO for power management
- **I2C pull-ups**: 4.7kΩ to 3.3V on SDA and SCL (some breakout boards include these)

**Supply voltage in firmware:**
The supply voltage is configured in `hubfx_esp32s3.ino` to set the correct analog gain.
The codec uses a two-phase init: `begin()` probes I2C and enters Deep Sleep (safe before
I2S clocks), then `activate()` configures registers and transitions to PLAY after I2S starts:
```cpp
// Phase 1: before I2S clocks
TAS5825Codec::instance().begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL,
                               AUDIO_SAMPLE_RATE, TAS5825M_12V);
// ... start I2S ...
// Phase 2: after I2S BCLK/LRCLK running
TAS5825Codec::instance().activate();
```
Change `TAS5825M_12V` to match your PVDD: `TAS5825M_12V`, `TAS5825M_15V`, `TAS5825M_20V`, or `TAS5825M_24V`.

**Wire length:** Keep I2S wires short (< 6 inches / 150mm). At 48kHz/16-bit stereo, BCLK is ~1.5 MHz — manageable, but shorter is better for signal integrity.

## Building

```bash
# Build
cd controllers/hubfx/esp32s3
python -m platformio run -e esp32s3

# Upload via esptool (UART0 via USB-UART bridge)
python -m platformio run -e esp32s3 -t upload

# Build and flash (centralized script, with post-flash verification)
python scripts/build_and_flash.py hubfx
python scripts/build_and_flash.py hubfx --port COM5  # explicit port

# Interactive CLI (UART0 @ 1Mbps — COBS binary protocol)
python -m tests.cli.interactive --port COM5

# Serial monitor (raw — binary protocol, not human-readable)
python -m platformio device monitor -b 1000000
```

### Serial Architecture

The ESP32-S3 DevKitC-1 has **two USB connectors**:

| Connector | Role | Maps to |
|-----------|------|--------|
| USB-UART (CP2102N/CH340) | Flashing + CLI/debug protocol | `Serial` (UART0) |
| USB-OTG (native USB) | USB Host for slave controllers | Reserved for USB Host mode |

`cdc_on_boot=0` prevents the OTG port from acting as CDC serial, reserving it for USB Host.
All protocol communication (COBS/INIT/STATUS/DIAG) goes through UART0.

## Firmware Version

| Version | Build | Date | Changes |
|---------|-------|------|----------|
| 0.31.0 | 170 | 2026-04-08 | **HubFX v1 board bring-up — GPIO expander, LED architecture, audio diagnostics.** Added PCAL6416A I2C GPIO expander driver and abstract `GpioExpander` interface (`gpio_expander.h`, `native_gpio.h`). TAS5825M codec control pins (PDN, MUTE, nFAULT) now managed via PCAL6416A Port 1 instead of direct GPIO — enables full hardware mute/unmute, power-down control, and fault monitoring through I2C. Added AW9523B GPIO expander driver for alternate boards. **LED architecture overhaul:** Extracted LED subsystem from LightFX-specific code into shared `sfx_peripherals/led/` library — BAM (Bit Angle Modulation) LED driver (`bam_led_drv.h`), templatized `LedManager<N>` with `LedControl<TDriver>`, shared `LedServer`/`LedClient` protocol handlers. LightFX server/client reduced to thin wrappers. **TAS5825M 2-phase codec init:** Phase 1 (Core 0) probes I2C and enters Deep Sleep (PLL off, safe without I2S clocks); Phase 2 waits for Core 1 I2S init then configures registers and transitions to PLAY. **Comprehensive audio hardware diagnostics:** `diagnoseAudioHardware()` reads expander pin states + all TAS5825M fault/state registers (power state, automute, channel faults, global faults, over-temp, volume, sample rate monitor). **Periodic codec health check:** Retries TAS5825M init every 5s if boot probe failed (e.g. battery not connected); full diagnostic dump every 30s. **INA226 power monitoring:** 6 INA226 monitors at 0x40-0x45 with bus voltage in STATUS payload. **Enhanced STATUS:** 19-byte module data — `[flags:u8][slaveMask:u8][loop1Count:u32LE][i2cDeviceMask:u8][ina226_mV[0..5]:u16LE×6]`. I2C device presence bitmask tracks PCAL6416A + INA226 availability. |
| 0.27.0 | 143 | 2026-03-31 | **Stream upload SD throughput fix.** Changed SDMMC bus clock from `SDMMC_FREQ_DEFAULT` (20 MHz) to `SDMMC_FREQ_HIGHSPEED` (40 MHz) in `EspSdioSdPolicy`, doubling sustained SD write throughput. Boot log now shows "SDIO 1-bit HS" for verification. Added **client-side flow control** to both Go and Python CLIs — segment ACK now includes `ring_fill_pct` byte, CLIs throttle proportionally when ring >50% full (`delay_ms = (pct - 50) * 60`). Together these changes resolve the remaining stream upload stall for large files where SD write latency caused ring buffer overflow. |
| 0.27.0 | 142 | 2026-03-31 | **Audio suspend during stream uploads.** Added `onStreamStart()`/`onStreamEnd()` lifecycle callbacks to `StorageServerT` with `_streamSuspended` exactly-once guard. HubFX firmware suspends both audio tasks (consumer via `vTaskSuspend`, producer via `stopProducerTask()`) during stream uploads to free Core 1 for the SD writer task. Resumes both tasks on upload completion or cancellation. Fixes priority starvation where audio tasks (prio 24/23) prevented the writer task (prio 23) from running despite same-priority scheduling. |
| 0.27.0 | 141 | 2026-03-31 | **Writer task instrumentation and lifecycle refactor.** Replaced persistent writer task with on-demand per-upload lifecycle: `startWriterTask()` creates FreeRTOS task on Core 1, `stopWriterTask()` signals drain and waits for self-delete. `WriterStats` struct exposes performance counters (`bytesWritten`, `writeCount`, `maxWriteLatency_ms`, `totalStallTime_ms`) as atomics for cross-core reading. Writer logs startup, first data, heartbeat, and exit diagnostics. `allocateWriterBuffers()` separated from task creation — called once at boot to pre-allocate 2 MB ring + 256 KB staging from PSRAM. |
| 0.27.0 | 139 | 2026-03-31 | **Stream upload processStream() diagnostics.** Added per-segment and per-call instrumentation: `_streamMaxGap_ms` tracks worst-case gap between `processStream()` calls, `_streamIterCount` counts calls per segment, periodic 2-second progress logs with bytes/sec, ring fill %, and writer stats. Segment ACK now includes `ring_fill_pct` byte for client visibility. Boot log updated with PSRAM ring buffer capacity. |
| 0.27.0 | 137 | 2026-03-30 | **PSRAM ring buffer for stream uploads.** Replaced flat 256 KB PSRAM upload buffer with 2 MB `SpscRingBuffer` + 64 KB fill buffer + 256 KB staging buffer. `Esp32StoragePolicy` now provides a full dual-core pipeline: Core 0 writes to ring via `processStream()`, Core 1 writer task drains ring to SD card in 256 KB batches. Added segmented stream mode — 512 KB segments with per-segment ACK (`FILE_UPLOAD_PROGRESS`) containing bytes received and segment index. |
| 0.27.0 | 136 | 2026-03-30 | **Stream upload mode (mode=3).** New upload mode where client sends raw binary data (no COBS framing) for maximum throughput. `processStream()` called from main loop when stream is active, reads serial directly into PSRAM buffer. `sendStreamSegmentAck()` sends progress after each 512 KB segment. Go CLI `uploadStream()` and Python CLI `_upload_stream()` implementations. |
| 0.27.0 | 135 | 2026-03-30 | **Windowed upload mode (mode=2).** New server-controlled flow upload mode that sends COBS-framed chunks in windows without per-chunk ACK. Server sends `FILE_UPLOAD_PROGRESS` (0xB0) as TAG_ASYNC after each window with diagnostics: `[acked_seq:u16LE][bytes_written_sd:u32LE][buf_fill_pct:u8][sd_write_rate:u16LE][crc_errors:u16LE][next_window:u16LE]` (13 bytes). Adaptive window sizing: doubles when buffer <25% full (max 128), halves when >75% full (min 4) or >10% CRC error rate. `UPLOAD_BEGIN` ACK payload for mode=2 includes initial `[window_size:u16LE]`. Reuses existing chunked double-buffer infrastructure. Updated: `hubfx.h` (UploadMode enum, packet constant), `storage_server.h/.ipp` (window state, progress send, adaptive compute), Python CLI (`--window` flag), Go CLI (`--window` flag + async filter infrastructure). |
| 0.26.2 | 131 | 2026-03-29 | **Fix: Stream upload fails for files > 1MB.** Root cause: `StreamWriter` task and Arduino `loop()` (which runs `processStreamData()`) were both pinned to Core 1. When the 1MB ring buffer filled, the writer (priority 2) preempted the Arduino task (priority 1) for sustained SD write batches (400-2400ms), starving UART RX reads. The 128KB UART software buffer overflowed, silently dropping bytes. `_streamBytesRemaining` never reached 0 → firmware stuck in stream mode. **Fix:** (1) Moved `StreamWriter` task to Core 0 for true parallel execution with serial reads on Core 1. (2) Added `streamBufferAvailable()` policy method — `processStreamData()` now checks ring buffer space before reading from serial; returns immediately when full instead of spin-waiting. (3) Replaced `esp_rom_delay_us(10)` spin-wait in `onStreamDataReceived()` with `vTaskDelay(1)` as safety fallback. Fixed incorrect "Core 0" comment on Arduino main loop (actually Core 1 per default `CONFIG_ARDUINO_RUNNING_CORE=1`). |
| 0.26.0 | 127 | 2026-03-28 | **BREAKING: Audio output channel bitmask refactoring.** Replaced `AudioOutput` enum (`Stereo=0/Left=1/Right=2`) with `AudioChannel` bitmask namespace (`CH1=0x01, CH2=0x02, ALL=0x03`). Wire protocol `output` byte in `AUDIO_PLAY` and `AUDIO_STATUS_RESP` now carries a bitmask instead of an enum ordinal. Config key renamed from `audio_output: stereo` to `output_channels: 3` (integer bitmask). Extensible to future multi-channel boards (CH3=0x04, etc.). Updated: `audio_mixer.h/ipp/cpp`, `audio_server.ipp`, `audio_client.h/cpp`, `hubfx.h`, config structs, `engine_fx`, Python framework, CLI. |
| 0.25.0 | 126 | 2026-03-28 | **Audio output channel routing config:** New `audio_output` field in `engine_fx` config section (`stereo`/`left`/`right`) — controls which speaker channel(s) engine sounds play through. Previously hardcoded to stereo. New `gun_fx` config section with `audio_output` field for gun sound routing (applied when GunFX hub audio is implemented). `GunFxHubConfig` struct + schema (`gunfx_hub_config.h`) composed into `HubFxConfig`. Helper `parseAudioOutput()` / `audioOutputString()` in `hubfx_audio.h`. Backward-compatible — missing fields default to `stereo`. |
| 0.24.0 | 123 | 2026-03-21 | **Slave core command routing:** `SLAVE_ROUTE_*` subcmd mechanism now supports core-range packet types (0xF0+) for controlling slave boards through the hub. Fire-and-forget handling for REBOOT/BOOTSEL (slave doesn't ACK — hub ACKs immediately and marks slave not-ready). ACK-based routing for SHUTDOWN, KEEPALIVE, STATUS_REQ (relays slave ACK/NACK). New `SLAVE_INFO` (0xAE) → `SLAVE_INFO_RESP` (0xAF) query returns cached `boardInfo()` from the slave's INIT_READY handshake without querying the slave. CLI commands: `gfx.info`, `gfx.init`, `gfx.status`, `gfx.reboot`, `gfx.shutdown`, `gfx.keepalive` (and `lfx.*`, `gc.*` equivalents). |
| 0.23.0 | 120 | 2026-03-21 | **USB bus recovery:** New `USB_RESET_BUS` command (0xAD) power-cycles the USB root port via `usb_host_lib_set_root_port_power()`, forcing full hub + downstream re-enumeration. Solves ESP-IDF ext_port driver limitation where a single failed reset attempt permanently disables a hub port. **Auto-recovery timer:** FreeRTOS one-shot timer (5s after disconnect, 10s cooldown) automatically triggers bus reset when a device disconnects and no replacement enumerates. Prevents cascading resets via cooldown guard. `setAutoRecovery(bool)` API to enable/disable. CLI: `hub.usb.reset`. `UsbHostStats.bus_resets` counter tracks reset count. `printStatus()` now includes bus reset count in diagnostic output. |
| 0.22.2 | 115 | 2026-03-20 | **ESP-IDF log capture:** All `ESP_LOGx()` output from ESP-IDF components (USB Host, WiFi, FreeRTOS, etc.) is now redirected into DiagLog via `esp_log_set_vprintf()` instead of writing raw text to UART0. This prevents ESP-IDF ERROR/WARN messages during USB hub hot-plug events from corrupting the binary COBS protocol stream — the root cause of CLI hangs when disconnecting/reconnecting the USB hub. IDF messages appear as `[IDF]`-prefixed LOG_MESSAGE packets, visible via the CLI `diag` command. USB stack log levels reduced from DEBUG to INFO to limit noise. |
| 0.22.1 | 112 | 2026-03-20 | **USB hub hot-plug fix:** Fixed critical bug where `_handleCdcEvent()` on disconnect did NOT call `cdc_acm_host_close()`, leaving stale CDC pseudo-devices in the driver's internal list. This prevented the USB Host Library from re-enumerating devices reconnected via a USB hub. The incorrect comment "cdc_acm_host_close() is called by the CDC driver on disconnect" has been corrected — the CDC-ACM driver only fires the callback, the user must close explicitly (safe per v2.3.0 SLIST_FOREACH_SAFE). **Boot reset-reason logging:** `esp_reset_reason()` logged at boot with human-readable labels — BROWNOUT, PANIC, and WDT resets get explicit warning messages to diagnose USB hub inrush current resets. **USB debug instrumentation:** ESP-IDF internal USB component log levels elevated (USB_HOST, USB_HUB, USB_HCDC, CDC_ACM → DEBUG). Enhanced logging in daemon task (NO_CLIENTS, ALL_FREE warnings), new device callback (slot availability), CDC event callback (event type + handle), and disconnect handler (close result). |
| 0.22.0 | 111 | 2026-03-20 | **Runtime codec supply voltage:** TAS5825M analog gain (supply voltage) now configurable via `config.yaml` under `audio.codec_supply_voltage` (values: `12v`, `15v`, `20v`, `24v`). New `AudioConfig` struct + schema (`audio_settings.h`) composed into `HubFxConfig`. Codec reads config at boot; `config.reload` applies changes at runtime via new `TAS5825Codec::setSupplyVoltage()` — briefly enters Hi-Z, writes analog gain register, returns to play mode. Static helpers `parseSupplyVoltage()` / `supplyVoltageStr()` added to `TAS5825Codec`. Default: `12v` (3S LiPo). |
| 0.21.2 | 110 | 2026-03-19 | **Engine state machine fix:** Fixed premature Starting→Running transition caused by async playback gap — `isPlaying()` returns false while the mixer command queue pre-fills the WAV buffer (~100-200ms), so the state machine misinterpreted "not yet started" as "finished playing". Added `_startupSoundConfirmed` / `_shutdownSoundConfirmed` flags that track when `isPlaying()` first returns true. State machine now waits for playback confirmation before checking crossfade timing or completion. Safety timeout (2s) prevents stuck states if sound file is missing. Fixed `enterState()` wiping `_runningSoundStarted` flag set by `crossfadeToRunning()`, which caused duplicate `playAsync()` calls — flag is now preserved across the Starting→Running transition. **AudioMixer `remainingMs()` → `remainingSec()` refactor:** Renamed to return `float` seconds (e.g., 90.6 = 90s 600ms) instead of `int` milliseconds — fixes uint32 overflow bug where `framesLeft * 1000` exceeded UINT32_MAX for files >89s at 48kHz, producing garbage values that triggered false crossfades. Calculation now uses simple float division (`framesLeft / sampleRate_Hz`). Wire protocol still sends ms as u32LE for backward compatibility. Engine crossfade constant renamed `CROSSFADE_MS` → `CROSSFADE_SEC` (0.5f). Engine config sound path defaults changed to empty strings — paths come from config.yaml at runtime. |
| 0.21.1 | 106 | 2026-03-19 | **Protocol rename:** CONFIG_GET/CONFIG_GET_RESP → CONFIG_STATUS/CONFIG_STATUS_RESP (0x91/0x92). CLI command renamed `config.get` → `config.status`. No wire format change (same byte values). **Server boilerplate refactoring:** Extracted 7 repeated patterns across BusServer subclasses into shared helpers/macros (volume formatting, config flag decoding, error message lookup, etc.). Expanded INIT handler with detailed boot diagnostics. Improved SHUTDOWN handler with graceful cleanup sequencing. |
| 0.21.0 | 101 | 2026-03-20 | **Engine FX port:** Ported engine sound effects state machine from HubFX Pico to ESP32-S3. `EngineFX` singleton with crossfade on channels 1+2 (`HubFxChannel::ENGINE_A/B`). New `hubfx_audio.h` centralizes `Mixer` type alias (`AudioMixer<EspI2SOutput, TAS5825Codec>`) and `HubFxChannel` namespace (SYSTEM=0, ENGINE_A=1, ENGINE_B=2, GUN=3). `EngineServer` BusServer handles ENGINE_START/STOP/STATUS (0x8C-0x8F). `EngineConfig` struct with declarative YAML schema integrated into `ConfigStore<HubFxConfigSchema>` — config reload applies engine settings via `onLoaded()` callback. Fixed `audio_server.h` missing includes (`audio_mixer.h`, `audio_ring_buffer.h`) for self-contained compilation. |
| 0.20.0 | 100 | 2026-03-19 | **Configuration management:** New `sfx_config` library with declarative YAML schema DSL (`yaml_schema.h`), generic `ConfigStore<TSchema>` with file read/write callbacks, raw YAML caching, and `onLoaded()` callback. `ConfigServerT<TConfigStore>` protocol handler for CONFIG_RELOAD (0x90), CONFIG_STATUS (0x91), CONFIG_SAVE (0xAC). HubFX config schema (`HubFxConfig` + `EngineConfig`) with composable `asGroup()`. Loads from LittleFS flash at boot, fires callback for applying engine settings. CLI: `config.reload`, `config.status`, `config.save`. |
| 0.19.0 | 99 | 2026-03-19 | **Dual-task Core 1 audio architecture:** Moved audio producer from Core 0 `loop()` to a dedicated FreeRTOS task on Core 1 (priority MAX-2, below consumer at MAX-1). Consumer blocks on `i2s_channel_write()` when DMA is full, yielding CPU to producer. Core 0 now handles only protocol + storage + slave management. Added SD card mutex locking in mixer file I/O (`refillWavBuffer`, `parseWavHeader`, `stop`, `produceFrame` file close) for cross-core safety. Changed direct `stopAll()` call to `stopAsync()` for thread-safe upload guard. Verified on hardware — device reports v0.19.0 build 99. |
| 0.18.1 | 96 | 2026-03-19 | **Audio producer optimization:** Removed DIAG instrumentation from hot paths (per-frame logging in `produceFrame()`, `getWavSample()`, `refillWavBuffer()`, periodic 500ms channel dump). Increased `produce()` budget from 256→1024 frames per main loop iteration. **Audio pipeline consistency cleanup:** All codec `begin()` defaults changed from hardcoded 44100 to `AUDIO_SAMPLE_RATE` (48000). I2S slot config now uses `AUDIO_BIT_DEPTH` instead of hardcoded `I2S_DATA_BIT_WIDTH_16BIT`. TAS5825M constructor default updated. `reinitialize()` sentinel changed from 44100 to 0 for proper default detection. |
| 0.18.0 | 92 | 2026-03-19 | **PCM5102A codec driver:** New `PCM5102ACodec` singleton for TI PCM5102A DAC (GPIO-only control: XSMT mute, FMT, FLT, DEMP). `CODEC_STATUS` protocol command for runtime codec diagnostics. Audio diagnostic DIAG instrumentation (since removed in 0.18.1). |
| 0.17.0 | 82 | 2026-03-18 | **TAS5825M codec integration:** Switched audio codec from SimpleI2SCodec to TAS5825Codec (TI TAS5825M stereo Class-D amplifier). I2C control on GPIO 8 (SDA) / GPIO 9 (SCL). TAS5825Codec converted to singleton pattern for AudioMixer compatibility. Supply voltage configurable (12V/15V/20V/24V). Added full wiring guide in README. |
| 0.16.0 | 80 | 2026-03-17 | **Platform migration:** Arduino ESP32 v2.x (ESP-IDF 4.4.7) → v3.3.7 (ESP-IDF 5.5.x) via pioarduino. Enables USB Hub support (TUSB2046IBVFR) via IDF 5.x internal hub driver. Rewrote `EspI2SOutput` from legacy `driver/i2s.h` to ESP-IDF 5.x channel-based `driver/i2s_std.h` API. Pinned ESP32Servo ≥3.0.5 for Arduino v3.x LEDC compatibility. |
| 0.7.3 | 40 | 2026-03-16 | Fixed loopTask stack overflow on ESP32-S3 — `StreamWriter` 2044-byte chunk buffer moved from stack to heap allocation, loopTask stack increased to 16KB (`ARDUINO_LOOP_STACK_SIZE=16384`). Fixed tab-delimited wire format for FILE_LIST/FILE_TREE (spaces in filenames no longer break parsing). CLI parsers updated with tab-first parsing + space fallback for legacy firmware. |
| 0.6.0 | 31 | 2026-03-14 | Burst upload mode (`--burst` flag) — fire-and-forget chunks without per-chunk ACK for maximum throughput. MD5 verification on all uploads — server computes running MD5 hash and returns 16-byte digest in FILE_UPLOAD_END ACK payload. CLI compares local vs remote MD5 automatically. Optional `mode` byte in FILE_UPLOAD_BEGIN payload (0=sync, 1=burst). Burst mode reports CRC error count in ACK payload. |
| 0.5.0 | 29 | 2026-03-13 | FILE_TREE (0xA9) — recursive directory tree via streaming (`sd.tree`/`flash.tree` CLI), improved `sd.ls`/`flash.ls` formatting (human-readable sizes, aligned columns, directory names in blue with trailing /) |
| 0.4.0 | 23 | 2026-03-13 | File upload support (UPLOAD_BEGIN/DATA/END/CANCEL for flash), CRC-16 per-chunk integrity with retry, shutdown cleanup for stale uploads, UART RX buffer increased to 1024 on ESP32 for large packets, `flash.upload`/`flash.download` CLI commands with visual progress bar |
| 0.3.0 | 15 | 2026-03-14 | USB Host CDC-ACM fully functional — vendored `esp_cdc_acm` library (PlatformIO can't process `idf_component.yml`), renamed `usb_host.h` → `sfx_usb_host.h` (ESP-IDF header collision fix), verified USB Host bit 3 active in STATUS |
| 0.3.0 | 11 | 2026-03-13 | USB Host integration — EspUsbHost (HW USB-OTG) init, mount/unmount logging, device tracking, STATUS flag bit 3 |
| 0.2.1 | 6 | 2026-03-12 | Fixed boot loop (partition table 8MB constraint), custom partitions.csv, PSRAM qio_opi config |
| 0.2.0 | 5 | 2026-03-11 | DiagLog over UART, core protocol hooks (INIT/STATUS/DIAG), STATUS callback, periodic diagnostics, UART serial architecture |
| 0.1.0 | 1 | 2026-03-11 | Initial skeleton — project structure, dual-core scaffold |
