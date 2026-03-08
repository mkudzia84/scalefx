/*
 * Storage Server â€” Implementation
 *
 * Handles config + SD card + file transfer commands (0x90-0xA3).
 *
 * Two transfer modes:
 *   DOWNLOAD (serverâ†’client): StreamWriter sends STREAM_BEGIN/DATA/END
 *     (0xA4-0xA6) from serial_stream.h. Fire-and-forget with end-of-stream
 *     CRC-16. Used by FILE_LIST and FILE_DOWNLOAD.
 *   UPLOAD (clientâ†’server): Client sends FILE_UPLOAD_BEGIN/DATA/END/CANCEL
 *     (0xA0-0xA3). Each chunk gets individual ACK/NACK, enabling per-chunk
 *     CRC-16 retry. Upload reuses StreamProtocol::CHUNK_HEADER_SIZE and
 *     StreamProtocol::crc16() for the shared [seqNum:u16LE][crc16:u16LE] format.
 */

#include "storage_server.h"
#include "config_reader.h"

using namespace CoreProtocol;


// ============================================================================
// handleModulePacket â€” Storage Commands (0x90-0xA6)
// ============================================================================

CommandHandleResult StorageServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        // --- Config (0x90-0x92) ---
        case HubFxPacket::CONFIG_RELOAD:
            handleConfigReload();
            return CommandHandleResult::Handled;

        case HubFxPacket::CONFIG_GET:
            handleConfigGet();
            return CommandHandleResult::Handled;

        // --- SD card (0x93-0x95) ---
        case HubFxPacket::SD_INIT:
            handleSdInit(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::SD_STATUS_REQ:
            handleSdStatusReq();
            return CommandHandleResult::Handled;

        // --- File operations (0x9A-0x9F) ---
        case HubFxPacket::FILE_LIST:
            handleFileList(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_DELETE:
            handleFileDelete(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_MKDIR:
            handleFileMkdir(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_INFO:
            handleFileInfo(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_DOWNLOAD:
            handleFileDownload(payload, len);
            return CommandHandleResult::Handled;

        // --- Upload (0xA0-0xA3) ---
        case HubFxPacket::FILE_UPLOAD_BEGIN:
            handleUploadBegin(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_UPLOAD_DATA:
            handleUploadData(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_UPLOAD_END:
            handleUploadEnd();
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_UPLOAD_CANCEL:
            handleUploadCancel();
            return CommandHandleResult::Handled;

        default:
            return CommandHandleResult::NotMyCommand;
    }
}


// ============================================================================
// Config Handlers
// ============================================================================

void StorageServer::handleConfigReload() {
    if (!_config) {
        sendNack(HubFxError::CONFIG_ERROR);
        return;
    }

    if (_config->load("/config.yaml")) {
        sendAck();
    } else {
        sendNack(HubFxError::CONFIG_ERROR);
    }
}

void StorageServer::handleConfigGet() {
    if (!_config) {
        sendNack(HubFxError::CONFIG_ERROR);
        return;
    }

    // Response: [loaded:u8][size:u16LE][reserved:u8]
    int size = _config->getSize("/config.yaml");
    uint8_t buf[4];
    buf[0] = _config->settings().loaded ? 1 : 0;
    putU16LE(&buf[1], (uint16_t)(size >= 0 ? size : 0));
    buf[3] = 0;  // reserved

    sendRawPacket(HubFxPacket::CONFIG_GET_RESP, currentTag(), buf, 4);
}


// ============================================================================
// SD Card Handlers
// ============================================================================

void StorageServer::handleSdInit(const uint8_t* payload, size_t len) {
    uint8_t speed_mhz = (len >= 1) ? payload[0] : 20;  // Default 20 MHz

    if (speed_mhz == 0 || speed_mhz > 50) {
        sendNack(SerialError::PARAM_OUT_OF_RANGE);
        return;
    }

    if (sd().retryInit(speed_mhz)) {
        sendAck();
    } else {
        sendNack(HubFxError::SD_NOT_INITIALIZED);
    }
}

void StorageServer::handleSdStatusReq() {
    if (!sd().isInitialized()) {
        // Not initialized â€” return minimal response
        uint8_t buf[1] = { 0 };  // initialized = false
        sendRawPacket(HubFxPacket::SD_STATUS_RESP, currentTag(), buf, 1);
        return;
    }

    // Enhanced response: [initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE][freeSpace_MB:u32LE][fatType:u8]
    StorageInfo info;
    uint8_t err = sd().getStorageInfo(info);

    if (err != SdError::OK) {
        uint8_t buf[1] = { 0 };  // initialized = false
        sendRawPacket(HubFxPacket::SD_STATUS_RESP, currentTag(), buf, 1);
        return;
    }

    uint8_t buf[14];
    buf[0] = info.initialized ? 1 : 0;
    putU32LE(&buf[1], info.cardSize_MB);     // MB
    putU32LE(&buf[5], info.totalSpace_MB);   // MB
    putU32LE(&buf[9], info.freeSpace_MB);    // MB
    buf[13] = info.fatType;

    sendRawPacket(HubFxPacket::SD_STATUS_RESP, currentTag(), buf, 14);
}


// ============================================================================
// File Operations
// ============================================================================

void StorageServer::handleFileList(const uint8_t* payload, size_t len) {
    if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }

    char path[128];
    if (!extractPath(payload, len, path, sizeof(path))) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    // Stream directory listing in POSIX-like format
    StreamWriter stream(*this, currentTag());
    stream.begin(0);  // Size unknown

    uint8_t err = sd().listDirectory(path, [&stream](const FileEntry& entry) {
        if (entry.isDirectory) {
            stream.printf("d  %9s  %s/\n", "-", entry.name);
        } else {
            stream.printf("-  %9lu  %s\n", (unsigned long)entry.size, entry.name);
        }
        return true;
    });

    if (err != SdError::OK && err != SdError::NOT_FOUND) {
        // Stream may be partially written â€” still send END for clean close
    }

    stream.end();
}

void StorageServer::handleFileDelete(const uint8_t* payload, size_t len) {
    if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }

    char path[128];
    if (!extractPath(payload, len, path, sizeof(path))) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    uint8_t err = sd().removeFile(path);
    if (err == SdError::OK) {
        sendAck();
    } else {
        sendNack(mapSdError(err));
    }
}

void StorageServer::handleFileMkdir(const uint8_t* payload, size_t len) {
    if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }

    char path[128];
    if (!extractPath(payload, len, path, sizeof(path))) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    uint8_t err = sd().makeDirectory(path);
    if (err == SdError::OK) {
        sendAck();
    } else {
        sendNack(mapSdError(err));
    }
}

void StorageServer::handleFileInfo(const uint8_t* payload, size_t len) {
    if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }

    char path[128];
    if (!extractPath(payload, len, path, sizeof(path))) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    FileEntry entry;
    uint8_t err = sd().getFileInfo(path, entry);

    // FILE_INFO_RESP: [exists:u8][isDir:u8][size:u32LE]
    uint8_t buf[6];
    if (err == SdError::OK) {
        buf[0] = 1;  // exists
        buf[1] = entry.isDirectory ? 1 : 0;
        putU32LE(&buf[2], entry.size);
    } else {
        buf[0] = 0;  // does not exist
        buf[1] = 0;
        putU32LE(&buf[2], 0);
    }

    sendRawPacket(HubFxPacket::FILE_INFO_RESP, currentTag(), buf, 6);
}

void StorageServer::handleFileDownload(const uint8_t* payload, size_t len) {
    if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }

    char path[128];
    if (!extractPath(payload, len, path, sizeof(path))) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    // Open file for reading (lock for open, then read in chunks)
    sd().lock();
    File32 file;
    uint8_t err = sd().openRead(path, file);
    if (err != SdError::OK) {
        sd().unlock();
        sendNack(mapSdError(err));
        return;
    }

    uint32_t fileSize = (uint32_t)file.size();
    sd().unlock();

    // Stream the file content
    StreamWriter stream(*this, currentTag());
    stream.begin(fileSize);

    uint8_t readBuf[StreamProtocol::MAX_CHUNK_DATA];
    bool ioError = false;

    while (true) {
        sd().lock();
        int n = file.read(readBuf, sizeof(readBuf));
        sd().unlock();

        if (n <= 0) break;

        if (!stream.write(readBuf, (size_t)n)) {
            ioError = true;
            break;
        }
    }

    sd().lock();
    file.close();
    sd().unlock();

    stream.end();
}


// ============================================================================
// Upload Handlers
// ============================================================================

void StorageServer::handleUploadBegin(const uint8_t* payload, size_t len) {
    if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }

    if (_upload.active) {
        sendNack(HubFxError::UPLOAD_IN_PROGRESS);
        return;
    }

    // FILE_UPLOAD_BEGIN: [size:u32LE][pathLen:u8][path:str]
    if (len < 5) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    uint32_t expectedSize = getU32LE(payload);
    char path[128];
    if (!extractPath(payload + 4, len - 4, path, sizeof(path))) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    // Open file for writing
    sd().lock();
    uint8_t err = sd().openWrite(path, _upload.file, true);
    sd().unlock();

    if (err != SdError::OK) {
        sendNack(mapSdError(err));
        return;
    }

    // Initialize upload state
    _upload.active = true;
    strncpy(_upload.path, path, sizeof(_upload.path) - 1);
    _upload.path[sizeof(_upload.path) - 1] = '\0';
    _upload.expectedSize = expectedSize;
    _upload.bytesReceived = 0;
    _upload.expectedSeq = 0;

    sendAck();
}

void StorageServer::handleUploadData(const uint8_t* payload, size_t len) {
    if (!_upload.active) {
        sendNack(HubFxError::NO_UPLOAD_ACTIVE);
        return;
    }

    // FILE_UPLOAD_DATA: [seqNum:u16LE][crc16:u16LE][data:N]
    if (len < StreamProtocol::CHUNK_HEADER_SIZE) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    uint16_t seqNum   = getU16LE(payload);
    uint16_t crc16    = getU16LE(payload + 2);
    const uint8_t* data = payload + StreamProtocol::CHUNK_HEADER_SIZE;
    size_t dataLen = len - StreamProtocol::CHUNK_HEADER_SIZE;

    // Verify sequence number
    if (seqNum != _upload.expectedSeq) {
        sendNack(SerialError::PARAM_OUT_OF_RANGE);
        return;
    }

    // Verify CRC-16
    uint16_t computedCrc = StreamProtocol::crc16(data, dataLen);
    if (computedCrc != crc16) {
        // CRC mismatch â€” client should retry this segment
        sendNack(SerialError::CRC_ERROR);
        return;
    }

    // Write data to SD card
    sd().lock();
    size_t written = _upload.file.write(data, dataLen);
    sd().unlock();

    if (written != dataLen) {
        // I/O error â€” abort upload
        sd().lock();
        _upload.file.close();
        sd().unlock();
        _upload.active = false;
        sendNack(HubFxError::FILE_IO_ERROR);
        return;
    }

    _upload.bytesReceived += dataLen;
    _upload.expectedSeq++;
    sendAck();
}

void StorageServer::handleUploadEnd() {
    if (!_upload.active) {
        sendNack(HubFxError::NO_UPLOAD_ACTIVE);
        return;
    }

    // Sync and close the file
    sd().lock();
    _upload.file.sync();
    _upload.file.close();
    sd().unlock();

    _upload.active = false;

    // Verify size if expected size was specified (non-zero)
    if (_upload.expectedSize > 0 && _upload.bytesReceived != _upload.expectedSize) {
        sendNack(HubFxError::FILE_IO_ERROR);
        return;
    }

    sendAck();
}

void StorageServer::handleUploadCancel() {
    if (!_upload.active) {
        sendAck();  // Nothing to cancel, but not an error
        return;
    }

    // Close and remove the partial file
    sd().lock();
    _upload.file.close();
    sd().getSd().remove(_upload.path);
    sd().unlock();

    _upload.active = false;
    sendAck();
}


// ============================================================================
// Helpers
// ============================================================================

uint8_t StorageServer::mapSdError(uint8_t sdErr) {
    switch (sdErr) {
        case SdError::OK:              return SerialError::OK;
        case SdError::NOT_INITIALIZED: return HubFxError::SD_NOT_INITIALIZED;
        case SdError::NOT_FOUND:       return HubFxError::FILE_NOT_FOUND;
        case SdError::IO_ERROR:        return HubFxError::FILE_IO_ERROR;
        case SdError::IS_DIRECTORY:    return HubFxError::FILE_NOT_FOUND;
        case SdError::ALREADY_EXISTS:  return HubFxError::FILE_ALREADY_EXISTS;
        default:                       return SerialError::INTERNAL_ERROR;
    }
}

bool StorageServer::extractPath(const uint8_t* payload, size_t len,
                                 char* pathBuf, size_t pathBufSize) {
    if (len < 1) return false;

    uint8_t pathLen = payload[0];
    if (pathLen == 0 || len < 1u + pathLen) return false;
    if (pathLen >= pathBufSize) return false;

    memcpy(pathBuf, payload + 1, pathLen);
    pathBuf[pathLen] = '\0';
    return true;
}
