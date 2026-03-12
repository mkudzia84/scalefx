# HubFX ESP32-S3

Central hub controller for the ScaleFX system, targeting the **ESP32-S3** (dual Xtensa LX7 @ 240 MHz).

Migrating from [HubFX Pico](../pico/) (RP2350). The Pico version is being superseded.

## Architecture

### Dual-Core Design

| Core | Responsibilities |
|------|------------------|
| **Core 0** | Main loop — serial protocol, SD card reads, WAV decoding, audio mixing (producer), slave management, effects state machines |
| **Core 1** | Audio I2S output (consumer), USB Host polling |

Core 1 runs as a FreeRTOS task pinned via `xTaskCreatePinnedToCore()` (replaces Pico's `setup1()`/`loop1()` pattern).

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
- [ ] Pin definitions — finalize I2S, SD, I2C pins for target board
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
| [ ] | **I2S driver** | Pico uses PIO-I2S (`I2S.h`). ESP32-S3 uses hardware I2S via `driver/i2s_std.h`. AudioMixer's `beginI2S()` / `consume()` need ESP32-S3 I2S init + `i2s_channel_write()`. This is the biggest single migration item. |
| [ ] | **Codec init** | Pico uses `SimpleI2SCodec` (PCM5101A) or `TAS5825Codec`. Same codec classes work on ESP32 (I2C is standard Wire). Verify `Wire.begin(SDA, SCL)` pin assignment. |
| [ ] | **Audio mixer begin** | `mixer.begin(data, bclk, lrclk, codec)` — shared code, but verify `SdCardModule` singleton works with ESP32 SD lib. |
| [ ] | **Audio producer** | `mixer.produce(2048)` in `loop()` — shared code, should work as-is. |
| [ ] | **Audio consumer** | `mixer.consume()` in Core 1 task — shared code, but I2S write path needs ESP32 native driver. |
| [ ] | **MCLK support** | ESP32-S3 has native MCLK output (Pico does not). Some DACs need it. Add optional MCLK pin config. |

### Phase 3 — USB Host (Slave Management)

This is the most significant platform difference. Pico uses PIO-USB (software USB via PIO state machine). ESP32-S3 has native USB-OTG hardware.

| Status | Item | Details |
|--------|------|---------|
| [ ] | **USB Host driver** | Replace `UsbHost` (PIO-USB, `usb_host.h`) with ESP32-S3 native USB Host. Options: ESP-IDF `usb_host_lib` or TinyUSB host (ESP-IDF includes TinyUSB support). |
| [ ] | **CDC device enumeration** | `UsbHost::cdcDeviceCount()`, `getCdcDevice()` — reimplement for ESP32-S3 USB Host API. |
| [ ] | **SerialBus adaptation** | `SerialBus` in `lib/sfx_serial/serial/client/bus.h` depends on `UsbHost` class. Need ESP32-compatible `UsbHost` implementation or an abstraction layer. |
| [ ] | **TinyUSB config** | Pico's `tusb_config.h` is RP2350-specific (PIO1, specific descriptors). ESP32-S3 needs its own TinyUSB config or uses ESP-IDF native USB Host. **Do not copy Pico's `tusb_config.h`**. |
| [ ] | **Slave discovery** | `tryInitSlave()`, `scanAndInitSlaves()` — logic is portable, but depends on `UsbHost` / `BusClient` API. |
| [ ] | **Mount/unmount callbacks** | `onUsbMount()`, `onUsbUnmount()` — reimplement with ESP32-S3 USB Host event model. |
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
| [ ] | **Codec selection** | `#define USE_WAVESHARE_PICOAUDIO` → define appropriate codec for ESP32-S3 DAC board. |
| [ ] | **SD card init** | `initSdCard()` with fallback speed pattern. |
| [ ] | **Config loading** | `configReader.begin()` + `configReader.load("/config.yaml")` with defaults fallback. |
| [ ] | **Audio init** | `initAudio()` — codec begin + mixer begin. |
| [ ] | **USB Host init** | `usbHost.begin()` + mount/unmount callbacks. |
| [ ] | **Slave pre-registration** | `slaveRegistry.registerSlave()` for GunFX, LightFX, GearControl. |
| [ ] | **Log message relay** | `*Client.onLogMessage()` → `DiagLog::instance().ingest()`. |
| [ ] | **Domain handlers** | Wire up `SlaveServer`, `HubFxAudioServer`, `EngineServer`, `HubFxStorageServer` — set dependencies, register with `server.addModuleHandler()`. |
| [ ] | **STATUS callback** | `server.core().onStatusData()` — 14-byte hub status (slave mask, SD, audio, PC serial, loop1Count, ring stats). |
| [ ] | **PC serial detection** | Pico uses `(bool)Serial` → `tud_cdc_connected()`. ESP32-S3 USB CDC: check `Serial` / `USBSerial` operator bool or DTR state (`Serial.dtr()`). Behavior may differ. |
| [ ] | **Main loop** | Serial processing (conditional on PC connected), slave polling, engine FX, audio producer, 5-second diagnostic logging. |
| [ ] | **Init sound** | Optional startup chime on system sounds channel. |

### Phase 6 — Platform-Specific Details

| Status | Item | Details |
|--------|------|---------|
| [ ] | **Free heap** | Pico: `rp2040.getFreeHeap()`. ESP32: `SFX_FREE_HEAP()` → `esp_get_free_heap_size()`. Already in `sfx_platform.h`. |
| [ ] | **PSRAM** | ESP32-S3-DevKitC-1 may have PSRAM. Consider using it for audio buffers (`WAV_BUF_FRAMES`, ring buffer). Needs board variant + `board_build.arduino.memory_type` in `platformio.ini`. |
| [ ] | **Watchdog** | ESP32 FreeRTOS has task watchdog (`esp_task_wdt`). Core 1 audio task must feed it or be excluded. |
| [ ] | **Partition table** | Needs partitions for: app, OTA (optional), LittleFS. Currently uses `default_16MB.csv` — verify it includes a data partition for LittleFS. |
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
| `serial/client/bus.h` | ⚠️ Depends | `SerialBus` depends on `UsbHost` class which is RP2350-specific (PIO-USB). Needs ESP32 USB Host backend. |
| `serial/client/usb_host.*` | ❌ Rewrite | Pico PIO-USB implementation. Must be reimplemented for ESP32-S3 native USB-OTG. |

---

## Hardware

- **MCU**: ESP32-S3-DevKitC-1 (or compatible)
- **DAC**: TBD (PCM5102A, MAX98357A, or TAS5825M via I2S)
- **Storage**: MicroSD card (SPI interface)
- **USB Host**: Native USB-OTG (ESP32-S3 has built-in USB host support)

## Pin Assignments

> **TODO**: Pin assignments pending hardware selection and board design.

## Building

```bash
# Build
cd controllers/hubfx/esp32s3
python -m platformio run -e esp32s3

# Upload via USB
python -m platformio run -e esp32s3 -t upload

# Serial monitor
python -m platformio device monitor -b 1000000
```

## Firmware Version

| Version | Build | Date | Changes |
|---------|-------|------|---------|
| 0.1.0 | 1 | 2026-03-11 | Initial skeleton — project structure, dual-core scaffold |
