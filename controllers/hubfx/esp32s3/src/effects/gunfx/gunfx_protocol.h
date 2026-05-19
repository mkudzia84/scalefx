/*
 * gunfx_protocol.h — HubFX wire surface for the GunFX effect.
 *
 *   Each gun unit couples a muzzle-flash LED, an optional recoil servo,
 *   an optional smoke heater, and an optional RC PWM trigger input.
 *   Master-side state machine drives a single-shot or auto-fire
 *   sequence: muzzle flash queue + recoil servo jerk + audio play.
 *
 *   Packet slice: 0xCC..0xD2 in the HubFX 0x80..0xCF range.
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
}

namespace GunError {
    constexpr uint8_t UNKNOWN_ID    = 0xCB;
    constexpr uint8_t GUN_TABLE_FULL= 0xCC;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case UNKNOWN_ID:     return "Unknown gun id";
            case GUN_TABLE_FULL: return "Gun registry full";
            default:             return nullptr;
        }
    }
}

}  // namespace hubfx::effects::gunfx

#endif  // HUBFX_GUNFX_PROTOCOL_H
