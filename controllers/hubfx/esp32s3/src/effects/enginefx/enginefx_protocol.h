/*
 * enginefx_protocol.h — HubFX wire surface for the EngineFX effect.
 *
 *   Master-side aircraft-engine-sound state machine.  Cycles audio
 *   loops (start → run → stop) on the local TAS5825P mixer in
 *   response to either an RC PWM toggle (bound to an InputPort role)
 *   or explicit ENGINE_START/STOP commands.
 *
 *   Packet slice: 0xC7..0xCB.
 */

#ifndef HUBFX_ENGINEFX_PROTOCOL_H
#define HUBFX_ENGINEFX_PROTOCOL_H

#include <cstdint>

namespace hubfx::effects::enginefx {

namespace EnginePacket {
    /// `[]` → ACK / NACK
    constexpr uint8_t ENGINE_START       = 0xC7;
    /// `[]` → ACK / NACK
    constexpr uint8_t ENGINE_STOP        = 0xC8;
    /// `[]` → ENGINE_STATUS_RESP
    constexpr uint8_t ENGINE_STATUS_REQ  = 0xC9;
    /// `[state:u8][toggleEngaged:u8][active:u8]`
    constexpr uint8_t ENGINE_STATUS_RESP = 0xCA;
    /// async TAG_ASYNC: `[state:u8]`
    constexpr uint8_t ENGINE_STATE_EVENT = 0xCB;
}

/// 4-state running-engine state machine.
namespace EngineState {
    constexpr uint8_t Stopped   = 0;
    constexpr uint8_t Starting  = 1;
    constexpr uint8_t Running   = 2;
    constexpr uint8_t Stopping  = 3;

    inline const char* getName(uint8_t s) {
        switch (s) {
            case Stopped:  return "stopped";
            case Starting: return "starting";
            case Running:  return "running";
            case Stopping: return "stopping";
            default:       return "unknown";
        }
    }
}

namespace EngineError {
    constexpr uint8_t ENGINE_NOT_AVAILABLE = 0xC6;   ///< effect disabled / mixer absent
    constexpr uint8_t MISSING_PATH         = 0xC7;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case ENGINE_NOT_AVAILABLE: return "Engine effect not available";
            case MISSING_PATH:         return "Engine sound path not configured";
            default:                   return nullptr;
        }
    }
}

}  // namespace hubfx::effects::enginefx

#endif  // HUBFX_ENGINEFX_PROTOCOL_H
