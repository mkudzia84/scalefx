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
- **P3 (BLOCKED — re-sequenced after P6/P7):** vestigial `<Arduino.h>` removal
  cannot be build-verified while `sfx_platform.h` itself still
  `#include <Arduino.h>` (every includer gets Arduino transitively). It must
  follow the GPIO/I2C/servo/ISR abstractions so `sfx_platform.h` can drop the
  include first. The classification of vestigial vs functional includers is
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
  through it → build-verified there. ⚠️ bench smoke-test pending (LED blink).

### Remaining (high-leverage, higher-risk — do attended, BENCH-verify each)
- **ADC:** `battery_monitor.h` `analogRead` → `esp_adc` / `hardware/adc`
  (Pico-only in practice — hubfx battery is INA226/I2C — so not hubfx-verifiable).
- **P7 — servo + interrupts + UART Stream + entry model:** servo
  (`ESP32Servo` → LEDC/MCPWM — behaviour-critical pulse timing), GPIO interrupts
  (`attachInterrupt` → gpio ISR), Stream-free wire seam; `.ino setup/loop` →
  `app_main` + loop task. Only after servo + interrupts can **`sfx_platform.h`
  drop `<Arduino.h>`** and P3 (vestigial-include removal) become build-verifiable.
  Then drop `framework = arduino` + the `ESP32Servo`/Arduino-`SD` `lib_deps`.
- **P8 — Pico controllers:** unblock the stale `.ino` vs `BoardOf<>` first, then
  swap each abstraction's Arduino-Pico branch for the native Pico SDK (+ a
  TinyUSB-CDC `Stream` adapter mirroring `NativeUartStream`).

### Bench-verification checklist (P5/P6 are build-green, NOT bench-tested)
Flash hubfx, then confirm: codec plays audio (TAS5825P over native I2C);
`system-info` shows INA226 battery reads; PCA9685 drives CH1–8 PWM; status/error
LEDs blink (NativeGpio). If any fail, suspect the native bus/pin `begin()` params
(`esp_i2c_bus.h` port/pins/freq, LEDC timer) — `coredump` first.

See [26-CODE-AND-DESIGN-IMPROVEMENTS.md](26-CODE-AND-DESIGN-IMPROVEMENTS.md) for
the broader code/design catalogue surfaced by this analysis.
