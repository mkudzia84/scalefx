# HubFX ESP32-S3

Central hub controller for the ScaleFX system, targeting the **ESP32-S3** (dual Xtensa LX7 @ 240 MHz).

This is a migration target from [HubFX Pico](../pico/) (RP2350). Code modules are being migrated individually.

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

## Migration Status

Modules to migrate from `controllers/hubfx/pico/src/`:

- [ ] **Audio mixer** — I2S output via ESP-IDF `i2s_channel_write()` (replaces PIO-I2S)
- [ ] **SD card storage** — ESP32 SD library (replaces SdFat/Adafruit Fork)
- [ ] **Config reader** — YAML config loading
- [ ] **Slave management** — USB Host via ESP32-S3 native USB-OTG (replaces PIO-USB)
- [ ] **Audio server** — Protocol handler for audio commands
- [ ] **Engine server** — Protocol handler for engine FX
- [ ] **Storage server** — Protocol handler for config/files
- [ ] **Engine FX** — RPM-based engine sound effects
- [ ] **Gun FX** — Gun sound effects
- [ ] **System sounds** — Boot/init audio cues

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
