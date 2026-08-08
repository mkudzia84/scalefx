// Tests for controllers/lib/sfx_board/motion/servo_profile_wire.h
//
// The servo-profile payload is decoded at three sites that must agree
// byte-for-byte (role attach, SERVO_SET_PROFILE, the Go pack). Locking the ONE
// codec down here is the regression net for layout drift.  Byte 6 (formerly
// the reversed/inverted flag) is RESERVED since 2.46.0 — writers send 0,
// readers ignore it; the layout stays 13 bytes for wire compat.

#include "doctest.h"
#include "servo_profile_wire.h"

#include <cstdint>

using namespace sfx_core;

TEST_CASE("ServoProfileWire pack→unpack round-trips every field") {
    ServoMotionProfile in;
    in.minUs             = 1100;
    in.maxUs             = 1900;
    in.centerUs          = 1480;        // deliberately off geometric centre
    in.maxSpeedUsPerSec  = 1234;
    in.maxAccelUsPerSec2 = 222;
    in.maxJerkUsPerSec3  = 99;

    uint8_t buf[ServoProfileWire::kSize];
    const size_t n = ServoProfileWire::pack(buf, in);
    CHECK(n == ServoProfileWire::kSize);
    CHECK(n == 13);

    ServoMotionProfile out;             // defaults differ from `in`
    ServoProfileWire::unpack(buf, n, out);

    CHECK(out.minUs             == in.minUs);
    CHECK(out.maxUs             == in.maxUs);
    CHECK(out.centerUs          == in.centerUs);
    CHECK(out.maxSpeedUsPerSec  == in.maxSpeedUsPerSec);
    CHECK(out.maxAccelUsPerSec2 == in.maxAccelUsPerSec2);
    CHECK(out.maxJerkUsPerSec3  == in.maxJerkUsPerSec3);
}

TEST_CASE("ServoProfileWire byte offsets are the canonical wire layout") {
    ServoMotionProfile in;
    in.minUs = 0x1234; in.maxUs = 0x5678; in.maxSpeedUsPerSec = 0x9ABC;
    in.centerUs = 0x0DEF;
    in.maxAccelUsPerSec2 = 0x1122; in.maxJerkUsPerSec3 = 0x3344;

    uint8_t b[ServoProfileWire::kSize];
    ServoProfileWire::pack(b, in);
    // [minUs:u16LE][maxUs:u16LE][maxSpeed:u16LE][RESERVED:u8]
    // [centerUs:u16LE][maxAccel:u16LE][maxJerk:u16LE]
    CHECK(b[0] == 0x34); CHECK(b[1] == 0x12);   // minUs LE
    CHECK(b[2] == 0x78); CHECK(b[3] == 0x56);   // maxUs LE
    CHECK(b[4] == 0xBC); CHECK(b[5] == 0x9A);   // maxSpeed LE
    CHECK(b[6] == 0);                            // RESERVED (was inverted) — always 0
    CHECK(b[7] == 0xEF); CHECK(b[8] == 0x0D);   // centerUs LE
    CHECK(b[9] == 0x22); CHECK(b[10] == 0x11);  // maxAccel LE
    CHECK(b[11] == 0x44); CHECK(b[12] == 0x33); // maxJerk LE
}

TEST_CASE("ServoProfileWire unpack ignores the reserved byte 6") {
    // An OLD writer (pre-2.46.0) may still send 1 in byte 6 — the modern
    // reader must not choke on it and must decode every other field.
    uint8_t b[ServoProfileWire::kSize] = {};
    b[0] = 0x4C; b[1] = 0x04;   // min 1100
    b[2] = 0x6C; b[3] = 0x07;   // max 1900
    b[4] = 0xF4; b[5] = 0x01;   // speed 500
    b[6] = 1;                    // legacy reversed flag — ignored
    b[7] = 0xC8; b[8] = 0x05;   // center 1480
    b[9] = 0xDE; b[10] = 0x00;  // accel 222
    b[11] = 0x63; b[12] = 0x00; // jerk 99

    ServoMotionProfile out;
    ServoProfileWire::unpack(b, sizeof b, out);
    CHECK(out.minUs             == 1100);
    CHECK(out.maxUs             == 1900);
    CHECK(out.maxSpeedUsPerSec  == 500);
    CHECK(out.centerUs          == 1480);
    CHECK(out.maxAccelUsPerSec2 == 222);
    CHECK(out.maxJerkUsPerSec3  == 99);
}

TEST_CASE("ServoProfileWire unpack is Rule-11 append-only (short payloads keep defaults)") {
    ServoMotionProfile in;
    in.minUs = 1100; in.maxUs = 1900; in.maxSpeedUsPerSec = 500;
    in.centerUs = 1480;
    in.maxAccelUsPerSec2 = 222; in.maxJerkUsPerSec3 = 99;
    uint8_t buf[ServoProfileWire::kSize];
    ServoProfileWire::pack(buf, in);

    // A 7-byte payload carries min/max/speed (+ the reserved byte) but NOT
    // centre/accel/jerk.
    ServoMotionProfile out;             // defaults: center 1500, accel 1600, jerk 0
    const uint16_t defCenter = out.centerUs, defAccel = out.maxAccelUsPerSec2, defJerk = out.maxJerkUsPerSec3;
    ServoProfileWire::unpack(buf, 7, out);

    CHECK(out.minUs            == 1100);
    CHECK(out.maxUs            == 1900);
    CHECK(out.maxSpeedUsPerSec == 500);
    // Absent fields keep their prior value.
    CHECK(out.centerUs          == defCenter);
    CHECK(out.maxAccelUsPerSec2 == defAccel);
    CHECK(out.maxJerkUsPerSec3  == defJerk);
}
