/*
 * HubFX Storage Client — Protocol Implementation
 *
 * HubFxStorageClient command methods and response parsing.
 * Ported from lib_archive/sfx_common/serial/hubfx/hubfx.cpp.
 */

#include <platform/sfx_platform.h>

#if defined(SFX_HAS_STORAGE_SERVER)

#include "storage_client.h"

using namespace CoreProtocol;

// ============================================================================
// Module Packet Handler (response parsing)
// ============================================================================

void HubFxStorageClient::onModulePacket(uint8_t type, uint8_t tag,
                                         const uint8_t* payload, size_t len) {
    switch (type) {
        case HubFxPacket::CONFIG_STATUS_RESP: {
            _lastConfigInfo = {};
            if (len >= 4) {
                _lastConfigInfo.loaded   = payload[0] != 0;
                _lastConfigInfo.fileSize = getU16LE(&payload[1]);
            }
            if (tag != CoreProtocol::TAG_ASYNC) {
                _resultQueue.resolve(tag, CommandResult::Ack());
            }
            if (_configInfoCb) _configInfoCb(_lastConfigInfo);
            break;
        }

        case HubFxPacket::SD_STATUS_RESP: {
            _lastSdStatus = {};
            if (len >= 14) {
                _lastSdStatus.initialized   = payload[0] != 0;
                _lastSdStatus.cardSize_MB   = getU32LE(&payload[1]);
                _lastSdStatus.totalSpace_MB = getU32LE(&payload[5]);
                _lastSdStatus.freeSpace_MB  = getU32LE(&payload[9]);
                _lastSdStatus.fatType       = payload[13];
            } else if (len >= 1) {
                _lastSdStatus.initialized = payload[0] != 0;
            }
            if (tag != CoreProtocol::TAG_ASYNC) {
                _resultQueue.resolve(tag, CommandResult::Ack());
            }
            if (_sdStatusCb) _sdStatusCb(_lastSdStatus);
            break;
        }

        case HubFxPacket::FLASH_STATUS_REQ: {
            // FLASH_STATUS uses same packet type for request and response
            _lastFlashStatus = {};
            if (len >= 13) {
                _lastFlashStatus.initialized = payload[0] != 0;
                _lastFlashStatus.totalBytes  = getU32LE(&payload[1]);
                _lastFlashStatus.usedBytes   = getU32LE(&payload[5]);
                _lastFlashStatus.freeBytes   = getU32LE(&payload[9]);
            } else if (len >= 1) {
                _lastFlashStatus.initialized = payload[0] != 0;
            }
            if (tag != CoreProtocol::TAG_ASYNC) {
                _resultQueue.resolve(tag, CommandResult::Ack());
            }
            if (_flashStatusCb) _flashStatusCb(_lastFlashStatus);
            break;
        }

        case HubFxPacket::FILE_INFO_RESP: {
            _lastFileInfo = {};
            if (len >= 6) {
                _lastFileInfo.exists      = payload[0] != 0;
                _lastFileInfo.isDirectory = payload[1] != 0;
                _lastFileInfo.size        = getU32LE(&payload[2]);
            }
            if (tag != CoreProtocol::TAG_ASYNC) {
                _resultQueue.resolve(tag, CommandResult::Ack());
            }
            if (_fileInfoCb) _fileInfoCb(_lastFileInfo);
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Config Commands
// ============================================================================

CommandResult HubFxStorageClient::configReload() {
    return sendCommand(HubFxPacket::CONFIG_RELOAD, nullptr, 0);
}

CommandResult HubFxStorageClient::configStatus() {
    return sendCommand(HubFxPacket::CONFIG_STATUS, nullptr, 0);
}

// ============================================================================
// SD Card Commands
// ============================================================================

CommandResult HubFxStorageClient::sdInit(uint8_t speed_mhz) {
    uint8_t payload[1] = { speed_mhz };
    return sendCommand(HubFxPacket::SD_INIT, payload, 1);
}

CommandResult HubFxStorageClient::sdStatus() {
    return sendCommand(HubFxPacket::SD_STATUS_REQ, nullptr, 0);
}

// ============================================================================
// Flash Commands
// ============================================================================

CommandResult HubFxStorageClient::flashStatus() {
    return sendCommand(HubFxPacket::FLASH_STATUS_REQ, nullptr, 0);
}

// ============================================================================
// File Operations
// ============================================================================

CommandResult HubFxStorageClient::fileList(const char* path, HubFxStorage::StorageTarget target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127 + 1];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    size_t totalLen = 1 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = (uint8_t)target;
    }

    return sendCommand(HubFxPacket::FILE_LIST, payload, totalLen);
}

CommandResult HubFxStorageClient::fileDelete(const char* path, HubFxStorage::StorageTarget target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127 + 1];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    size_t totalLen = 1 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = (uint8_t)target;
    }

    return sendCommand(HubFxPacket::FILE_DELETE, payload, totalLen);
}

CommandResult HubFxStorageClient::fileMkdir(const char* path, HubFxStorage::StorageTarget target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127 + 1];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    size_t totalLen = 1 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = (uint8_t)target;
    }

    return sendCommand(HubFxPacket::FILE_MKDIR, payload, totalLen);
}

CommandResult HubFxStorageClient::fileInfo(const char* path, HubFxStorage::StorageTarget target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127 + 1];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    size_t totalLen = 1 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = (uint8_t)target;
    }

    return sendCommand(HubFxPacket::FILE_INFO, payload, totalLen);
}

CommandResult HubFxStorageClient::fileDownload(const char* path, HubFxStorage::StorageTarget target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127 + 1];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    size_t totalLen = 1 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = (uint8_t)target;
    }

    return sendCommand(HubFxPacket::FILE_DOWNLOAD, payload, totalLen);
}

// ============================================================================
// Upload Commands
// ============================================================================

CommandResult HubFxStorageClient::uploadBegin(const char* path, uint32_t size,
                                               HubFxStorage::StorageTarget target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[4 + 1 + 127 + 1];
    putU32LE(payload, size);
    payload[4] = (uint8_t)pathLen;
    memcpy(&payload[5], path, pathLen);

    size_t totalLen = 5 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = (uint8_t)target;
    }

    return sendCommand(HubFxPacket::FILE_UPLOAD_BEGIN, payload, totalLen);
}

CommandResult HubFxStorageClient::uploadData(uint16_t seqNum, const uint8_t* data, size_t dataLen) {
    uint8_t payload[4 + 512];
    putU16LE(payload, seqNum);
    uint16_t crc = StreamProtocol::crc16(data, dataLen);
    putU16LE(payload + 2, crc);

    size_t copyLen = (dataLen <= 508) ? dataLen : 508;
    memcpy(payload + 4, data, copyLen);

    return sendCommand(HubFxPacket::FILE_UPLOAD_DATA, payload, 4 + copyLen);
}

CommandResult HubFxStorageClient::uploadEnd() {
    return sendCommand(HubFxPacket::FILE_UPLOAD_END, nullptr, 0);
}

CommandResult HubFxStorageClient::uploadCancel() {
    return sendCommand(HubFxPacket::FILE_UPLOAD_CANCEL, nullptr, 0);
}

#endif  // SFX_HAS_STORAGE_SERVER
