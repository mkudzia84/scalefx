/*
 * Storage Server — Command Handler for Config, SD Card, and File Transfer
 *
 * Handles storage-related packets (0x90-0xA3):
 *   - Config reload, config get              (0x90-0x92)
 *   - SD card init, SD card status           (0x93-0x95)
 *   - File list, delete, mkdir, info         (0x9A-0x9E)
 *   - File download (streamed)               (0x9F)
 *   - File upload (chunked with CRC)         (0xA0-0xA3)
 *
 * Streaming responses (0xA4-0xA6) are sent via StreamWriter using
 * protocol-level types from serial_stream.h, not the module range.
 *
 * File transfers larger than MAX_PAYLOAD_SIZE use the StreamWriter
 * (serial_stream.h) for downloads and POSIX-style listing output.
 * Uploads use per-chunk CRC-16 with ACK/NACK for client retry.
 *
 * Note: Packets 0x96-0x99 are handled by SlaveServer and DiagLog
 * earlier in the CommandRouter chain, so they never reach this handler.
 */

#ifndef STORAGE_SERVER_H
#define STORAGE_SERVER_H

#include <Arduino.h>
#include <SdFat.h>
#include <serial_bus_server.h>
#include <serial_stream.h>

#include "../board_manager/hubfx_protocol.h"
#include "sd_card.h"

// Forward declarations
class ConfigReader;

// ============================================================================
// StorageServer — ICommandHandler for Config + SD Card + File Transfer
// ============================================================================

class StorageServer : public BusServer {
public:
    StorageServer() = default;

    const char* handlerName() const override { return "StorageServer"; }

    void setConfigReader(ConfigReader* config) { _config = config; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override  { return 0x90; }
    uint8_t moduleRangeHigh() const override { return 0xA3; }
    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

private:
    // --- Config handlers ---
    void handleConfigReload();
    void handleConfigGet();

    // --- SD card handlers ---
    void handleSdInit(const uint8_t* payload, size_t len);
    void handleSdStatusReq();

    // --- File operations ---
    void handleFileList(const uint8_t* payload, size_t len);
    void handleFileDelete(const uint8_t* payload, size_t len);
    void handleFileMkdir(const uint8_t* payload, size_t len);
    void handleFileInfo(const uint8_t* payload, size_t len);
    void handleFileDownload(const uint8_t* payload, size_t len);

    // --- Upload handlers ---
    void handleUploadBegin(const uint8_t* payload, size_t len);
    void handleUploadData(const uint8_t* payload, size_t len);
    void handleUploadEnd();
    void handleUploadCancel();

    // --- Helpers ---
    /// Map SdError code to HubFxError code
    uint8_t mapSdError(uint8_t sdErr);

    /// Extract length-prefixed path from payload: [pathLen:u8][path:str]
    bool extractPath(const uint8_t* payload, size_t len,
                     char* pathBuf, size_t pathBufSize);

    // --- Singleton accessor ---
    SdCardModule& sd() { return SdCardModule::instance(); }

    // --- Dependencies ---
    ConfigReader* _config = nullptr;

    // --- Upload state machine ---
    struct {
        bool active       = false;
        File32 file;
        char path[128]    = {};
        uint32_t expectedSize  = 0;
        uint32_t bytesReceived = 0;
        uint16_t expectedSeq   = 0;
    } _upload;
};

#endif // STORAGE_SERVER_H
