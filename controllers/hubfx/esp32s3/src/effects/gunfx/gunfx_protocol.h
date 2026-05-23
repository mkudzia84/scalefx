/*
 * gunfx_protocol.h — HubFX wire surface for the GunFX effect.
 *
 *   Each gun unit couples a muzzle-flash LED, an optional recoil servo,
 *   an optional smoke heater + fan, an optional yaw/pitch turret pair,
 *   and a pair of RC inputs (fire trigger + ROF selector).  Master-side
 *   state machine drives single-shot / auto-fire at a band-selected
 *   RPM, fan puffing, voltage-scaled heater PWM, and a manual override
 *   mode (Studio "simulate" panel) backed by verbose status broadcasts.
 *
 *   Packet slices:
 *     0xCC..0xD2  primary  (fire/auto-fire/smoke/status/shot-event)
 *     0xE2..0xE5  manual override + verbose status (Phase 1, GunFX
 *                 rollout — instructions/22)
 */

#ifndef HUBFX_GUNFX_PROTOCOL_H
#define HUBFX_GUNFX_PROTOCOL_H

#include <cstdint>

namespace hubfx::effects::gunfx {

namespace GunPacket {
    /// `[id:u8]` → ACK / NACK.  Fires exactly one shot.
    constexpr uint8_t GUN_FIRE_ONCE     = 0xCC;
    /// `[id:u8][rpm:u16LE]` → ACK.  Begins auto-fire at the given RPM
    /// (0 falls back to `GunDef::defaultIntervalMs`).
    constexpr uint8_t GUN_START_FIRING  = 0xCD;
    /// `[id:u8]` → ACK.  Stops auto-fire.
    constexpr uint8_t GUN_STOP_FIRING   = 0xCE;
    /// `[id:u8][armed:u8]` → ACK.  Enable / disable smoke heater.
    constexpr uint8_t GUN_SMOKE_ARM     = 0xCF;
    /// `[]` → GUN_STATUS_RESP
    constexpr uint8_t GUN_STATUS_REQ    = 0xD0;
    /// `[count:u8]` per-entry: `[id:u8][firing:u8][smokeArmed:u8]`
    constexpr uint8_t GUN_STATUS_RESP   = 0xD1;
    /// async TAG_ASYNC: `[id:u8]` — one packet per fired shot.
    constexpr uint8_t GUN_SHOT_EVENT    = 0xD2;

    /// `[id:u8][flags:u8][yawUs:u16LE][pitchUs:u16LE][rofIndex:u8]
    ///  [fireHold:u8][smokeArm:u8][smokeFanBurst:u8]` → ACK / NACK.
    ///
    /// Puts the named gun into MANUAL OVERRIDE mode: RC reads for the
    /// selected subsystems are suspended in favour of the values in this
    /// packet.  `flags` bitmask says which fields are valid this call:
    ///   bit 0 (0x01) yaw         (yawUs)
    ///   bit 1 (0x02) pitch       (pitchUs)
    ///   bit 2 (0x04) rof         (rofIndex; 0xFF = none armed)
    ///   bit 3 (0x08) fire        (fireHold: 1 = hold trigger,
    ///                                       0 = release)
    ///   bit 4 (0x10) smoke       (smokeArm: 1 = arm heater, 0 = off)
    ///   bit 5 (0x20) fanBurst    (smokeFanBurst: 1 = one fan puff now)
    /// Unset bits leave the corresponding subsystem at its current
    /// manual value (or RC-driven for fields that have never been set
    /// in manual since `GUN_MANUAL_RELEASE`).
    /// Auto-release: the firmware reverts to RC if no MANUAL_SET arrives
    /// for `kManualTimeoutMs` (default 5000 ms) so a Studio crash never
    /// leaves a gun in puppet mode.
    constexpr uint8_t GUN_MANUAL_SET    = 0xE2;
    /// `[id:u8]` → ACK.  Exit manual mode immediately; RC takes over.
    constexpr uint8_t GUN_MANUAL_RELEASE = 0xE3;
    /// `[id:u8][enable:u8]` → ACK.  Subscribe/unsubscribe verbose status
    /// broadcasts at ~10 Hz for one gun.  Subscriptions are per-gun;
    /// disconnect releases them all.
    constexpr uint8_t GUN_VERBOSE_STATUS_REQ = 0xE4;
    /// async TAG_ASYNC.  Verbose per-gun state — every field a Studio
    /// "simulate" panel needs to mirror what the firmware is actually
    /// doing.  Layout v1 (fixed, no Rule-11 append-only — greenfield):
    ///   [id:u8]
    ///   [mode:u8]            0=rc, 1=manual
    ///   [firing:u8]
    ///   [smokeArmed:u8]
    ///   [smokeFanRunning:u8]
    ///   [heaterDuty_pct:u8]
    ///   [heaterTempCx10:i16LE]   0x7FFF = no sensor
    ///   [yawCurrentUs:u16LE]
    ///   [yawTargetUs:u16LE]
    ///   [pitchCurrentUs:u16LE]
    ///   [pitchTargetUs:u16LE]
    ///   [rofIndex:u8]            0xFF = none armed
    ///   [rofSelectorUs:u16LE]    last raw ROF channel value
    ///   [triggerUs:u16LE]        last raw trigger channel value
    ///   [shotsThisSession:u32LE]
    /// Total: 26 bytes; <<512 B payload cap.  Phase 2 fills the producer
    /// side — Phase 1 firmware NACKs SUBSCRIBE with NOT_IMPLEMENTED.
    constexpr uint8_t GUN_VERBOSE_STATUS     = 0xE5;
}

/// Bitmask values for `GUN_MANUAL_SET.flags`.
namespace GunManualFlag {
    constexpr uint8_t YAW       = 1u << 0;
    constexpr uint8_t PITCH     = 1u << 1;
    constexpr uint8_t ROF       = 1u << 2;
    constexpr uint8_t FIRE      = 1u << 3;
    constexpr uint8_t SMOKE     = 1u << 4;
    constexpr uint8_t FAN_BURST = 1u << 5;
}

/// Mode field in `GUN_VERBOSE_STATUS` packet.
namespace GunMode {
    constexpr uint8_t RC      = 0;
    constexpr uint8_t MANUAL  = 1;
}

// GunFX error codes — CLAUDE.md spec allocates 0x30..0x3F.
// (Old values 0xCB-0xCD squatted in the GunFX packet-type range —
// no collision today but inconsistent with the error-range spec;
// relocated as part of the comprehensive error-code cleanup.)
namespace GunError {
    constexpr uint8_t UNKNOWN_ID     = 0x30;
    constexpr uint8_t GUN_TABLE_FULL = 0x31;
    /// Phase 2 will set the corresponding subsystem up; Phase 1 returns
    /// this code so Studio sees the protocol round-trip work end-to-end
    /// while the firmware behaviour is still a stub.
    constexpr uint8_t NOT_IMPLEMENTED = 0x32;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case UNKNOWN_ID:      return "Unknown gun id";
            case GUN_TABLE_FULL:  return "Gun registry full";
            case NOT_IMPLEMENTED: return "Not implemented (Phase 1 stub)";
            default:              return nullptr;
        }
    }
}

}  // namespace hubfx::effects::gunfx

#endif  // HUBFX_GUNFX_PROTOCOL_H
