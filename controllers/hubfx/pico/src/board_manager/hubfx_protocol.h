/*
 * HubFX Protocol — Packet Types and Error Codes
 *
 * Defines all HubFX-specific packet types for:
 *   - Slave management (list, init, status)
 *   - Audio control (play, stop, volume, queue, fade)
 *   - Engine FX control (start, stop, status)
 *   - Config management (reload, get)
 *   - SD card management (init, status)
 *
 * Packet range: 0x80-0xA3 (streaming 0xA4-0xA6 in serial_stream.h)
 *
 * Previous CLI text commands are now replaced by binary COBS packets
 * using the same protocol as all other ScaleFX controllers.
 */

#ifndef HUBFX_PROTOCOL_H
#define HUBFX_PROTOCOL_H

#include <stdint.h>
#include <serial_core.h>

// ============================================================================
// HubFX Packet Types (0x80-0x9F)
// ============================================================================

namespace HubFxPacket {

    // --- Slave Management (0x80-0x83) ---
    constexpr uint8_t SLAVE_LIST        = 0x80;  // [] → SLAVE_LIST_RESP
    constexpr uint8_t SLAVE_LIST_RESP   = 0x81;  // [count:u8][per-slave: type:u8,connected:u8,ready:u8,nameLen:u8,name:str]
    constexpr uint8_t SLAVE_INIT        = 0x82;  // [slaveType:u8] → ACK/NACK
    constexpr uint8_t SLAVE_STATUS      = 0x83;  // [] → ACK (status via core STATUS callback)

    // --- Audio Control (0x84-0x8B) ---
    constexpr uint8_t AUDIO_PLAY        = 0x84;  // [ch:u8][vol:u8][output:u8][loopMode:u8][loopCount:u16LE][pathLen:u8][path:str]
    constexpr uint8_t AUDIO_STOP        = 0x85;  // [ch:u8] (0xFF=all)
    constexpr uint8_t AUDIO_VOLUME      = 0x86;  // [ch:u8][vol:u8] (ch 0xFF=master, 0-7=channel, vol 0-100)
    constexpr uint8_t AUDIO_FADE        = 0x87;  // [ch:u8]
    constexpr uint8_t AUDIO_QUEUE       = 0x88;  // [ch:u8][vol:u8][loopCount:u16LE][behavior:u8][pathLen:u8][path:str]
    constexpr uint8_t AUDIO_QUEUE_CLEAR = 0x89;  // [ch:u8] (0xFF=all)
    constexpr uint8_t AUDIO_STATUS_REQ  = 0x8A;  // [] → AUDIO_STATUS_RESP
    constexpr uint8_t AUDIO_STATUS_RESP = 0x8B;  // [masterVol:u8][activeCh:u8][per-channel data...]

    // --- Engine FX Control (0x8C-0x8F) ---
    constexpr uint8_t ENGINE_START       = 0x8C;  // [] → ACK
    constexpr uint8_t ENGINE_STOP        = 0x8D;  // [] → ACK
    constexpr uint8_t ENGINE_STATUS_REQ  = 0x8E;  // [] → ENGINE_STATUS_RESP
    constexpr uint8_t ENGINE_STATUS_RESP = 0x8F;  // [state:u8][toggleEngaged:u8][active:u8]

    // --- Config Management (0x90-0x91) ---
    constexpr uint8_t CONFIG_RELOAD     = 0x90;  // [] → ACK/NACK
    constexpr uint8_t CONFIG_GET        = 0x91;  // [] → CONFIG_GET_RESP
    constexpr uint8_t CONFIG_GET_RESP   = 0x92;  // [data...] (raw YAML config)

    // --- SD Card Management (0x93-0x95) ---
    constexpr uint8_t SD_INIT           = 0x93;  // [speed_mhz:u8] → ACK/NACK
    constexpr uint8_t SD_STATUS_REQ     = 0x94;  // [] → SD_STATUS_RESP
    constexpr uint8_t SD_STATUS_RESP    = 0x95;  // [initialized:u8][sizeMB:u16LE]

    // --- Slave Routing (0x96-0x98) — subcmd pattern ---
    // Payload: [subcmd:u8][original_payload...]
    // The subcmd byte is the original slave packet type, forwarded to the slave.
    constexpr uint8_t SLAVE_ROUTE_GUNFX       = 0x96;  // [subcmd:u8][...] → route to GunFX
    constexpr uint8_t SLAVE_ROUTE_LIGHTFX     = 0x97;  // [subcmd:u8][...] → route to LightFX
    constexpr uint8_t SLAVE_ROUTE_GEARCONTROL = 0x98;  // [subcmd:u8][...] → route to GearControl

    // --- Diagnostics ---
    // LOG_MESSAGE moved to CorePacket::LOG_MESSAGE (0xFD) — universal across all boards

    // --- File Operations (0x9A-0xA3) ---
    constexpr uint8_t FILE_LIST          = 0x9A;  // [pathLen:u8][path:str] → STREAM_BEGIN + STREAM_DATA + STREAM_END
    constexpr uint8_t FILE_DELETE        = 0x9B;  // [pathLen:u8][path:str] → ACK/NACK
    constexpr uint8_t FILE_MKDIR         = 0x9C;  // [pathLen:u8][path:str] → ACK/NACK
    constexpr uint8_t FILE_INFO          = 0x9D;  // [pathLen:u8][path:str] → FILE_INFO_RESP
    constexpr uint8_t FILE_INFO_RESP     = 0x9E;  // [exists:u8][isDir:u8][size:u32LE]
    constexpr uint8_t FILE_DOWNLOAD      = 0x9F;  // [pathLen:u8][path:str] → STREAM_BEGIN + STREAM_DATA + STREAM_END
    constexpr uint8_t FILE_UPLOAD_BEGIN  = 0xA0;  // [size:u32LE][pathLen:u8][path:str] → ACK
    constexpr uint8_t FILE_UPLOAD_DATA   = 0xA1;  // [seqNum:u16LE][crc16:u16LE][data:N] → ACK/NACK(CRC_ERROR)
    constexpr uint8_t FILE_UPLOAD_END    = 0xA2;  // [] → ACK/NACK
    constexpr uint8_t FILE_UPLOAD_CANCEL = 0xA3;  // [] → ACK

    // Streaming packet types (STREAM_BEGIN/DATA/END) are defined in
    // StreamProtocol (serial_stream.h) — they are protocol infrastructure
    // reusable by any controller, not HubFX-specific.
}

// ============================================================================
// HubFX Error Codes (0x80-0x8F)
// ============================================================================

namespace HubFxError {
    using namespace SerialError;

    constexpr uint8_t SLAVE_NOT_FOUND      = 0x80;  // No slave of requested type
    constexpr uint8_t SLAVE_NOT_CONNECTED  = 0x81;  // Slave registered but not connected
    constexpr uint8_t SLAVE_INIT_FAILED    = 0x82;  // Slave INIT handshake failed
    constexpr uint8_t NO_SLAVES            = 0x83;  // No slaves registered
    constexpr uint8_t SLAVE_COMM_ERROR     = 0x84;  // Communication error with slave
    constexpr uint8_t AUDIO_ERROR          = 0x85;  // Audio system error (play/queue failed)
    constexpr uint8_t SD_NOT_INITIALIZED   = 0x86;  // SD card not initialized
    constexpr uint8_t ENGINE_NOT_AVAILABLE = 0x87;  // Engine FX not configured
    constexpr uint8_t CONFIG_ERROR         = 0x88;  // Config load/reload failed
    constexpr uint8_t INVALID_CHANNEL      = 0x89;  // Audio channel out of range

    // File operation errors (0x8A-0x8F)
    constexpr uint8_t FILE_NOT_FOUND       = 0x8A;  // File or directory not found
    constexpr uint8_t FILE_ALREADY_EXISTS  = 0x8B;  // Path exists but is wrong type
    constexpr uint8_t FILE_IO_ERROR        = 0x8C;  // SD card read/write error
    constexpr uint8_t FILE_TOO_LARGE       = 0x8D;  // File exceeds size limit
    constexpr uint8_t UPLOAD_IN_PROGRESS   = 0x8E;  // Another upload is active
    constexpr uint8_t NO_UPLOAD_ACTIVE     = 0x8F;  // No upload in progress

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case SLAVE_NOT_FOUND:      return "Slave not found";
            case SLAVE_NOT_CONNECTED:  return "Slave not connected";
            case SLAVE_INIT_FAILED:    return "Slave INIT failed";
            case NO_SLAVES:            return "No slaves registered";
            case SLAVE_COMM_ERROR:     return "Slave communication error";
            case AUDIO_ERROR:          return "Audio error";
            case SD_NOT_INITIALIZED:   return "SD card not initialized";
            case ENGINE_NOT_AVAILABLE: return "Engine FX not available";
            case CONFIG_ERROR:         return "Config error";
            case INVALID_CHANNEL:      return "Invalid audio channel";
            case FILE_NOT_FOUND:       return "File not found";
            case FILE_ALREADY_EXISTS:  return "Path already exists";
            case FILE_IO_ERROR:        return "File I/O error";
            case FILE_TOO_LARGE:       return "File too large";
            case UPLOAD_IN_PROGRESS:   return "Upload already in progress";
            case NO_UPLOAD_ACTIVE:     return "No upload active";
            default:
                return SerialError::getMessage(code);
        }
    }
}

// ============================================================================
// Audio Output Mapping (wire format)
// ============================================================================

namespace HubFxAudio {
    constexpr uint8_t OUTPUT_STEREO = 0;
    constexpr uint8_t OUTPUT_LEFT   = 1;
    constexpr uint8_t OUTPUT_RIGHT  = 2;

    constexpr uint8_t LOOP_NONE     = 0;  // Play once
    constexpr uint8_t LOOP_FINITE   = 1;  // Loop N times
    constexpr uint8_t LOOP_INFINITE = 2;  // Loop forever

    constexpr uint8_t QUEUE_FINISH_LOOP = 0;  // Wait for current loop to finish
    constexpr uint8_t QUEUE_STOP_NOW    = 1;  // Stop current immediately

    constexpr uint8_t CH_ALL        = 0xFF;  // All channels / master
    constexpr uint8_t MAX_CHANNELS  = 8;
}

#endif // HUBFX_PROTOCOL_H
