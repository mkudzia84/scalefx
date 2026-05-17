/*
 * HubFX Storage Client — Client-Side Storage Serial Communication
 *
 * Used by any controller or app that sends config, SD, flash,
 * and file commands to a HubFX hub over USB. Extends BusClient
 * with storage-specific command methods and response parsing.
 *
 * For streaming responses (FILE_LIST, FILE_DOWNLOAD), the client
 * sends the command and the caller must handle STREAM_BEGIN/DATA/END
 * packets separately.
 *
 * Shared library component (controllers/lib/sfx_storage/).
 * Depends on: sfx_serial (BusClient, HubFxPacket, HubFxStorage, HubFxError).
 */

#ifndef STORAGE_CLIENT_H
#define STORAGE_CLIENT_H

#include <protocol/storage_protocol.h>
#include <serial/client/bus_client.h>

// ============================================================================
// HubFxStorageClient — Client for Storage Commands
// ============================================================================

class HubFxStorageClient : public BusClient {
public:
    // ========================================================================
    // Config Commands
    // ========================================================================

    CommandResult configReload();
    CommandResult configStatus();

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
    CommandResult fileList(const char* path, StorageWire::StorageTarget target = StorageWire::TARGET_SD);
    CommandResult fileDelete(const char* path, StorageWire::StorageTarget target = StorageWire::TARGET_SD);
    CommandResult fileMkdir(const char* path, StorageWire::StorageTarget target = StorageWire::TARGET_SD);
    CommandResult fileInfo(const char* path, StorageWire::StorageTarget target = StorageWire::TARGET_SD);

    /// Starts streamed download — handle STREAM_BEGIN/DATA/END via callbacks
    CommandResult fileDownload(const char* path, StorageWire::StorageTarget target = StorageWire::TARGET_SD);

    // ========================================================================
    // Upload Commands
    // ========================================================================

    CommandResult uploadBegin(const char* path, uint32_t size,
                              StorageWire::StorageTarget target = StorageWire::TARGET_SD);
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
    const char* getModuleErrorMessage(uint8_t code) override { return StorageError::getMessage(code); }

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

#endif // STORAGE_CLIENT_H
