/*
 * Serial HubFX Protocol — Protocol Constants and Client Classes
 *
 * This header contains:
 *   - HubFxPacket:  Packet type constants (0x80-0xA8)
 *   - HubFxError:   Error code constants
 *   - HubFxAudio:   Audio wire format constants
 *   - HubFxStorage: Storage target constants
 *   - Data structs: Status/response structs for client-side parsing
 *   - HubFxAudioClient:   Client for sending audio commands
 *   - HubFxStorageClient:  Client for sending storage commands
 *
 * Server-side command handlers (HubFxAudioServer, HubFxStorageServer) are NOT
 * in this header — they live in the HubFX ESP32-S3 controller codebase because
 * they depend on platform-specific audio/storage backends (AudioMixer, SdFat,
 * LittleFS, etc.) that vary by target.
 *
 * NOTE: This header is NOT auto-included by serial.h — include explicitly:
 *   #include <serial/hubfx/hubfx.h>
 */

#ifndef SERIAL_HUBFX_H
#define SERIAL_HUBFX_H

#include <Arduino.h>
#include <functional>
#include "serial/core/core.h"
#include "serial/client/bus_client.h"
#include "serial/core/stream.h"


// ============================================================================
// HubFX Packet Types (0x80-0xA8)
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

    // --- Config Management (0x90-0x92) ---
    constexpr uint8_t CONFIG_RELOAD     = 0x90;  // [] → ACK/NACK
    constexpr uint8_t CONFIG_GET        = 0x91;  // [] → CONFIG_GET_RESP
    constexpr uint8_t CONFIG_GET_RESP   = 0x92;  // [loaded:u8][size:u16LE][reserved:u8]

    // --- SD Card Management (0x93-0x95) ---
    constexpr uint8_t SD_INIT           = 0x93;  // [speed_mhz:u8] → ACK/NACK
    constexpr uint8_t SD_STATUS_REQ     = 0x94;  // [] → SD_STATUS_RESP
    constexpr uint8_t SD_STATUS_RESP    = 0x95;  // [initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE][freeSpace_MB:u32LE][fatType:u8]

    // --- Slave Routing (0x96-0x98) — subcmd pattern ---
    constexpr uint8_t SLAVE_ROUTE_GUNFX       = 0x96;  // [subcmd:u8][...] → route to GunFX
    constexpr uint8_t SLAVE_ROUTE_LIGHTFX     = 0x97;  // [subcmd:u8][...] → route to LightFX
    constexpr uint8_t SLAVE_ROUTE_GEARCONTROL = 0x98;  // [subcmd:u8][...] → route to GearControl

    // --- Flash Management (0x99) ---
    constexpr uint8_t FLASH_STATUS_REQ  = 0x99;  // [] → FLASH_STATUS_RESP (same type as response)

    // --- USB Host Diagnostics (0xA7-0xA8) ---
    constexpr uint8_t USB_DEVICES_REQ    = 0xA7;  // [] → USB_DEVICES_RESP
    constexpr uint8_t USB_DEVICES_RESP   = 0xA8;  // [initialized:u8][taskRunning:u8][backendLen:u8][backend:str]
                                                   //   [deviceCount:u8] per-device: [addr:u8][vid:u16LE][pid:u16LE][state:u8][slaveType:u8]

    // --- File Operations (0x9A-0xA3) ---
    // File commands accept an optional [target:u8] at the end of the payload:
    //   0 = SD card (default if omitted, backward-compatible)
    //   1 = Flash (onboard LittleFS)
    constexpr uint8_t FILE_LIST          = 0x9A;  // [pathLen:u8][path:str][target:u8?] → STREAM_BEGIN + STREAM_DATA + STREAM_END
    constexpr uint8_t FILE_DELETE        = 0x9B;  // [pathLen:u8][path:str][target:u8?] → ACK/NACK
    constexpr uint8_t FILE_MKDIR         = 0x9C;  // [pathLen:u8][path:str][target:u8?] → ACK/NACK
    constexpr uint8_t FILE_INFO          = 0x9D;  // [pathLen:u8][path:str][target:u8?] → FILE_INFO_RESP
    constexpr uint8_t FILE_INFO_RESP     = 0x9E;  // [exists:u8][isDir:u8][size:u32LE]
    constexpr uint8_t FILE_DOWNLOAD      = 0x9F;  // [pathLen:u8][path:str][target:u8?] → STREAM_BEGIN + STREAM_DATA + STREAM_END
    constexpr uint8_t FILE_UPLOAD_BEGIN  = 0xA0;  // [size:u32LE][pathLen:u8][path:str][target:u8?] → ACK
    constexpr uint8_t FILE_UPLOAD_DATA   = 0xA1;  // [seqNum:u16LE][crc16:u16LE][data:N] → ACK/NACK(CRC_ERROR)
    constexpr uint8_t FILE_UPLOAD_END    = 0xA2;  // [] → ACK/NACK
    constexpr uint8_t FILE_UPLOAD_CANCEL = 0xA3;  // [] → ACK
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


// ============================================================================
// Storage Target Constants
// ============================================================================

namespace HubFxStorage {
    constexpr uint8_t TARGET_SD    = 0;  // SD card (default)
    constexpr uint8_t TARGET_FLASH = 1;  // Onboard LittleFS flash
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
    uint8_t output       = 0;      // HubFxAudio::OUTPUT_*
    uint16_t wavRate_Hz  = 0;
    uint8_t wavChannels  = 0;
    uint8_t wavBits      = 0;
    char filename[64]    = {};
};

/// Full audio mixer status (parsed from AUDIO_STATUS_RESP)
struct HubFxAudioStatus {
    uint8_t masterVolPct  = 0;     // 0-100
    bool initialized      = false;
    bool i2sRunning       = false;
    bool hasCodec         = false;
    bool hasRingStats     = false;  // v3 flag
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

/// Config info (parsed from CONFIG_GET_RESP)
struct HubFxConfigInfo {
    bool loaded          = false;
    uint16_t fileSize    = 0;
};


// ============================================================================
// Client Callback Types
// ============================================================================

using HubFxAudioStatusCallback  = std::function<void(const HubFxAudioStatus& status)>;
using HubFxSdStatusCallback     = std::function<void(const HubFxSdStatus& status)>;
using HubFxFlashStatusCallback  = std::function<void(const HubFxFlashStatus& status)>;
using HubFxFileInfoCallback     = std::function<void(const HubFxFileInfo& info)>;
using HubFxConfigInfoCallback   = std::function<void(const HubFxConfigInfo& info)>;


// ============================================================================
// HubFxAudioClient — Client for Audio Commands
// ============================================================================

/**
 * @brief Client-side audio serial communication for HubFX.
 *
 * Used by any controller or app that sends audio playback commands
 * to a HubFX hub over USB. Extends BusClient with audio-specific
 * command methods and AUDIO_STATUS_RESP parsing.
 */
class HubFxAudioClient : public BusClient {
public:
    // ========================================================================
    // Playback Control
    // ========================================================================

    CommandResult play(uint8_t channel, const char* path, uint8_t volumePct = 100,
                       uint8_t output = HubFxAudio::OUTPUT_STEREO,
                       uint8_t loopMode = HubFxAudio::LOOP_NONE,
                       uint16_t loopCount = 0);
    CommandResult stop(uint8_t channel = HubFxAudio::CH_ALL);
    CommandResult fade(uint8_t channel);

    // ========================================================================
    // Volume Control
    // ========================================================================

    CommandResult setVolume(uint8_t channel, uint8_t volumePct);
    CommandResult setMasterVolume(uint8_t volumePct);

    // ========================================================================
    // Queue Control
    // ========================================================================

    CommandResult queueSound(uint8_t channel, const char* path, uint8_t volumePct = 100,
                             uint16_t loopCount = 0,
                             uint8_t behavior = HubFxAudio::QUEUE_FINISH_LOOP);
    CommandResult queueClear(uint8_t channel = HubFxAudio::CH_ALL);

    // ========================================================================
    // Status
    // ========================================================================

    CommandResult requestStatus();

    // ========================================================================
    // Callbacks
    // ========================================================================

    void onAudioStatus(HubFxAudioStatusCallback cb) { _statusCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================

    const HubFxAudioStatus& lastStatus() const { return _lastStatus; }

protected:
    void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override;
    const char* getModuleErrorMessage(uint8_t code) override { return HubFxError::getMessage(code); }

private:
    HubFxAudioStatus _lastStatus;
    HubFxAudioStatusCallback _statusCallback;
};


// ============================================================================
// HubFxStorageClient — Client for Storage Commands
// ============================================================================

/**
 * @brief Client-side storage serial communication for HubFX.
 *
 * Used by any controller or app that sends config, SD, flash,
 * and file commands to a HubFX hub over USB. Extends BusClient
 * with storage-specific command methods and response parsing.
 *
 * For streaming responses (FILE_LIST, FILE_DOWNLOAD), the client
 * sends the command and the caller must handle STREAM_BEGIN/DATA/END
 * packets separately.
 */
class HubFxStorageClient : public BusClient {
public:
    // ========================================================================
    // Config Commands
    // ========================================================================

    CommandResult configReload();
    CommandResult configGet();

    // ========================================================================
    // SD Card Commands
    // ========================================================================

    CommandResult sdInit(uint8_t speed_mhz = 20);
    CommandResult sdStatus();

    // ========================================================================
    // Flash Commands
    // ========================================================================

    CommandResult flashStatus();

    // ========================================================================
    // File Operations (target: TARGET_SD=0, TARGET_FLASH=1)
    // ========================================================================

    /// Starts streamed listing — handle STREAM_BEGIN/DATA/END via callbacks
    CommandResult fileList(const char* path, uint8_t target = HubFxStorage::TARGET_SD);
    CommandResult fileDelete(const char* path, uint8_t target = HubFxStorage::TARGET_SD);
    CommandResult fileMkdir(const char* path, uint8_t target = HubFxStorage::TARGET_SD);
    CommandResult fileInfo(const char* path, uint8_t target = HubFxStorage::TARGET_SD);

    /// Starts streamed download — handle STREAM_BEGIN/DATA/END via callbacks
    CommandResult fileDownload(const char* path, uint8_t target = HubFxStorage::TARGET_SD);

    // ========================================================================
    // Upload Commands
    // ========================================================================

    CommandResult uploadBegin(const char* path, uint32_t size,
                              uint8_t target = HubFxStorage::TARGET_SD);
    CommandResult uploadData(uint16_t seqNum, const uint8_t* data, size_t dataLen);
    CommandResult uploadEnd();
    CommandResult uploadCancel();

    // ========================================================================
    // Callbacks
    // ========================================================================

    void onSdStatus(HubFxSdStatusCallback cb)      { _sdStatusCb = cb; }
    void onFlashStatus(HubFxFlashStatusCallback cb) { _flashStatusCb = cb; }
    void onFileInfo(HubFxFileInfoCallback cb)       { _fileInfoCb = cb; }
    void onConfigInfo(HubFxConfigInfoCallback cb)   { _configInfoCb = cb; }

    // ========================================================================
    // State
    // ========================================================================

    const HubFxSdStatus& lastSdStatus() const       { return _lastSdStatus; }
    const HubFxFlashStatus& lastFlashStatus() const  { return _lastFlashStatus; }
    const HubFxFileInfo& lastFileInfo() const        { return _lastFileInfo; }
    const HubFxConfigInfo& lastConfigInfo() const    { return _lastConfigInfo; }

protected:
    void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override;
    const char* getModuleErrorMessage(uint8_t code) override { return HubFxError::getMessage(code); }

private:
    HubFxSdStatus _lastSdStatus;
    HubFxFlashStatus _lastFlashStatus;
    HubFxFileInfo _lastFileInfo;
    HubFxConfigInfo _lastConfigInfo;

    HubFxSdStatusCallback _sdStatusCb;
    HubFxFlashStatusCallback _flashStatusCb;
    HubFxFileInfoCallback _fileInfoCb;
    HubFxConfigInfoCallback _configInfoCb;
};


#endif // SERIAL_HUBFX_H
