/*
 * servo_profile_wire.h — the ONE canonical wire codec for a ServoMotionProfile.
 *
 * The 13-byte servo-profile payload travels two paths that MUST agree
 * byte-for-byte: the role-attach cfg (apply_hubfx_config.h pack →
 * RoleServicePolicy::attachServoActuator unpack) and SERVO_SET_PROFILE
 * (handleServoSetProfile). Three hand-coded copies of the same offsets is how
 * the REV-on-attach bug happened. This header is the single definition; every
 * site calls pack()/unpack() so the layout can never drift again.
 *
 * Layout (Rule 11 append-only — older readers stop early, newer fields default):
 *   [minUs:u16LE][maxUs:u16LE][maxSpeedUsPerSec:u16LE][RESERVED:u8]
 *   [centerUs:u16LE][maxAccelUsPerSec2:u16LE][maxJerkUsPerSec3:u16LE]   = 13 bytes
 *
 * Byte 6 was the `inverted`/reversed flag — RETIRED 2.46.0 (direction now
 * lives as absolute open/close µs in each effect's config; the profile is a
 * pure motion envelope).  The byte stays in the layout for wire compat:
 * writers send 0, readers ignore it.
 *
 * Go mirror: protocol/roles CmdServoSetProfile / decode (b[6] = reserved).
 */

#ifndef SFX_SERVO_PROFILE_WIRE_H
#define SFX_SERVO_PROFILE_WIRE_H

#include <cstddef>
#include <cstdint>

#include <serial/core/core.h>     // SfxWire::putU16LE / getU16LE
#include "motion_profile.h"       // sfx_core::ServoMotionProfile

namespace sfx_core {

struct ServoProfileWire {
    static constexpr size_t kSize = 13;

    /// Serialise `p` into `buf` (must hold ≥ kSize). Returns bytes written.
    static size_t pack(uint8_t* buf, const ServoMotionProfile& p) {
        SfxWire::putU16LE(&buf[0],  p.minUs);
        SfxWire::putU16LE(&buf[2],  p.maxUs);
        SfxWire::putU16LE(&buf[4],  p.maxSpeedUsPerSec);
        buf[6]                    = 0;   // RESERVED (was inverted — retired 2.46.0)
        SfxWire::putU16LE(&buf[7],  p.centerUs);
        SfxWire::putU16LE(&buf[9],  p.maxAccelUsPerSec2);
        SfxWire::putU16LE(&buf[11], p.maxJerkUsPerSec3);
        return kSize;
    }

    /// Overlay the fields present in `buf[0..len)` onto `p` (Rule 11
    /// append-only: a field whose bytes are absent keeps `p`'s current value,
    /// so a caller seeds `p` from defaults / the live profile first).
    static void unpack(const uint8_t* buf, size_t len, ServoMotionProfile& p) {
        if (len >= 4)  { p.minUs = SfxWire::getU16LE(&buf[0]);
                         p.maxUs = SfxWire::getU16LE(&buf[2]); }
        if (len >= 6)  p.maxSpeedUsPerSec  = SfxWire::getU16LE(&buf[4]);
        // buf[6] reserved (was inverted) — ignored.
        if (len >= 9)  p.centerUs          = SfxWire::getU16LE(&buf[7]);
        if (len >= 11) p.maxAccelUsPerSec2 = SfxWire::getU16LE(&buf[9]);
        if (len >= 13) p.maxJerkUsPerSec3  = SfxWire::getU16LE(&buf[11]);
    }
};

}  // namespace sfx_core

#endif  // SFX_SERVO_PROFILE_WIRE_H
