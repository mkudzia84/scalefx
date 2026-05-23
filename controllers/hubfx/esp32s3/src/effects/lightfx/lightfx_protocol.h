/*
 * lightfx_protocol.h — HubFX wire surface for the LightFX effect.
 *
 *   Studio / CLI talks to the hub through this packet set.  The hub's
 *   `LightFxEffectServicePolicy` orchestrates LED programs + driver
 *   commands across hub-local LEDs and expander LEDs (via topology) —
 *   the host never addresses individual ports for program control.
 *
 *   Packet slice: 0xB7..0xBD inside the HubFX-master 0x80..0xCF range
 *   (0xB0 = upload progress, 0xB1..0xB6 = landing-lights, this is the
 *   next free block).
 *
 *   Programs are mutually exclusive — `LIGHTFX_PROGRAM_SELECT` is an
 *   atomic switch.  Master brightness is a dynamic 0..100 scalar that
 *   applies to EVERY claimed LightFx LED channel; changes re-broadcast
 *   `LED_SET_BRIGHTNESS` to the current set.
 *
 *   Rule 11 append-only.
 */

#ifndef HUBFX_LIGHTFX_PROTOCOL_H
#define HUBFX_LIGHTFX_PROTOCOL_H

#include <cstdint>

namespace hubfx::effects::lightfx {

namespace LightFxPacket {
    /// `[]` → LIGHTFX_PROGRAM_LIST_RESP
    constexpr uint8_t LIGHTFX_PROGRAM_LIST_REQ      = 0xB7;
    /// `[count:u8]` per-entry: `[idx:u8][nameLen:u8][name:str]`
    constexpr uint8_t LIGHTFX_PROGRAM_LIST_RESP     = 0xB8;
    /// `[idx:u8]` → ACK / NACK.  Atomic program switch.
    constexpr uint8_t LIGHTFX_PROGRAM_SELECT        = 0xB9;
    /// `[]` → ACK.  Drops the active program; every claimed LED off.
    constexpr uint8_t LIGHTFX_PROGRAM_RESET         = 0xBA;
    /// `[]` → LIGHTFX_STATUS_RESP
    constexpr uint8_t LIGHTFX_STATUS_REQ            = 0xBB;
    /// `[activeIdx:u8 (0xFF=none)][masterBrightnessPct:u8]`
    constexpr uint8_t LIGHTFX_STATUS_RESP           = 0xBC;
    /// `[brightnessPct:u8 (0..100)]` → ACK.  Live, scales every channel.
    constexpr uint8_t LIGHTFX_MASTER_BRIGHTNESS_SET = 0xBD;
}

/// Sentinel returned in `LIGHTFX_STATUS_RESP.activeIdx` when no
/// program is currently active.
constexpr uint8_t kLightFxNoActiveProgram = 0xFF;

/// LightFX-effect error codes — CLAUDE.md spec allocates 0x50..0x5F.
/// (Old values 0xB6-0xB8 squatted in the LightFX packet-type range —
/// no collision today but inconsistent with the error-range spec.)
namespace LightFxError {
    constexpr uint8_t UNKNOWN_PROGRAM   = 0x50;
    constexpr uint8_t PROGRAM_TOO_LARGE = 0x51;
    constexpr uint8_t BRIGHTNESS_RANGE  = 0x52;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case UNKNOWN_PROGRAM:   return "Unknown lightfx program index";
            case PROGRAM_TOO_LARGE: return "Program exceeds wire payload budget";
            case BRIGHTNESS_RANGE:  return "Brightness must be 0..100";
            default:                return nullptr;
        }
    }
}

}  // namespace hubfx::effects::lightfx

#endif  // HUBFX_LIGHTFX_PROTOCOL_H
