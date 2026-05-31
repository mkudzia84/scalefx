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

    // Upload diagnostics (0xA4/0xA5) — post-mortem of the last (or active)
    // upload.  Query any time AFTER the raw-stream phase (the firmware is in
    // COBS mode again) to see why a transfer stalled: SD write latencies, loop
    // gap, segment progress, abort reason.  The stats survive cleanupUpload()
    // until the next FILE_UPLOAD_BEGIN, so a client that hit a segment-ACK
    // timeout can pull the smoking gun (e.g. a single 16 KB SD write that took
    // 30 s on a flaky card) it could never see live (stream mode can't emit
    // COBS log packets — they only reach the native USB-JTAG console).
    constexpr uint8_t FILE_UPLOAD_DIAG_REQ  = 0xA4;  ///< [] → FILE_UPLOAD_DIAG_RESP
    constexpr uint8_t FILE_UPLOAD_DIAG_RESP = 0xA5;
    ///< [bytesRecv:u32LE][expectedSize:u32LE][segIndex:u16LE][segCount:u16LE]
    ///< [fillPct:u8][sdWriteCount:u32LE][sdBytesWritten:u32LE][sdMaxLat_ms:u32LE]
    ///< [sdTotalStall_ms:u32LE][maxLoopGap_ms:u32LE][flags:u8][abortReason:u8]
    ///< (flags bit0=uploadActive bit1=streamActive) — 35 bytes

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

    // Why the most-recent upload ended — reported in FILE_UPLOAD_DIAG_RESP's
    // abortReason byte.  ACTIVE/COMPLETED are the healthy states; the rest tell
    // a stalled-upload post-mortem apart (a >30 s SD write self-evident from
    // sdMaxLat_ms; an INACTIVITY abort means the client genuinely went silent).
    enum UploadEndReason : uint8_t {
        REASON_NONE        = 0,  ///< no upload has run since boot
        REASON_ACTIVE      = 1,  ///< an upload is in progress right now
        REASON_COMPLETED   = 2,  ///< finished + MD5-verified OK
        REASON_INACTIVITY  = 3,  ///< checkUploadTimeout() fired (client silent)
        REASON_FLUSH_FAIL  = 4,  ///< SD/flash write returned short/error
        REASON_HEALTH      = 5,  ///< policy health check aborted the stream
        REASON_CLIENT_CANCEL = 6,///< FILE_UPLOAD_CANCEL received
        REASON_STALE_RESET = 7,  ///< superseded by a fresh UPLOAD_BEGIN
    };
}

// ============================================================================
// Storage-layer error codes (0x86, 0x8A..0x8F)
// ============================================================================

// Storage error codes — CLAUDE.md allocates 0xA0..0xAF.  Old values
// (0x86, 0x8A-0x8F) squatted in expander/storage PACKET-TYPE bytes;
// the rename to 0xA0-0xA6 puts them squarely in error-range territory.
namespace StorageError {
    using namespace SerialError;
    constexpr uint8_t SD_NOT_INITIALIZED   = 0xA0;
    constexpr uint8_t FILE_NOT_FOUND       = 0xA1;
    constexpr uint8_t FILE_ALREADY_EXISTS  = 0xA2;
    constexpr uint8_t FILE_IO_ERROR        = 0xA3;
    constexpr uint8_t FILE_TOO_LARGE       = 0xA4;
    constexpr uint8_t UPLOAD_IN_PROGRESS   = 0xA5;
    constexpr uint8_t NO_UPLOAD_ACTIVE     = 0xA6;

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
