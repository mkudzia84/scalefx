// Tests for controllers/lib/sfx_board/motion/motion_profile.h
//
// Locks in the math of ServoMotionProfile + MotionProfile1D:
//   - clamp (pure cap since 2.46.0 — the inverted mirror is retired)
//   - hasSlew() gate (zero limits → write-through, no integration)
//   - trapezoidal motion (no jerk cap): cruise at max speed, decel
//     to land on target
//   - jerk-bounded S-curve: rate-of-acceleration clamp
//   - end-state convergence (within 0.5 µs of target → snap + idle)
//
// These tests run a fixed dt over many ticks and assert on the steady-
// state trajectory.  Where exact-equality assertions would be brittle
// against float drift, we use a tolerance band.

#include "doctest.h"
#include "motion_profile.h"

#include <cstdint>
#include <cmath>

using namespace sfx_core;

// ---- ServoMotionProfile config struct --------------------------------

TEST_CASE("ServoMotionProfile default values are servo-safe") {
    ServoMotionProfile p;
    CHECK(p.minUs == 1000);
    CHECK(p.maxUs == 2000);
    CHECK(p.centerUs == 1500);
    CHECK(p.maxSpeedUsPerSec  == 800);
    CHECK(p.maxAccelUsPerSec2 == 1600);
    CHECK(p.maxJerkUsPerSec3  == 0);
    CHECK(p.hasSlew());
}

TEST_CASE("clamp() bounds within [min, max]") {
    ServoMotionProfile p; p.minUs = 1100; p.maxUs = 1900;
    CHECK(p.clamp(500)  == 1100);
    CHECK(p.clamp(2500) == 1900);
    CHECK(p.clamp(1500) == 1500);
    CHECK(p.clamp(1100) == 1100);
    CHECK(p.clamp(1900) == 1900);
}

TEST_CASE("clamp() is a pure cap — no hidden mapping (2.46.0)") {
    // The old `inverted` mirror (min+max−target) inside clamp() was one of
    // the THREE stacked direction layers behind the 2026-08-08 "double
    // reverse" saga.  clamp() must never remap — only bound.
    ServoMotionProfile p; p.minUs = 1100; p.maxUs = 1900;
    CHECK(p.clamp(1100) == 1100);
    CHECK(p.clamp(1900) == 1900);
    CHECK(p.clamp(1500) == 1500);
    CHECK(p.clamp(500)  == 1100);
    CHECK(p.clamp(2500) == 1900);
}

TEST_CASE("hasSlew is false only when BOTH speed and accel are 0") {
    ServoMotionProfile p;
    p.maxSpeedUsPerSec = 0; p.maxAccelUsPerSec2 = 0;
    CHECK_FALSE(p.hasSlew());

    p.maxSpeedUsPerSec = 100;
    CHECK(p.hasSlew());

    p.maxSpeedUsPerSec = 0; p.maxAccelUsPerSec2 = 100;
    CHECK(p.hasSlew());
}

// ---- MotionProfile1D snapTo + atTarget -------------------------------

TEST_CASE("snapTo jumps to target with no slew") {
    ServoMotionProfile p; // defaults
    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(1700);
    CHECK(m.current() == 1700);
    CHECK(m.target()  == 1700);
    CHECK(m.atTarget());
}

TEST_CASE("snapTo clamps to range") {
    ServoMotionProfile p; // 1000..2000
    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(500);
    CHECK(m.current() == 1000);
    CHECK(m.atTarget());
}

// ---- No-slew profile: write-through on first tick --------------------

TEST_CASE("zero slew profile writes through on tick") {
    ServoMotionProfile p;
    p.maxSpeedUsPerSec  = 0;
    p.maxAccelUsPerSec2 = 0;
    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(1500);
    m.setTarget(1800);
    m.tick(10);   // 10 ms tick
    CHECK(m.current() == 1800);
    CHECK(m.atTarget());
}

// ---- dtMs == 0 is a no-op (Rule 40 idempotent latch) ----------------

TEST_CASE("dtMs=0 tick does not advance position") {
    ServoMotionProfile p; // slew active
    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(1500);
    m.setTarget(1800);
    const uint16_t before = m.current();
    m.tick(0);
    CHECK(m.current() == before);
}

// ---- Trapezoidal (no jerk): velocity-clamped ramp -------------------

TEST_CASE("trapezoidal ramp reaches target without overshoot") {
    ServoMotionProfile p;
    p.minUs = 1000; p.maxUs = 2000;
    p.maxSpeedUsPerSec  = 1000;   // 1000 µs/sec
    p.maxAccelUsPerSec2 = 4000;   // 4 µs/ms² — fast accel
    p.maxJerkUsPerSec3  = 0;      // trap, no jerk cap

    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(1500);
    m.setTarget(1800);

    // 300 µs at 1000 µs/sec = 0.3 s ideal travel; with finite accel +
    // decel, expect ~0.5 s real.  Run 2 s worth of 1 ms ticks.
    for (int i = 0; i < 2000; ++i) m.tick(1);

    CHECK(m.current() == 1800);
    CHECK(m.atTarget());
}

TEST_CASE("trapezoidal ramp respects clamp on the input side") {
    ServoMotionProfile p;
    p.minUs = 1100; p.maxUs = 1900;
    p.maxSpeedUsPerSec  = 1000;
    p.maxAccelUsPerSec2 = 4000;
    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(1500);
    m.setTarget(2500);   // beyond maxUs
    for (int i = 0; i < 2000; ++i) m.tick(1);
    CHECK(m.current() == 1900);
}

TEST_CASE("trapezoidal ramp lands EXACTLY on the commanded absolute target") {
    // 2.46.0 explicit-position model: setTarget is absolute — no mapping
    // anywhere between the commanded µs and the settled position.
    ServoMotionProfile p;
    p.minUs = 1100; p.maxUs = 1900;
    p.maxSpeedUsPerSec  = 1000;
    p.maxAccelUsPerSec2 = 4000;
    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(1500);
    m.setTarget(1100);
    for (int i = 0; i < 2000; ++i) m.tick(1);
    CHECK(m.current() == 1100);
}

TEST_CASE("trapezoidal max-speed bound respected at cruise") {
    ServoMotionProfile p;
    p.minUs = 1000; p.maxUs = 5000;   // wide range so cruise dominates
    p.maxSpeedUsPerSec  = 500;        // 0.5 µs/ms
    p.maxAccelUsPerSec2 = 4000;       // accel is fast; cruise phase covers most travel
    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(1000);
    m.setTarget(4000);

    // After ~200 ms we should be near cruise speed and have moved
    // ~100 µs (accel ramp eats a bit) + cruise — roughly between
    // 1050 and 1200.  Just check we're past the bottom and not blown
    // past the speed cap (sample two ticks 100 ms apart, delta should
    // be ≤ 0.5 µs/ms × 100 ms = 50 µs).
    for (int i = 0; i < 200; ++i) m.tick(1);
    const uint16_t a = m.current();
    for (int i = 0; i < 100; ++i) m.tick(1);
    const uint16_t b = m.current();
    const int delta = (int)b - (int)a;
    CAPTURE(a); CAPTURE(b);
    CHECK(delta > 0);
    CHECK(delta <= 55);   // 50 + tolerance for accel-ramp transient
}

// ---- S-curve (jerk-bounded) ------------------------------------------

TEST_CASE("S-curve profile converges to target (jerk cap engaged)") {
    ServoMotionProfile p;
    p.minUs = 1000; p.maxUs = 2000;
    p.maxSpeedUsPerSec  = 500;
    p.maxAccelUsPerSec2 = 2000;
    p.maxJerkUsPerSec3  = 8000;   // S-curve mode
    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(1500);
    m.setTarget(1800);

    // S-curve takes longer to settle than trapezoid but still
    // converges.  Run 4 seconds of 1 ms ticks.
    for (int i = 0; i < 4000; ++i) m.tick(1);
    CHECK(m.current() == 1800);
    CHECK(m.atTarget());
}

// ---- setProfile in-flight clamps current() to the new range ---------

TEST_CASE("setProfile re-clamps current to the new range") {
    ServoMotionProfile wide;
    wide.minUs = 1000; wide.maxUs = 2000;
    MotionProfile1D m;
    m.setProfile(wide);
    m.snapTo(1900);

    ServoMotionProfile narrow;
    narrow.minUs = 1100; narrow.maxUs = 1600;
    narrow.maxSpeedUsPerSec  = 800;
    narrow.maxAccelUsPerSec2 = 1600;
    m.setProfile(narrow);
    // current was 1900, narrow.maxUs is 1600 → must be clamped.
    CHECK(m.current() == 1600);
}

// ---- Target setpoint is itself clamped ------------------------------

TEST_CASE("setTarget clamps to current profile range") {
    ServoMotionProfile p;
    p.minUs = 1100; p.maxUs = 1900;
    MotionProfile1D m;
    m.setProfile(p);
    m.setTarget(500);
    CHECK(m.target() == 1100);
    m.setTarget(2500);
    CHECK(m.target() == 1900);
}

// ---- atTarget is false during the ramp ------------------------------

TEST_CASE("atTarget is false mid-ramp, true after settle") {
    ServoMotionProfile p;   // defaults: speed 800, accel 1600 — slow
    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(1500);
    m.setTarget(1800);

    m.tick(10);             // 10 ms in: still moving
    CHECK_FALSE(m.atTarget());

    for (int i = 0; i < 2000; ++i) m.tick(1);  // ~2 s settle
    CHECK(m.atTarget());
}

// ---- Reverse-direction (decel + accel through zero) -----------------

TEST_CASE("reversing target mid-ramp converges to new target") {
    ServoMotionProfile p;
    p.minUs = 1000; p.maxUs = 2000;
    p.maxSpeedUsPerSec  = 1000;
    p.maxAccelUsPerSec2 = 4000;
    MotionProfile1D m;
    m.setProfile(p);
    m.snapTo(1500);

    m.setTarget(1800);
    for (int i = 0; i < 100; ++i) m.tick(1);   // partial travel
    REQUIRE_FALSE(m.atTarget());

    m.setTarget(1200);                          // reverse
    for (int i = 0; i < 4000; ++i) m.tick(1);  // give it plenty of time
    CHECK(m.current() == 1200);
    CHECK(m.atTarget());
}
