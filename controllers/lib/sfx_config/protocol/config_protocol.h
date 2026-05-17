/*
 * config_protocol.h — wire-format constants for the config-store subsystem.
 *
 * Owned by sfx_config (alongside the YAML parser + ConfigStore + the
 * service policy that dispatches the opcodes below).  Was part of the
 * monolithic `serial/hubfx/hubfx.h` umbrella before that header was
 * split per subsystem.
 *
 * Packet types: 0x90..0x92 (reload/status), 0xAC (save).
 */

#ifndef SFX_CONFIG_PROTOCOL_H
#define SFX_CONFIG_PROTOCOL_H

#include <cstdint>

#include <serial/core/core.h>

namespace ConfigPacket {
    constexpr uint8_t CONFIG_RELOAD      = 0x90;  ///< [] or [pathLen:u8][path:str] → ACK/NACK
    constexpr uint8_t CONFIG_STATUS      = 0x91;
    constexpr uint8_t CONFIG_STATUS_RESP = 0x92;  ///< [loaded:u8][size:u16LE][validOk:u8]
    constexpr uint8_t CONFIG_SAVE        = 0xAC;  ///< [] or [pathLen:u8][path:str] → ACK/NACK
}

namespace ConfigError {
    using namespace SerialError;
    constexpr uint8_t CONFIG_FAILURE = 0x88;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case CONFIG_FAILURE: return "Config error";
            default:             return nullptr;
        }
    }
}

#endif  // SFX_CONFIG_PROTOCOL_H
