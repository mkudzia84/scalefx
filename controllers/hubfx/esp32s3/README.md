# HubFX ESP32-S3

Central hub controller for the ScaleFX system, targeting the **ESP32-S3** (dual Xtensa LX7 @ 240 MHz).

Migrating from [HubFX Pico](../pico/) (RP2350). The Pico version is being superseded.

## Architecture

### Dual-Core Design

| Core | Responsibilities |
|------|------------------|
| **Core 0** | Main loop — serial protocol, storage operations, slave management, effects state machines |
| **Core 1** | Audio consumer task (I2S DMA output, priority MAX-1), Audio producer task (WAV decode + SD reads + mixing, priority MAX-2), USB Host polling |

Core 1 runs two FreeRTOS tasks pinned via `xTaskCreatePinnedToCore()`. The consumer blocks on `i2s_channel_write()` when DMA is full, yielding CPU to the lower-priority producer task. This gives the producer ~10ms of uncontested CPU time per DMA batch to decode WAV files and mix audio, while freeing Core 0 entirely for protocol handling.

All playback commands from Core 0 go through the async command queue (`playAsync()`, `stopAsync()`, etc.) — the producer task drains and executes them. SD file I/O in the mixer is mutex-protected for cross-core safety with storage operations.

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
| [ ] | `audio/audio_channels.h` | `audio/audio_channels.h` | Copy as-is (channel index constants) |
| [ ] | `audio/system_sounds.h` | `audio/system_sounds.h` | Copy as-is (file paths, playback config) |
| [ ] | `board_manager/hubfx_protocol.h` | `board_manager/hubfx_protocol.h` | Copy as-is (just a redirect to shared lib `serial/hubfx/hubfx.h`) |
| [ ] | `board_manager/slave_registry.h` | `board_manager/slave_registry.h` | Copy as-is (singleton, no platform deps) |
| [ ] | `board_manager/slave_server.h/.cpp` | `board_manager/slave_server.h/.cpp` | Copy; verify `UsbHost` API compatibility (see Phase 3) |
| [ ] | `effects/effects_config.h` | `effects/effects_config.h` | Copy as-is (constants) |
| [ ] | `effects/engine_fx.h/.cpp` | `effects/engine_fx.h/.cpp` | Review for `millis()` usage (safe on ESP32) |
| [ ] | `effects/engine_server.h/.cpp` | `effects/engine_server.h/.cpp` | Copy as-is (pure protocol handler) |
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
| [ ] | **SerialBus adaptation** | `SerialBus` in `lib/sfx_serial/serial/client/bus.h` depends on `UsbHost` class. Need to verify ESP32 `cdcRead/cdcWrite` works with COBS framing. |
| [ ] | **Slave discovery** | `tryInitSlave()`, `scanAndInitSlaves()` — logic is portable, but depends on `UsbHost` / `BusClient` API. |
| [ ] | **Slave clients** | `GunFxClient`, `LightFxClient`, `GearControlClient` — shared code extending `BusClient` → depends on `SerialBus` → depends on `UsbHost`. |
| [ ] | **Keepalive / polling** | Slave poll interval (100ms), discovery scan (5s) — logic is portable. |

### Phase 4 — Storage

| Status | Item | Details |
|--------|------|---------|
| [ ] | **SD card library** | Pico uses `SdFat` (Adafruit Fork) with `SdCardModule` singleton. ESP32 Arduino has `SD.h` / `SD_MMC.h`. Options: (a) port SdFat to ESP32 (may work), or (b) adapt `SdCardModule` to use ESP32's `SD` library. `SdCardModule` API (`begin()`, `isInitialized()`, `fs()`) must remain compatible since `AudioMixer` and `HubFxStorageServer` use it. |
| [ ] | **SD init with fallback** | Pico tries 20/15/10/5 MHz. Same pattern works on ESP32 SPI. |
| [ ] | **LittleFS flash** | Both platforms support LittleFS via Arduino framework. `FlashModule` singleton should work. Verify ESP32 partition table includes a LittleFS partition. |
| [ ] | **ConfigReader** | Uses `File32` (SdFat type) for YAML parsing. If SD library changes, file I/O calls need adaptation. The YAML parser logic itself is portable. |
| [ ] | **File operations** | `HubFxStorageServer` uses `SdCardModule::instance()` and `FlashModule::instance()`. Works if singletons compile on ESP32. |

### Phase 5 — Main Firmware Integration

| Status | Item | Details |
|--------|------|---------|
| [ ] | **Codec selection** | ~~`#define USE_WAVESHARE_PICOAUDIO`~~ Done — using `TAS5825Codec` singleton via `AudioMixer<EspI2SOutput, TAS5825Codec>`. |
| [ ] | **SD card init** | `initSdCard()` with fallback speed pattern. |
| [ ] | **Config loading** | `configReader.begin()` + `configReader.load("/config.yaml")` with defaults fallback. |
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
| I2S DOUT | 41 | I2S serial data | TAS5825M SDIN |
| I2S BCLK | 42 | I2S bit clock | TAS5825M SCK |
| I2S LRCLK | 14 | I2S word select | TAS5825M FSYNC |
| I2C SDA | 8 | I2C data | TAS5825M SDA |
| I2C SCL | 9 | I2C clock | TAS5825M SCL |
| SD CMD | 38 | SD_MMC command | MicroSD CMD |
| SD CLK | 39 | SD_MMC clock | MicroSD CLK |
| SD D0 | 40 | SD_MMC data 0 | MicroSD DAT0 |
| LED (conn) | 48 | Onboard RGB LED | (built-in) |
| USB D- | 19 | USB Host (fixed) | USB hub/device |
| USB D+ | 20 | USB Host (fixed) | USB hub/device |

### TAS5825M Codec Wiring

The TAS5825M is a stereo closed-loop Class-D audio amplifier with I2C control.
It receives audio data over I2S and is configured/controlled via I2C.

```
  ESP32-S3 DevKitC-1                TAS5825M Breakout
  ═══════════════════                ═════════════════
  GPIO 41 (I2S DOUT) ──────────── SDIN   (serial audio data)
  GPIO 42 (I2S BCLK) ──────────── SCK    (bit clock)
  GPIO 14 (I2S LRCLK) ─────────── FSYNC  (frame sync / word select)
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
The supply voltage is configured in `hubfx_esp32s3.ino` to set the correct analog gain:
```cpp
TAS5825Codec::instance().begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL,
                               AUDIO_SAMPLE_RATE, TAS5825M_12V);
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
