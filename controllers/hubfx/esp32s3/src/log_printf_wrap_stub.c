/*
 * log_printf_wrap_stub.c — supplies `__wrap_log_printf` for ESP32-S3
 * builds under the Arduino-as-IDF-component path.
 *
 * Background
 * ──────────
 * pioarduino's linker flag file
 *   ~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/flags/ld_flags
 * adds `-Wl,--wrap=log_printf` unconditionally.  The implementation
 * `__wrap_log_printf` ships inside `libespressif__esp_diagnostics.a` —
 * BUT only for esp32, esp32c3, esp32c5, esp32c6, esp32h2, esp32s2.
 * The ESP32-S3 archive does NOT include it (verified 2026-05-28 against
 * framework-arduinoespressif32-libs / Espressif Arduino-ESP32 v3.3.x).
 * Without it, every Arduino log_v/log_d/log_i/log_w/log_e call from
 * Arduino-ESP32 internals or libraries (ESP32Servo,
 * esp32-hal-i2c-slave, …) becomes an undefined-reference at link time
 * when the IDF-component path is active.
 *
 * Why a stub here (not in lib/)
 * ─────────────────────────────
 * This is purely a pioarduino/IDF-component packaging gap for the S3
 * target.  It's NOT a HubFX concern — drop it here so the bridge lives
 * at the same level as the build that needs it.  If GunFx or any other
 * ESP32-S3 board ever joins, lift this into a shared location.  When
 * Espressif ships esp_diagnostics for S3 the linker will report a
 * duplicate definition — at that point delete this file and bump the
 * platform-espressif32 pin to the fixed version.
 *
 * Behaviour
 * ─────────
 * Transparent pass-through to `log_printfv` (defined in
 * framework-arduinoespressif32/cores/esp32/esp32-hal-uart.c) — the
 * same path the original `log_printf` takes internally.  No recursion:
 * `log_printfv` is NOT wrapped, so calling it directly bypasses the
 * `--wrap=log_printf` rewrite.
 */

#include <stdarg.h>

extern int log_printfv(const char *format, va_list arg);

int __wrap_log_printf(const char *format, ...) {
    va_list arg;
    va_start(arg, format);
    const int n = log_printfv(format, arg);
    va_end(arg);
    return n;
}
