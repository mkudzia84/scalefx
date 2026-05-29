# tests/native/

C++ host-compilable unit tests for `controllers/lib/*` pure-logic code
(no MCU required). Uses [doctest](https://github.com/doctest/doctest)
v2.4.11, vendored as a single header at `lib/doctest/doctest.h`.

## Run

```powershell
# From the repo root
./tests/native/build.ps1
```

The script compiles all `src/test_*.cpp` files + the firmware sources
they exercise into one `.tmp/tests_native.exe` and runs it. Exit code
is 0 on all-tests-pass, non-zero on any failure. Compiles with
`g++ -std=gnu++20 -Wall`.

Also runs as part of the pre-merge gate
([`tools/run-tests.ps1 -Premerge`](../../tools/run-tests.ps1) — Rule 52).

## Layout

```
tests/native/
├── README.md
├── build.ps1               # one-command build + run
├── lib/doctest/doctest.h   # vendored single-header framework (v2.4.11)
└── src/
    ├── test_main.cpp                 # doctest main entry point
    ├── test_wire.cpp                 # tests sfx_serial/serial/wire.cpp
    ├── test_motion_profile.cpp       # tests sfx_board/motion/motion_profile.h
    └── test_element_scaling.cpp      # tests sfx_board/element/element_scaling.h
```

## Adding a test

1. Create `src/test_<subject>.cpp` with `#include "doctest.h"` and
   `TEST_CASE(...)` blocks. Don't include doctest's `IMPLEMENT_WITH_MAIN`
   — that lives in `test_main.cpp`.
2. If the test exercises a `.cpp` source from `controllers/lib/*`,
   add the source path to `$LibSources` in `build.ps1` so it gets
   compiled and linked. Header-only targets don't need this.
3. Re-run `./build.ps1` — the new tests are auto-discovered.

## Why not PlatformIO native?

PIO's native test framework works but adds a toolchain lookup hop +
platform-detection quirks on Windows. For host-only pure-logic tests
that just need `g++`, direct invocation is faster and more portable.
The hardware-target firmware builds stay on PIO; this is unit tests.

## What's covered

| Test file | Subject | Lines |
|-----------|---------|-------|
| `test_wire.cpp` | CRC-8, COBS encode/decode, U16/U32/I16 LE helpers, buildPacket / encodePacket / parsePacket | ~180 in wire.cpp |
| `test_motion_profile.cpp` | `ServoMotionProfile`, `MotionProfile1D` trapezoidal + S-curve | ~215 in motion_profile.h |
| `test_element_scaling.cpp` | `scaleDuty()` for sub-rail elements | ~105 in element_scaling.h |

See [Rule 51](../../CLAUDE.md) for the test-placement contract and
[Rule 52](../../CLAUDE.md) for the pre-merge gate.
