/*
 * storage_protocol.h — wire-format constants for the storage subsystem.
 *
 * Canonical home for the storage wire packets / error codes / enums.
 * Lives under `sfx_serial` (alongside `serial/ports.h`, `serial/roles.h`,
 * `serial/wire.h`) so Go / Python / Studio clients share one include
 * path with the C++ firmware.  Implementations of the SD / LittleFS /
 * upload pipeline + the `StorageServicePolicy<TPolicy>` that dispatches
 * the opcodes below live in `sfx_storage/`.
 *
 * Packet types: 0x93..0x95 (SD), 0x99 (Flash), 0x9A..0xA3 (file ops),
 *               0xA9 (file tree), 0xB0 (upload progress async).
 *
 * Storage commands accept an optional `[target:u8]` byte
 * (`StorageWire::TARGET_SD` / `TARGET_FLASH`) so a single command set
 * spans SD card + onboard flash.
 */

#ifndef SFX_STORAGE_PROTOCOL_H
#define SFX_STORAGE_PROTOCOL_H

#include <cstdint>

#include <serial/core/core.h>

// ============================================================================
// Storage packet types
// ============================================================================

namespace StoragePacket {
    // SD card management (0x93..0x95)
    constexpr uint8_t SD_INIT             = 0x93;  ///< [speed_mhz:u8] → ACK/NACK
    constexpr uint8_t SD_STATUS_REQ       = 0x94;
    constexpr uint8_t SD_STATUS_RESP      = 0x95;  ///< [initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE]
                                                   ///< [freeSpace_MB:u32LE][fatType:u8]
                                                   ///< extended (v0.5+): [cardType:u8][busMode:u8][usedSpace_MB:u32LE]

    // Flash management (0x99)
    constexpr uint8_t FLASH_STATUS_REQ    = 0x99;  ///< [] → FLASH_STATUS_RESP (same type as response)

    // File operations (0x9A..0xA3)
    // Most commands take optional [target:u8] at the end of the payload:
    //   0 = SD card (default if omitted), 1 = Flash.
    constexpr uint8_t FILE_LIST           = 0x9A;  ///< [pathLen:u8][path:str][target:u8?] → STREAM
    constexpr uint8_t FILE_DELETE         = 0x9B;  ///< [pathLen:u8][path:str][target:u8?][flags:u8?]
    constexpr uint8_t FILE_MKDIR          = 0x9C;  ///< [pathLen:u8][path:str][target:u8?][flags:u8?]
    constexpr uint8_t FILE_INFO           = 0x9D;
    constexpr uint8_t FILE_INFO_RESP      = 0x9E;  ///< [exists:u8][isDir:u8][size:u32LE]
    constexpr uint8_t FILE_DOWNLOAD       = 0x9F;  ///< → STREAM
    constexpr uint8_t FILE_UPLOAD_BEGIN   = 0xA0;
    constexpr uint8_t FILE_UPLOAD_DATA    = 0xA1;
    constexpr uint8_t FILE_UPLOAD_END     = 0xA2;
    constexpr uint8_t FILE_UPLOAD_CANCEL  = 0xA3;

    // File tree (0xA9)
    constexpr uint8_t FILE_TREE           = 0xA9;  ///< [pathLen:u8][path:str][target:u8?] → STREAM

    // Async upload progress (0xB0) — server-sent during STREAM uploads
    constexpr uint8_t FILE_UPLOAD_PROGRESS = 0xB0; ///< TAG_ASYNC: [segment_idx:u16LE][bytes_recv:u32LE][ring_fill_pct:u8]
}

// ============================================================================
// Storage wire-format enums + flags
// ============================================================================

namespace StorageWire {
    enum StorageTarget : uint8_t {
        TARGET_SD    = 0,
        TARGET_FLASH = 1,
    };

    enum UploadMode : uint8_t {
        UPLOAD_SYNC   = 0,  ///< per-chunk ACK + CRC retry (default)
        UPLOAD_STREAM = 3,  ///< raw binary stream + segment ACKs
    };

    namespace DeleteFlags {
        constexpr uint8_t NONE      = 0x00;
        constexpr uint8_t RECURSIVE = 0x01;
    }

    namespace MkdirFlags {
        constexpr uint8_t NONE    = 0x00;
        constexpr uint8_t PARENTS = 0x01;     ///< mkdir -p (idempotent)
    }
}

// ============================================================================
// Storage-layer error codes (0x86, 0x8A..0x8F)
// ============================================================================

namespace StorageError {
    using namespace SerialError;
    constexpr uint8_t SD_NOT_INITIALIZED   = 0x86;
    constexpr uint8_t FILE_NOT_FOUND       = 0x8A;
    constexpr uint8_t FILE_ALREADY_EXISTS  = 0x8B;
    constexpr uint8_t FILE_IO_ERROR        = 0x8C;
    constexpr uint8_t FILE_TOO_LARGE       = 0x8D;
    constexpr uint8_t UPLOAD_IN_PROGRESS   = 0x8E;
    constexpr uint8_t NO_UPLOAD_ACTIVE     = 0x8F;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case SD_NOT_INITIALIZED:   return "SD card not initialized";
            case FILE_NOT_FOUND:       return "File not found";
            case FILE_ALREADY_EXISTS:  return "Path already exists";
            case FILE_IO_ERROR:        return "File I/O error";
            case FILE_TOO_LARGE:       return "File too large";
            case UPLOAD_IN_PROGRESS:   return "Upload already in progress";
            case NO_UPLOAD_ACTIVE:     return "No upload active";
            default:                   return nullptr;
        }
    }
}

#endif  // SFX_STORAGE_PROTOCOL_H
