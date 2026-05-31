# 26 — Code & Design Improvements (catalogue)

Observations from the `arduino-removal` analysis (3 parallel audits of
`controllers/{hubfx,gearcontrol,lightfx}` + `controllers/lib/`) and the
migration work itself. Ordered by leverage. Items marked **DONE** were executed
on the `arduino-removal` branch; the rest are recommendations.

## A. Dead / removable code

1. **DONE — dead audio codecs/outputs.** `pcm5102a_codec`, `simple_i2s_codec`,
   `tas5825_m_codec`, `pico_i2s_output.h` — 0 active includers, removed.
2. **`sfx_usb/usb/pico/pico_usb_host.{h,cpp}`** — only reachable via the
   `sfx_usb_host.h` platform switch; no Pico board hosts USB. Removable if the
   Pico-USB-host direction is abandoned (else keep as the Pico branch).
3. **`sfx_storage/server/pico/pico_storage_policy.cpp.todo`** — parked stub
   (`.todo`, not compiled). Kept deliberately; delete if Pico storage is dropped.
4. **Stale docs (Rule 0):** `sfx_serial/serial/PROTOCOL.md` documents the deleted
   `SfxServer`/`BusServer`/`CommandRouter`/`ICommandHandler` server API
   (**DONE — bannered**). ~10 `.h`/README files still carry legacy-shape mentions
   in comments — a mechanical comment sweep.
5. **Stale Pico controller `.ino`s.** `gearcontrol_pico.ino` / `lightfx_pico.ino`
   **do not compile** against the live `BoardOf<TBoard, TStream, …>` /
   `begin(stream, …)` signature (stream-injection refactor). Pre-existing
   breakage unrelated to this branch — fix before any Pico work (see §C-1).

## B. The platform-abstraction seam (the highest-leverage refactor)

6. **`sfx_platform.h` is the single Arduino chokepoint — but it's incomplete.**
   It abstracts delays/heap/mutex/board-id/PSRAM well, yet **10 platform concerns
   are used RAW (Arduino) across `lib/`** and are absent from the macro layer:
   GPIO digital I/O, `millis/micros` (**DONE**), I2C, PWM (`analogWrite`), ADC
   (`analogRead`), GPIO interrupts, servo PWM, UART `Stream`, `String`,
   `getCpuFrequencyMhz` (**DONE**). Until `sfx_platform.h` itself drops
   `#include <Arduino.h>`, every includer gets Arduino transitively — so the
   migration MUST be driven from this file outward, not file-by-file.
   **Recommendation:** finish the abstraction (GPIO/I2C/ADC/PWM/servo/ISR), THEN
   remove `<Arduino.h>` from `sfx_platform.h` — that one edit makes the whole
   tree's vestigial includes build-verifiable for removal.
7. **`native_gpio.h` is mis-named** — it wraps *Arduino* `pinMode`/`digitalWrite`/
   `analogWrite`, not native SDK calls. It is the de-facto GPIO/PWM layer that
   every LED/port/indicator path funnels through, so porting it once
   (ESP32 `driver/gpio`+`driver/ledc`; Pico `hardware/gpio`+`hardware/pwm`)
   unblocks the largest cluster.
8. **The `TwoWire&` type crosses the hubfx↔lib boundary.** `i2c_device.h`,
   `pca9685`, `ina226`, `tas5825_p_codec`, and `hubfx_hw_probe.h` all pass an
   Arduino `Wire`. Introduce a thin `SfxI2cBus` handle (ESP-IDF `i2c_master` /
   Pico `hardware/i2c`) and thread *that* through `I2CDevice` so the drivers
   stop naming `TwoWire`. This is the only raw hardware I/O left in hubfx `src/`.

## C. Architecture / design

9. **Stream-injection is good — finish it.** `BoardServer` is now templated on
   `TStream` with `NativeUartStream` injected on ESP32 (off Arduino
   `HardwareSerial`). The Pico side still defaults to the Arduino CDC `Stream`.
   A `Stream`-shaped TinyUSB-CDC adapter (mirroring `NativeUartStream`) finishes
   the seam and is the single highest-risk item for the Pico native port.
10. **Protocol simplification (from the earlier audit, still open):** 191 packet
    types, with ~32 being per-effect `*_STATUS_REQ/RESP` pairs and 6 being the
    three input `*_SET_BROADCAST_HZ`/`*_FRAME_BROADCAST` clusters. Unify the
    input subscribe into one `INPUT_SET_BROADCAST_HZ`, and consider a generic
    `SUBSYS_STATUS_REQ[id]` envelope to retire the ~16 effect status pairs. See
    the protocol-rationalization notes.
11. **`EffectClock` pattern should be the rule, not the exception.** It's now
    Arduino-free and is the right model (latch once/pass, everyone reads the same
    `nowMs()`). Several drivers still read time independently; where lockstep
    matters, prefer the clock.
12. **`InputBroadcaster` extraction (done earlier this cycle) is the template**
    for de-duplicating the three input roles — apply the same "shared component"
    treatment to the per-role wire-emit code in `role_service.cpp`.

## D. Code quality / correctness

13. **Cross-task raw pointers are a latent hazard (Rule 15).** The audio
    `WavState::source` use-after-free (fixed this cycle via a busy-flag handshake)
    likely has siblings — audit every raw pointer shared between the audio
    decoder/producer tasks and the USB-host/expander threads. Prefer
    `std::atomic<T*>` + a documented ownership handshake.
14. **Unbounded `while(available())` drains are a stall class.** The Jeti parser
    one (fixed) is the archetype; grep for other `while (stream->available())`
    drains on any UART that can be noisy/floating and bound them.
15. **Host-side: async dispatch must never run on the reader path.** Fixed for
    the Go client this cycle (dispatcher goroutine). Keep that invariant — any
    new inline consumer on the reader re-introduces the command-timeout class.
16. **LF/CRLF churn.** The C++ tree is LF but Windows checkouts re-mark CRLF on
    every `git touch` (noisy diffs). A `.gitattributes` (`*.cpp text eol=lf`, etc.)
    would stop the warnings and keep diffs clean.
17. **`config_store` aggregate semantics** (fixed this cycle: absent optional
    files now count as OK) — the same "absent ≠ failure" principle should be
    audited anywhere boot uses `loadOrFallback` but a later op treats missing as
    an error.

## E. Build / tooling

18. **The build is already arduino-as-IDF-component** — the final framework
    switch is `framework =` (drop `arduino`) + an `app_main` entry + dropping the
    `ESP32Servo`/Arduino-`SD` `lib_deps`. Storage/audio/UART/memory are already
    native, so the switch is smaller than it looks once §B is done.
19. **`scalefx-flash coredump`/`programs`** (added this cycle) are the model for
    folding manual bench workflows into the CLI — consider the same for the
    espcoredump/monitor flows and a `flash --power-cycle` helper.
