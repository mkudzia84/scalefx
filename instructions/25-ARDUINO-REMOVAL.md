# 25 — Arduino Framework Removal → Native ESP-IDF (HubFX) + Pico SDK (expanders)

Branch: `arduino-removal`. Goal: drop the Arduino framework and run HubFX on
native ESP-IDF and the Pico expanders (GearControl, LightFX) on the native
Raspberry Pi Pico SDK. This is the roadmap + the executed-phase log.

## TL;DR state of play (analysis, 2026-05-31)

- **HubFX/ESP32-S3 is already ~80 % IDF-native.** The PlatformIO build is the
  pioarduino *Arduino-as-an-ESP-IDF-component* path (`framework = arduino` +
  non-empty `custom_sdkconfig` → CMake/IDF underneath, ESP-IDF 5.5.4). Storage
  (`esp_vfs_fat`/`esp_vfs_littlefs`), audio I2S (`i2s_std`), the 6 Mbps wire
  UART (`sfx::NativeUartStream` over `driver/uart`), memory instrumentation
  (`heap_caps_*`, `esp_psram_*`) and the partition table are **already native**.
  Remaining Arduino surface in `controllers/hubfx/esp32s3/src/**` is small and
  concentrated:
  - `Wire` I2C: `hubfx_hw_probe.h` (11 calls), `effects/audio_codec.h` (1),
    `.ino enableI2CScan(Wire)` (1) — the only raw Arduino hardware I/O.
  - `millis()`/`delay()`: ~13 sites (mostly `expanders/expander_service.ipp`).
  - The `.ino` `setup()/loop()` entry model.
  - ~10 vestigial `#include <Arduino.h>` carrying no functional dependency.
  - `lib_deps` `ESP32Servo` / Arduino `SD`/`SPI` — consumed in `lib/`, not src.
- **The migration burden is in `controllers/lib/`, not the controllers.** The
  `.ino` files are thin. `sfx_platform.h` is the linchpin abstraction.
- **Pico expanders:** `framework = arduino` + `board_build.core = earlephilhower`
  (Arduino-Pico, itself on the Pico SDK). `sfx_platform.h`'s Pico branch already
  uses native `pico/time.h`, `pico/mutex.h`, `pico/unique_id.h`, `busy_wait_*`.
  ⚠️ **GearControl + LightFX `.ino` currently FAIL to build** (stale vs the live
  `BoardOf<TBoard, TStream, …>` stream-injection signature) — a pre-existing
  breakage, NOT caused by this work. The shared lib still compiles for Pico.

## `sfx_platform.h` — abstractions present vs. missing

Present (native both sides): `SFX_DELAY_MS/US`, `SfxMutex`, PSRAM
(`sfxPsram*`), `sfxGetBoardId`, SPSC ring, IRAM/DRAM attrs, I2S detect flags.

**Missing — must be added for a no-Arduino build** (used raw across lib today):
1. `millis()`/`micros()` — deliberately unabstracted; resolve to Arduino.
2. GPIO digital I/O (`pinMode`/`digitalWrite`/`digitalRead`).
3. I2C / `Wire` (entirely unabstracted).
4. PWM (`analogWrite` / LEDC / RP2040 PWM slices).
5. ADC (`analogRead`).
6. GPIO interrupts (`attachInterrupt*`).
7. Servo PWM (`<Servo.h>` / `<ESP32Servo.h>` — Arduino libs).
8. UART `Stream` (ESP32 half-native via `NativeUartStream`; base is Arduino `Stream`).
9. `String` (sfx_config only).
10. `getCpuFrequencyMhz` / `F_CPU` / `rp2040.*` heap+reboot helpers.

## Native equivalents (reference)

| Concern | ESP-IDF (HubFX) | Pico SDK (expanders) |
|---|---|---|
| millis/micros | `esp_timer_get_time()/1000`, `esp_timer_get_time()` | `to_ms_since_boot(get_absolute_time())`, `time_us_32()` |
| delay | `vTaskDelay` / `esp_rom_delay_us` | `sleep_ms`/`sleep_us` (Rule 16 bans blocking in effects) |
| GPIO | `driver/gpio.h` `gpio_set_direction/_level/_get_level` | `hardware/gpio.h` `gpio_init/set_dir/put/get` |
| PWM | `driver/ledc.h` | `hardware/pwm.h` `pwm_set_gpio_level` |
| ADC | `esp_adc/adc_oneshot.h` | `hardware/adc.h` |
| I2C | `driver/i2c_master.h` (new) | `hardware/i2c.h` |
| GPIO ISR | `gpio_install_isr_service` + `gpio_isr_handler_add` | `gpio_set_irq_enabled_with_callback` |
| servo | LEDC 50 Hz, or `driver/mcpwm.h` | PWM slice 50 Hz, or PIO servo |
| reboot | `esp_restart()` | `watchdog_reboot` / `reset_usb_boot` |
| cpu MHz | `esp_clk_cpu_freq()` | `clock_get_hz(clk_sys)` |

## Phased plan (each phase keeps `scalefx-flash build hubfx` green + commits)

- **P1 — Dead code removal.** Delete confirmed-unused: `sfx_audio/codec/
  {pcm5102a,simple_i2s,tas5825_m}_codec.*`, `sfx_audio/audio/pico_i2s_output.h`,
  `sfx_storage/server/pico/pico_storage_policy.cpp.todo`. Removes the #1 and #5
  Arduino-coupling hotspots outright (both are dead codecs).
- **P2 — Stale doc/comment cleanup (Rule 0).** `sfx_serial/serial/PROTOCOL.md`
  + legacy-shape comments (`SfxServer`/`BusServer`/`CommandRouter`/
  `ICommandHandler`/`addModuleHandler`) that describe deleted shapes.
- **P3 — Vestigial `<Arduino.h>` removal.** Effect `.ipp` + protocol/service
  headers that include `<Arduino.h>` but use no Arduino API → `<cstdint>`.
- **P4 — Time abstraction.** Add `SFX_MILLIS()`/`SFX_MICROS()` to
  `sfx_platform.h` (esp_timer / pico time, native both sides). Migrate
  `EffectClock`, `board_server`, the hubfx `.ino`, `expander_service`, and lib
  call sites off raw `millis()`/`micros()`.
- **P5 — I2C abstraction.** A platform I2C wrapper (`sfx_i2c.h`) with an ESP-IDF
  `i2c_master` backend (and a Pico `hardware/i2c` backend), behind which
  `i2c_device.h`, `pca9685`, `ina226`, `tas5825_p_codec`, and `hubfx_hw_probe.h`
  move off `Wire`. Keep an Arduino-`Wire` backend during transition.
- **P6 — GPIO/PWM/ADC abstraction.** Make `native_gpio.h` native (LEDC/gpio on
  ESP32, hardware/gpio+pwm on Pico); ports + `battery_monitor` follow.
- **P7 — Servo + UART Stream + entry model.** LEDC/MCPWM servo; a `Stream`-free
  wire seam; replace `.ino setup/loop` with `app_main` + loop task. This is the
  final framework-switch step (drop `framework = arduino`).
- **P8 — Pico controllers.** First unblock the stale `.ino` vs `BoardOf<>`, then
  swap the Arduino-Pico branch of each abstraction for the native Pico SDK.

## Executed-phase log (branch `arduino-removal`, hubfx green throughout)

- **P1 (done):** removed 4 dead audio codecs/outputs (`pcm5102a`, `simple_i2s`,
  `tas5825_m`, `pico_i2s_output`) — also the #1/#5 Arduino-coupling hotspots.
- **P2 (done):** PROTOCOL.md stale-server-API banner.
- **P4 (done, 4 commits):** native `SFX_MILLIS()`/`SFX_MICROS()` added to
  `sfx_platform.h`; migrated **all** ESP32/shared + audio timing off Arduino
  `millis()`/`micros()` (effect_clock, board server/bus, roles, peripherals,
  serial client, storage, usb host, hubfx effects/expander, audio hot path);
  `SFX_CPU_MHZ()` now native (`esp_clk_tree`). Only Pico-only ISR timing remains.
- **P3 (BLOCKED — now gated only on the Stream/Print seam):** vestigial
  `<Arduino.h>` removal cannot be build-verified while `sfx_platform.h` still
  `#include <Arduino.h>` (every includer gets Arduino transitively). With I2C /
  GPIO / servo / timing now native, the SOLE remaining reason `sfx_platform.h`
  pulls Arduino is the `Stream`/`Print` base classes — so P3 unblocks the moment
  the seam (below) lands. Vestigial-vs-functional includer classification is
  done (grep) and ready.

- **P5 (done) — native RTTI-free I2C.** New `sfx_peripherals/i2c/`
  (`esp_i2c_bus.h` / `pico_i2c_bus.h` / `sfx_i2c.h` selector → `SfxI2cBus`).
  `I2CDevice` → header-only CRTP `I2CDeviceT<Derived>` (no vtable; `i2c_device.cpp`
  deleted). INA226 / PCA9685 / TAS5825P codec / board I2C-scan / boot probe all
  off `TwoWire` onto a shared `hubI2cBus()` singleton. hubfx green, **−21 KB**
  (Arduino Wire lib dropped). ⚠️ bench smoke-test pending (audio / INA / PCA).
- **P6 (done) — native RTTI-free GPIO/PWM.** `gpio/{esp,pico}_native_gpio.h`
  (`driver/gpio`+`driver/ledc` / `hardware/gpio`+`hardware/pwm`) behind a
  `using NativeGpio` selector in `native_gpio.h`. hubfx status/error LEDs route
  through it. ✅ **BENCH-VERIFIED** (LEDs blink).
- **P7 (done) — native MCPWM servo.** `ports/esp_servo.h` (MCPWM, 2 groups ×
  3 op × 2 gen = 12 outputs @ 50 Hz; per-servo comparator) + `sfx_servo.h`
  selector; `MicroservoPort` holds `ServoDriver`. `ESP32Servo` dropped from
  `lib_deps`; `sfx_platform.h` drops its `<ESP32Servo.h>` block. hubfx −2 KB.
  ✅ **BENCH-VERIFIED** (servos centre + sweep on both groups, IN_3..IN_12).
- **Port drivers (done).** `NativePwmPort`/`PwmDirHBridgePort` → NativeGpio
  (was `pinMode`/`analogWrite`/`digitalWrite`). Concrete classes, build-verified
  on hubfx (not on its hot path — PCA9685 + no H-bridge there).
- **Dead code (done).** `pwm_control.{h,cpp}` deleted — legacy `PwmInput` RC
  capture, not `#include`d anywhere; was the ONLY always-compiled `.cpp` still
  using Arduino IO (`attachInterrupt`/`analogRead`/`pinMode`).

### P5 / P6 bench checklist — ✅ ALL CONFIRMED
Codec audio (native I2C), INA226 reads, PCA9685 CH1–8, status LEDs, servos:
all verified on hardware (build 687/689).

- **`sfx::Stream`/`sfx::Print` seam (done) — drop Arduino `Stream`/`Print`.**
  New `platform/sfx_stream.h` (6-method `available`/`read`/`peek`/`readBytes`/
  `write`/`flush` + `operator bool`; no `print`/`printf` — the wire is binary
  COBS). `NativeUartStream : public sfx::Stream`. Migrated every polymorphic
  `Stream*`/`Print&` holder — BoardServerBase, DiagLog, board/storage service
  `serial()`, `InputPort::uartStream()`, SbusInput, JetiExBus/frame/monitor/
  expander, sbus_input_role. ✅ **BENCH-VERIFIED** (init/status + 4.5 MB upload
  MD5-match). Exposed two latent upload bugs (see "Upload fallout" below).
- **Native RC UART (done) — delete `ArduinoStreamAdapter`.** `NativeUartStream`
  gained `beginConfig(uart_config_t, invertMask, rs485)`; `EspInputPort` drives
  SBUS (100000 8E2 + `UART_SIGNAL_RXD_INV`), Jeti EX (8N1 + manual GPIO-matrix
  half-duplex), raw — all off Arduino `HardwareSerial`. `uartStream()` returns a
  native `sfx::Stream`. hubfx **−10 KB**. Wire verified; ⚠️ SBUS/Jeti *channel
  decode* still needs an RC receiver on IN_1/IN_2.
- **Vestigial `<Arduino.h>` swept (P3 partial, done).** `esp_rmt_ppm_policy.h`
  (→ `esp_attr.h` for IRAM_ATTR), `rx_common.h`, `rx_inputs.h`, `jeti_ex_common.h`
  off `<Arduino.h>` (used nothing from it post-seam).

### Upload fallout — fixed (2026-05-31), see [27-WIRE-ASYNC-AND-UPLOAD.md](27-WIRE-ASYNC-AND-UPLOAD.md)
The seam routed the upload read through the bulk `NativeUartStream::readBytes`
(the Arduino per-byte polyfill had masked an undersized RX ring) and the faster/
segmented path surfaced two latent CLIENT bugs Studio hit (CLI never did):
1. **8 KB RX ring < 16 KB segment** → overflow stall. Fixed: ring → **32 KB**.
2. **KEEPALIVE injected mid-stream** → swallowed as file data. Fixed: keepalive
   gated off during `streamActive` (Rule 54).
3. **`FILE_UPLOAD_PROGRESS` on the lossy async queue** → dropped under the 50 Hz
   live-view flood. Fixed: registered async filters delivered reliably (Rule 53).
Wire-collision instrumentation (`SetStreamPhase` + `COLLIDE` trace) left in as a
permanent guard.

- **Framework switch (DONE) — `framework = arduino` → `framework = espidf`.**
  HubFX now builds + runs PURE ESP-IDF, **zero Arduino**. ✅ **HW-VALIDATED**
  (build 714: boots, wire IDENTIFY, RC input, audio, uploads, ~5 MB PSRAM free).
  - `src/hubfx_esp32s3.ino` → `.cpp`; `extern "C" void app_main()` spawns a
    loopTask (`setup()` once → `for(;;) loop()`, Core 0, 16 KB stack) — exact
    replica of Arduino-ESP32's loopTask. `loop()` already ends in `vTaskDelay(1)`
    so IDLE0/TWDT stay fed. Our own `loopTaskHandle` for the `[stack]` probe.
  - `platformio.ini`: `framework = espidf`; dropped `custom_sdkconfig`,
    `board_build.arduino.*`, the `ARDUINO_*` flags, the Arduino `lib_deps`
    (SPI/SD/arduino-libhelix). `sdkconfig.defaults` already carried the
    boot-critical config (PSRAM OPI + `_BOOT_INIT`, USB-Serial-JTAG console,
    8 MB QIO flash, 240 MHz, FreeRTOS HZ=1000, no BT/WiFi/LWIP).
  - `src/idf_component.yml`: `espressif/esp-dsp`, `chmorgan/esp-libhelix-mp3`
    (raw-C decoder — `#include "mp3dec.h"`), `joltwallet/littlefs`.
  - `src/CMakeLists.txt`: firmware `src/` + `../../lib` compile as ONE IDF
    component (a separate `lib_extra_dirs` component can't carry the IDF
    `REQUIRES`) with all `REQUIRES` (driver/*, esp_psram, fatfs, vfs,
    joltwallet__littlefs, usb, sdmmc, esp_cdc_acm include) + the 8 `sfx_*`
    include roots. Pico-gated lib sources compile to empty TUs.
  - Build-green fixes: ESP32 detection via `ESP_PLATFORM` (not
    `ARDUINO_ARCH_ESP32`) in `sfx_platform.h` + the direct arch gates
    (audio_config/esp_i2s_output/wire/flash); MD5 `MD5Builder` → native
    `esp_rom_md5` (`sfx_storage/server/sfx_md5.h`); Arduino macros
    `constrain`→`std::clamp`, `min`→`std::min`, `millis`→`SFX_MILLIS`.
  - Tooling: flash CLI `findVersionSource` scans `*.cpp` (entry is now
    `hubfx_esp32s3.cpp`); post-flash lightfx-program auto-deploy removed from
    `flash`/`upload` (use `scalefx-flash programs <ctrl>`).

### Remaining
- **Stale-upload recovery (firmware hardening):** an abandoned/timed-out upload
  can leave the wire wedged until reset (`checkUploadTimeout` didn't bail out).
  Surfaced once the NextTag race (below) was triggering upload timeouts.
- **ADC (deferred):** `battery_monitor.h` `analogRead` → `esp_adc`/`hardware/adc`
  (Pico-only in practice — hubfx battery is INA226/I2C — so not hubfx-verifiable).
- **P8 — Pico controllers:** unblock the stale `.ino` vs `BoardOf<>` first, then
  swap each abstraction's Arduino-Pico branch for the native Pico SDK (+ a
  TinyUSB-CDC `sfx::Stream` adapter mirroring `NativeUartStream`). Pico keeps
  `framework = arduino` + the Pico-gated `<Arduino.h>` until then.

### Client concurrency (fixed alongside) — Rule 56
`Connection.NextTag()` was an unlocked read-modify-write on the correlation-tag
counter. Studio drives the wire from several goroutines (config-apply upload +
status/telemetry pollers + Wails RPCs); two concurrent commands could get the
SAME tag → one command's ACK delivered to the other's waiter → periodic
"upload chunk @0 (seq=0): timeout" on Studio apply. Fixed with a `tagMu` mutex.
The CLI never reproduced (single-threaded). See Rule 56 + Rules 53/54.

### Legacy bench note (superseded — kept for history)
Earlier P5/P6 were build-green-not-bench-tested; now confirmed. If a native
bus/pin/servo path ever regresses, suspect the `begin()` params
(`esp_i2c_bus.h` port/pins/freq, LEDC timer) — `coredump` first.

See [26-CODE-AND-DESIGN-IMPROVEMENTS.md](26-CODE-AND-DESIGN-IMPROVEMENTS.md) for
the broader code/design catalogue surfaced by this analysis.
