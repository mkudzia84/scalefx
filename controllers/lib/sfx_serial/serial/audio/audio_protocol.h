/*
 * audio_protocol.h — wire-format constants for the audio subsystem.
 *
 * Canonical home for the audio wire packets / error codes / enums.
 * Lives under `sfx_serial` (alongside `serial/ports.h`, `serial/roles.h`,
 * `serial/storage/storage_protocol.h`) so Go / Python / Studio clients
 * share one include path with the C++ firmware.  Implementations of the
 * mixer + codec drivers + the `AudioServicePolicy<TMixer>` that
 * dispatches the opcodes below live in `sfx_audio/`.
 *
 * Packet types: 0xDA..0xE1 (control / status) + 0xAA..0xAB (codec).
 * Error codes:  0x85, 0x89 inside the 0x80..0x8F generic-error band.
 *
 * NOTE (2026-05-22): control packets moved off 0x84..0x8B — that block
 * collides head-on with the hub's ExpanderService (0x80..0x87) and
 * TopologyService (0x88..0x8E), which sit earlier in the BoardOf<> pack
 * and so were eating AUDIO_STOP/QUEUE/STATUS_REQ.  Now at 0xDA..0xE1
 * (free, just past the effect block; below the BatteryServicePolicy slot
 * 0xEE and BoardService 0xEF..0xFF).  Codec 0xAA..0xAB never collided.
 *
 * The Go-side mirror is the live source of truth — keep these in sync
 * with `app/go/protocol/audio/audio.go` (when split there too).
 */

#ifndef SFX_AUDIO_PROTOCOL_H
#define SFX_AUDIO_PROTOCOL_H

#include <cstdint>

#include <serial/core/core.h>     // SerialError fallback table

// ============================================================================
// Audio packet types
// ============================================================================

namespace AudioPacket {
    // Control (0xDA..0xE1) — relocated off 0x84..0x8B (expander/topology
    // collision; see file header).
    constexpr uint8_t AUDIO_PLAY        = 0xDA;  ///< [ch:u8][vol:u8][output:u8][loopMode:u8][loopCount:u16LE][pathLen:u8][path:str]
    constexpr uint8_t AUDIO_STOP        = 0xDB;  ///< [ch:u8] (0xFF=all)
    constexpr uint8_t AUDIO_VOLUME      = 0xDC;  ///< [ch:u8][vol:u8]   ch 0xFF=master, vol 0-100
    constexpr uint8_t AUDIO_FADE        = 0xDD;  ///< [ch:u8]
    constexpr uint8_t AUDIO_QUEUE       = 0xDE;  ///< [ch:u8][vol:u8][loopCount:u16LE][behavior:u8][pathLen:u8][path:str]
    constexpr uint8_t AUDIO_QUEUE_CLEAR = 0xDF;  ///< [ch:u8] (0xFF=all)
    constexpr uint8_t AUDIO_STATUS_REQ  = 0xE0;
    constexpr uint8_t AUDIO_STATUS_RESP = 0xE1;

    // Codec hardware status (0xAA..0xAB — never collided, left in place)
    constexpr uint8_t CODEC_STATUS_REQ  = 0xAA;
    constexpr uint8_t CODEC_STATUS_RESP = 0xAB;
}

// ============================================================================
// Audio wire-format constants
// ============================================================================

namespace AudioWire {
    /// Output channel bitmask — bit 0/1 select the codec's physical
    /// output(s); `OUTPUT_ALL` = both.
    constexpr uint8_t OUTPUT_CH1 = 0x01;
    constexpr uint8_t OUTPUT_CH2 = 0x02;
    constexpr uint8_t OUTPUT_ALL = OUTPUT_CH1 | OUTPUT_CH2;

    constexpr uint8_t LOOP_NONE     = 0;
    constexpr uint8_t LOOP_FINITE   = 1;
    constexpr uint8_t LOOP_INFINITE = 2;

    constexpr uint8_t QUEUE_FINISH_LOOP = 0;
    constexpr uint8_t QUEUE_STOP_NOW    = 1;

    constexpr uint8_t CH_ALL       = 0xFF;
    constexpr uint8_t MAX_CHANNELS = 8;
}

// ============================================================================
// Audio-layer error codes (within the 0x80..0x8F band)
// ============================================================================

// Audio error codes — CLAUDE.md allocates 0xB0..0xBF.  Old values
// (0x85, 0x89) squatted in expander/topology PACKET-TYPE bytes.
namespace AudioError {
    using namespace SerialError;
    constexpr uint8_t AUDIO_FAILURE    = 0xB0;  ///< generic audio-engine failure
    constexpr uint8_t INVALID_CHANNEL  = 0xB1;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case AUDIO_FAILURE:   return "Audio error";
            case INVALID_CHANNEL: return "Invalid audio channel";
            default:              return nullptr;   // caller falls back to SerialError table
        }
    }
}

#endif  // SFX_AUDIO_PROTOCOL_H
