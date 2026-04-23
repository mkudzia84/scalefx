/*
 * Serial HubFX Protocol — Protocol Constants and Data Types
 *
 * This header contains:
 *   - HubFxPacket:  Packet type constants (0x80-0xA9)
 *   - HubFxError:   Error code constants
 *   - HubFxAudio:   Audio wire format constants
 *   - HubFxStorage: Storage target constants
 *   - Data structs: Status/response structs for client-side parsing
 *   - Callback types: Client callback typedefs
 *
 * Client classes live in their domain libraries:
 *   - HubFxAudioClient:   sfx_audio/client/audio_client.h
 *   - HubFxStorageClient:  sfx_storage/client/storage_client.h
 *
 * Server classes live in their domain libraries:
 *   - AudioServerT<TMixer>:        sfx_audio/server/audio_server.h
 *   - StorageServerT<TPolicy>:     sfx_storage/server/storage_server.h
 *
 * NOTE: This header is NOT auto-included by serial.h — include explicitly:
 *   #include <serial/hubfx/hubfx.h>
 */

#ifndef SERIAL_HUBFX_H
#define SERIAL_HUBFX_H

#include <Arduino.h>
#include <functional>
#include "serial/core/core.h"
#include "serial/core/stream.h"


// ============================================================================
// HubFX Packet Types (0x80-0xAF)
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
    constexpr uint8_t AUDIO_STATUS_RESP = 0x8B;  // v3: see HubFxAudioServer::handleStatusReq()

    // --- Engine FX Control (0x8C-0x8F) ---
    constexpr uint8_t ENGINE_START       = 0x8C;  // [] → ACK
    constexpr uint8_t ENGINE_STOP        = 0x8D;  // [] → ACK
    constexpr uint8_t ENGINE_STATUS_REQ  = 0x8E;  // [] → ENGINE_STATUS_RESP
    constexpr uint8_t ENGINE_STATUS_RESP = 0x8F;  // [state:u8][toggleEngaged:u8][active:u8]

    // --- Config Management (0x90-0x92, 0xAC) ---
    constexpr uint8_t CONFIG_RELOAD      = 0x90;  // [] or [pathLen:u8][path:str] → ACK/NACK
    constexpr uint8_t CONFIG_STATUS      = 0x91;  // [] → CONFIG_STATUS_RESP
    constexpr uint8_t CONFIG_STATUS_RESP = 0x92;  // [loaded:u8][size:u16LE][validOk:u8]
    constexpr uint8_t CONFIG_SAVE       = 0xAC;  // [] or [pathLen:u8][path:str] → ACK/NACK

    // --- SD Card Management (0x93-0x95) ---
    constexpr uint8_t SD_INIT           = 0x93;  // [speed_mhz:u8] → ACK/NACK (remounts card)
    constexpr uint8_t SD_STATUS_REQ     = 0x94;  // [] → SD_STATUS_RESP
    constexpr uint8_t SD_STATUS_RESP    = 0x95;  // [initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE][freeSpace_MB:u32LE][fatType:u8]
                                                 //   extended (v0.5+): [cardType:u8][busMode:u8][usedSpace_MB:u32LE]

    // --- Flash Management (0x99) ---
    constexpr uint8_t FLASH_STATUS_REQ  = 0x99;  // [] → FLASH_STATUS_RESP (same type as response)

    // --- USB Host Diagnostics (0xA7-0xA8, 0xAD) ---
    constexpr uint8_t USB_DEVICES_REQ    = 0xA7;  // [] → USB_DEVICES_RESP
    constexpr uint8_t USB_DEVICES_RESP   = 0xA8;  // [initialized:u8][taskRunning:u8][backendLen:u8][backend:str]
                                                   //   [deviceCount:u8] per-device: [addr:u8][vid:u16LE][pid:u16LE][state:u8][slaveType:u8]

    // --- Codec Status (0xAA-0xAB) ---
    constexpr uint8_t CODEC_STATUS_REQ   = 0xAA;  // [] → CODEC_STATUS_RESP
    constexpr uint8_t CODEC_STATUS_RESP  = 0xAB;  // [codecType:u8][initialized:u8][i2cOk:u8][sdaPin:u8][sclPin:u8]
                                                   //   [supplyVoltage:u8][muted:u8][digitalVol:u8][deviceCtrl:u8][faultStatus:u8]
                                                   //   [codecNameLen:u8][codecName:str]

    // --- USB Bus Recovery (0xAD) ---
    constexpr uint8_t USB_RESET_BUS      = 0xAD;  // [] → ACK (power-cycles root port, re-enumerates)

    // --- Slave Info Query (0xAE-0xAF) ---
    constexpr uint8_t SLAVE_INFO          = 0xAE;  // [slaveType:u8] → SLAVE_INFO_RESP (cached boardInfo, no slave query)
    constexpr uint8_t SLAVE_INFO_RESP     = 0xAF;  // [slaveType:u8][ready:u8][connected:u8][nameLen:u8][name:str]
                                                    //   [verLen:u8][ver:str][platLen:u8][plat:str]
                                                    //   [cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]

    // --- File Tree (0xA9) ---
    constexpr uint8_t FILE_TREE          = 0xA9;  // [pathLen:u8][path:str][target:u8?] → STREAM_BEGIN + STREAM_DATA + STREAM_END
                                                   //   Streamed text: "<depth> <d|f> <name> <size>\n" per entry (recursive)

    // --- File Operations (0x9A-0xA3) ---
    // File commands accept an optional [target:u8] at the end of the payload:
    //   0 = SD card (default if omitted, backward-compatible)
    //   1 = Flash (onboard LittleFS)
    constexpr uint8_t FILE_LIST          = 0x9A;  // [pathLen:u8][path:str][target:u8?] → STREAM_BEGIN + STREAM_DATA + STREAM_END
    constexpr uint8_t FILE_DELETE        = 0x9B;  // [pathLen:u8][path:str][target:u8?][flags:u8?] → ACK/NACK
                                                   //   flags bit 0 = RECURSIVE (delete non-empty directories).
                                                   //   Legacy default (flags byte absent) = RECURSIVE for back-compat.
    constexpr uint8_t FILE_MKDIR         = 0x9C;  // [pathLen:u8][path:str][target:u8?][flags:u8?] → ACK/NACK
                                                   //   flags bit 0 = PARENTS (mkdir -p; create missing ancestors, idempotent).
    constexpr uint8_t FILE_INFO          = 0x9D;  // [pathLen:u8][path:str][target:u8?] → FILE_INFO_RESP
    constexpr uint8_t FILE_INFO_RESP     = 0x9E;  // [exists:u8][isDir:u8][size:u32LE]
    constexpr uint8_t FILE_DOWNLOAD      = 0x9F;  // [pathLen:u8][path:str][target:u8?] → STREAM_BEGIN + STREAM_DATA + STREAM_END
    constexpr uint8_t FILE_UPLOAD_BEGIN  = 0xA0;  // [size:u32LE][pathLen:u8][path:str][target:u8?][mode:u8?] → ACK
                                                   //   mode: 0=sync (COBS per-chunk ACK), 3=stream (raw binary, segment ACKs)
                                                   //   Stream ACK payload: [segment_size:u32LE][segment_count:u16LE]
    constexpr uint8_t FILE_UPLOAD_DATA   = 0xA1;  // [seqNum:u16LE][crc16:u16LE][data:N] → ACK/NACK (sync mode only)
    constexpr uint8_t FILE_UPLOAD_END    = 0xA2;  // [] → ACK[md5:16B] / NACK
    constexpr uint8_t FILE_UPLOAD_CANCEL = 0xA3;  // [] → ACK

    // --- Upload Progress (0xB0) — server-sent segment ACK during STREAM upload ---
    constexpr uint8_t FILE_UPLOAD_PROGRESS = 0xB0;  // Server → Client (async, TAG_ASYNC)
                                                    //   [segment_idx:u16LE][bytes_received:u32LE][ring_fill_pct:u8]

    // --- Slave Registry Enumeration (0xB1-0xB2) ---
    // Clients query SLAVE_ENUM_REQ to learn which slave controller types are
    // currently attached to the hub. Slave-range packets (GunFX 0x01-0x2F,
    // LightFX 0x40-0x5F, GearControl 0x60-0x7F) sent to the hub are auto-
    // routed to the matching attached slave by packet-type range — no
    // envelope wrapping is needed. Responses (ACK/NACK or typed RESP) are
    // forwarded back upstream verbatim with the original correlation tag.
    // Async slave packets (STATUS broadcasts in slave ranges) are forwarded
    // verbatim with TAG_ASYNC. See instructions/13-PASSTHROUGH-ROUTING.md.
    constexpr uint8_t SLAVE_ENUM_REQ      = 0xB1;  // [] → SLAVE_ENUM_RESP
    constexpr uint8_t SLAVE_ENUM_RESP     = 0xB2;  // [count:u8] then per slot:
                                                    //   [slot:u8][type:u8][connected:u8][ready:u8]
                                                    //   [nameLen:u8][name:str]
                                                    // Empty slots emit type=Unknown, nameLen=0.
}


// ============================================================================
// HubFX Error Codes (0x80-0x8F)
// ============================================================================

namespace HubFxError {
    using namespace SerialError;

    constexpr uint8_t SLAVE_NOT_FOUND      = 0x80;
    constexpr uint8_t SLAVE_NOT_CONNECTED  = 0x81;
    constexpr uint8_t SLAVE_INIT_FAILED    = 0x82;
    constexpr uint8_t NO_SLAVES            = 0x83;
    constexpr uint8_t SLAVE_COMM_ERROR     = 0x84;
    constexpr uint8_t AUDIO_ERROR          = 0x85;
    constexpr uint8_t SD_NOT_INITIALIZED   = 0x86;
    constexpr uint8_t ENGINE_NOT_AVAILABLE = 0x87;
    constexpr uint8_t CONFIG_ERROR         = 0x88;
    constexpr uint8_t INVALID_CHANNEL      = 0x89;

    // File operation errors (0x8A-0x8F)
    constexpr uint8_t FILE_NOT_FOUND       = 0x8A;
    constexpr uint8_t FILE_ALREADY_EXISTS  = 0x8B;
    constexpr uint8_t FILE_IO_ERROR        = 0x8C;
    constexpr uint8_t FILE_TOO_LARGE       = 0x8D;
    constexpr uint8_t UPLOAD_IN_PROGRESS   = 0x8E;
    constexpr uint8_t NO_UPLOAD_ACTIVE     = 0x8F;

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
// Audio Wire Format Constants
// ============================================================================

namespace HubFxAudio {
    // Output channel bitmask (matches AudioChannel:: constants)
    constexpr uint8_t OUTPUT_CH1 = 0x01;  // Channel 1 only
    constexpr uint8_t OUTPUT_CH2 = 0x02;  // Channel 2 only
    constexpr uint8_t OUTPUT_ALL = OUTPUT_CH1 | OUTPUT_CH2;  // All channels (default)

    constexpr uint8_t LOOP_NONE     = 0;  // Play once
    constexpr uint8_t LOOP_FINITE   = 1;  // Loop N times
    constexpr uint8_t LOOP_INFINITE = 2;  // Loop forever

    constexpr uint8_t QUEUE_FINISH_LOOP = 0;  // Wait for current loop to finish
    constexpr uint8_t QUEUE_STOP_NOW    = 1;  // Stop current immediately

    constexpr uint8_t CH_ALL        = 0xFF;  // All channels / master
    constexpr uint8_t MAX_CHANNELS  = 8;
}


// ============================================================================
// Storage Target & Upload Mode Enums
// ============================================================================

namespace HubFxStorage {
    /// Storage target (SD card or onboard LittleFS flash)
    enum StorageTarget : uint8_t {
        TARGET_SD    = 0,  // SD card (default)
        TARGET_FLASH = 1   // Onboard LittleFS flash
    };

    /// Upload transfer mode
    enum UploadMode : uint8_t {
        UPLOAD_SYNC     = 0,  // ACK per chunk, CRC retry (default)
        UPLOAD_STREAM   = 3   // Raw binary stream: bypass COBS for data, segment-based ACKs
    };

    /// FILE_DELETE flag bits (optional trailing byte)
    namespace DeleteFlags {
        constexpr uint8_t NONE      = 0x00;
        constexpr uint8_t RECURSIVE = 0x01;  // Recursively delete non-empty directories
    }

    /// FILE_MKDIR flag bits (optional trailing byte)
    namespace MkdirFlags {
        constexpr uint8_t NONE    = 0x00;
        constexpr uint8_t PARENTS = 0x01;  // Create missing parent directories (mkdir -p); idempotent
    }
}


// ============================================================================
// Client Status Data Structures (parsed from response payloads)
// ============================================================================

/// Per-channel audio status (parsed from AUDIO_STATUS_RESP)
struct HubFxAudioChannelInfo {
    uint8_t channel      = 0;
    uint8_t volumePct    = 0;      // 0-100
    bool playing         = false;
    bool looping         = false;
    uint16_t loopCount   = 0;
    uint32_t remaining_ms = 0;
    uint8_t queueLen     = 0;
    uint8_t output       = 0;      // HubFxAudio::OUTPUT_CH* bitmask
    uint16_t wavRate_Hz  = 0;
    uint8_t wavChannels  = 0;
    uint8_t wavBits      = 0;
    uint8_t wavBufFillPct = 0;     // v4: WAV decode buffer fill %
    char filename[64]    = {};
};

/// Full audio mixer status (parsed from AUDIO_STATUS_RESP)
struct HubFxAudioStatus {
    uint8_t masterVolPct  = 0;     // 0-100
    bool initialized      = false;
    bool i2sRunning       = false;
    bool hasCodec         = false;
    bool hasRingStats     = false;  // v3 flag
    bool hasBufferCaps    = false;  // v4 flag
    uint16_t sampleRate_Hz = 0;
    uint8_t bitDepth      = 0;
    uint8_t maxChannels   = 0;
    char codecName[32]    = {};

    // Ring buffer stats (v3)
    uint8_t ringFillPct      = 0;
    uint16_t ringAvailRead   = 0;
    uint16_t ringAvailWrite  = 0;
    uint32_t underruns       = 0;
    uint32_t consumeLoops    = 0;
    uint32_t consumeFrames   = 0;

    // Buffer capacities (v4)
    uint16_t wavBufCapacity  = 0;  // WAV_BUF_FRAMES
    uint16_t ringCapacity    = 0;  // RING_FRAMES

    // Per-channel
    uint8_t activeMask    = 0;
    HubFxAudioChannelInfo channels[HubFxAudio::MAX_CHANNELS];
    uint8_t activeCount   = 0;
};

/// SD card status (parsed from SD_STATUS_RESP)
struct HubFxSdStatus {
    bool initialized     = false;
    uint32_t cardSize_MB   = 0;
    uint32_t totalSpace_MB = 0;
    uint32_t freeSpace_MB  = 0;
    uint8_t fatType        = 0;
    // Extended fields (v0.5+)
    uint8_t cardType       = 0;   ///< 0=NONE,1=MMC,2=SD,3=SDHC,4=UNKNOWN
    uint8_t busMode        = 0;   ///< 0=SPI,1=SDIO_1BIT,2=SDIO_4BIT
    uint32_t usedSpace_MB  = 0;
};

/// Flash status (parsed from FLASH_STATUS_REQ response)
struct HubFxFlashStatus {
    bool initialized     = false;
    uint32_t totalBytes  = 0;
    uint32_t usedBytes   = 0;
    uint32_t freeBytes   = 0;
};

/// File info (parsed from FILE_INFO_RESP)
struct HubFxFileInfo {
    bool exists          = false;
    bool isDirectory     = false;
    uint32_t size        = 0;
};

/// Config info (parsed from CONFIG_STATUS_RESP)
struct HubFxConfigInfo {
    bool loaded          = false;
    uint16_t fileSize    = 0;
    bool validOk         = false;
};


// ============================================================================
// Client Callback Types
// ============================================================================

using HubFxAudioStatusCallback  = std::function<void(const HubFxAudioStatus& status)>;
using HubFxSdStatusCallback     = std::function<void(const HubFxSdStatus& status)>;
using HubFxFlashStatusCallback  = std::function<void(const HubFxFlashStatus& status)>;
using HubFxFileInfoCallback     = std::function<void(const HubFxFileInfo& info)>;
using HubFxConfigInfoCallback   = std::function<void(const HubFxConfigInfo& info)>;


#endif // SERIAL_HUBFX_H
