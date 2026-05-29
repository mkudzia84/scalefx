// tests/native/stubs/Arduino.h
//
// Host-side stand-in for Arduino.h.  The real one pulls in:
//   - <stdint.h> for int32_t, uint8_t, etc.
//   - <stdlib.h> for malloc / strtol / strtof / strtoul
//   - <string.h> for memcpy / strcmp / strcasecmp etc.
//   - <stdio.h>  for snprintf
//
// controllers/lib/* defensively `#include <Arduino.h>` and then relies
// on those transitive includes.  This stub forwards them so host
// compilation finds the same types and functions, without the heavy
// Arduino runtime (HardwareSerial, String, millis(), etc.) we don't
// need for pure-logic tests.
//
// On MinGW-w64 strings.h (which defines strcasecmp) is part of the
// system headers, so #include <strings.h> works.  On MSVC it would
// require remapping to _stricmp; gcc/clang on Windows + Linux are
// the supported host toolchains here.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <strings.h>   // strcasecmp
