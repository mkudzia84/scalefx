// tests/native/stubs/platform/sfx_platform.h
//
// Host stand-in for controllers/lib/sfx_platform/platform/sfx_platform.h.
// The real header errors out for non-Pico, non-ESP32 targets via a
// hard #error in its platform-detection block.  This stub provides
// the symbols controllers/lib/* actually USES from sfx_platform on the
// host build path:
//
//   - sfxPsramCalloc / sfxPsramMalloc / sfxPsramFree → wrap calloc/malloc/free
//   - sfxPsramFree_bytes (no PSRAM on host → 0)
//   - SFX_PLATFORM_HOST = 1 (so test code that wants to know it's
//     running on the host can check)
//
// Anything else (mutex, delay, millis, etc.) is NOT stubbed — tests
// that pull in code needing those are out of scope for the native
// suite and belong in tests/hw/ or tests/host/go_integration/.
#pragma once

#include <cstddef>
#include <cstdlib>

#define SFX_PLATFORM_HOST  1
#define SFX_PLATFORM_PICO  0
#define SFX_PLATFORM_ESP32 0
#define SFX_PLATFORM_NAME  "host"

inline void* sfxPsramCalloc(std::size_t n, std::size_t size) { return std::calloc(n, size); }
inline void* sfxPsramMalloc(std::size_t size)                { return std::malloc(size);    }
inline void  sfxPsramFree(void* ptr)                         { std::free(ptr);              }
inline std::size_t sfxPsramFree_bytes()                      { return 0; }
