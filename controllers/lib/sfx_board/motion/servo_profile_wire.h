/*
 * servo_profile_wire.h — the ONE canonical wire codec for a ServoMotionProfile.
 *
 * The 13-byte servo-profile payload travels two paths that MUST agree
 * byte-for-byte: the role-attach cfg (apply_hubfx_config.h pack →
 * RoleServicePolicy::attachServoActuator unpack) and SERVO_SET_PROFILE
 * (handleServoSetProfile). Three hand-coded copies of the same offsets is how
 * the REV-on-attach bug happened (the pack site set `inverted`, the unpack site
 * silently ignored it). This header is the single definition; every site calls
 * pack()/unpack() so the layout can never drift again.
 *
 * Layout (Rule 11 append-only — older readers stop early, newer fields default):
 *   [minUs:u16LE][maxUs:u16LE][maxSpeedUsPerSec:u16LE][inverted:u8]
 *   [centerUs:u16LE][maxAccelUsPerSec2:u16LE][maxJerkUsPerSec3:u16LE]   = 13 bytes
 *
 * Go mirror: protocol/roles CmdServoSetProfile / decode (b[6] = reversed).
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
        buf[6]                    = p.inverted ? 1 : 0;
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
        if (len >= 7)  p.inverted          = buf[6] != 0;
        if (len >= 9)  p.centerUs          = SfxWire::getU16LE(&buf[7]);
        if (len >= 11) p.maxAccelUsPerSec2 = SfxWire::getU16LE(&buf[9]);
        if (len >= 13) p.maxJerkUsPerSec3  = SfxWire::getU16LE(&buf[11]);
    }
};

}  // namespace sfx_core

#endif  // SFX_SERVO_PROFILE_WIRE_H
