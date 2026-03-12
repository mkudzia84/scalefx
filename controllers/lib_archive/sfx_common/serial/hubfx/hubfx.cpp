/*
 * Serial HubFX Protocol — Implementation
 *
 * HubFX-specific client/server protocol implementation.
 *   - HubFxAudioServer:   Audio playback command handlers
 *   - HubFxStorageServer: Config, SD, flash, and file transfer command handlers
 *   - HubFxAudioClient:   Audio command methods and status parsing
 *   - HubFxStorageClient: Storage command methods and response parsing
 */

#include "hubfx.h"
#include <audio/audio_mixer.h>
#include <audio/audio_codec.h>
#include <audio/audio_config.h>
#include <serial/core/diag_log.h>

using namespace CoreProtocol;


// ============================================================================
// ========================  AUDIO SERVER  ====================================
// ============================================================================

CommandHandleResult HubFxAudioServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case HubFxPacket::AUDIO_PLAY:
            handlePlay(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::AUDIO_STOP:
            handleStop(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::AUDIO_VOLUME:
            handleVolume(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::AUDIO_FADE:
            handleFade(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::AUDIO_QUEUE:
            handleQueue(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::AUDIO_QUEUE_CLEAR:
            handleQueueClear(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::AUDIO_STATUS_REQ:
            handleStatusReq();
            return CommandHandleResult::Handled;

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ----------------------------------------------------------------------------
// Audio Play
// Wire: [ch:u8][vol:u8][output:u8][loopMode:u8][loopCount:u16LE][pathLen:u8][path:str]
// ----------------------------------------------------------------------------

void HubFxAudioServer::handlePlay(const uint8_t* payload, size_t len) {
    if (len < 7) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    if (!_mixer) {
        sendNack(HubFxError::AUDIO_ERROR);
        return;
    }

    uint8_t ch       = payload[0];
    uint8_t volPct   = payload[1];    // 0-100
    uint8_t output   = payload[2];
    uint8_t loopMode = payload[3];
    uint16_t loopCnt = getU16LE(&payload[4]);
    uint8_t pathLen  = payload[6];

    if (ch >= HubFxAudio::MAX_CHANNELS) {
        sendNack(HubFxError::INVALID_CHANNEL);
        return;
    }

    if (len < (size_t)(7 + pathLen) || pathLen == 0) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    char path[128];
    size_t copyLen = (pathLen < sizeof(path) - 1) ? pathLen : sizeof(path) - 1;
    memcpy(path, &payload[7], copyLen);
    path[copyLen] = '\0';

    AudioPlaybackOptions opts;
    opts.volume = volPct / 100.0f;
    opts.output = (output == HubFxAudio::OUTPUT_LEFT)  ? AudioOutput::Left
                : (output == HubFxAudio::OUTPUT_RIGHT) ? AudioOutput::Right
                : AudioOutput::Stereo;

    switch (loopMode) {
        case HubFxAudio::LOOP_INFINITE:
            opts.loop = true;
            opts.loopCount = LOOP_INFINITE;
            break;
        case HubFxAudio::LOOP_FINITE:
            opts.loop = true;
            opts.loopCount = (int)loopCnt;
            break;
        default:
            opts.loop = false;
            opts.loopCount = 0;
            break;
    }

    DiagLog::instance().info("[AudioSrv] Play ch%d: %s vol=%d%% loop=%d", ch, path, volPct, loopMode);

    if (_mixer->playAsync(ch, path, opts)) {
        sendAck();
    } else {
        DiagLog::instance().error("[AudioSrv] playAsync failed ch%d: %s", ch, path);
        sendNack(HubFxError::AUDIO_ERROR);
    }
}

// ----------------------------------------------------------------------------
// Audio Stop
// Wire: [ch:u8] (0xFF=all)
// ----------------------------------------------------------------------------

void HubFxAudioServer::handleStop(const uint8_t* payload, size_t len) {
    if (len < 1) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    if (!_mixer) {
        sendNack(HubFxError::AUDIO_ERROR);
        return;
    }

    uint8_t ch = payload[0];

    if (ch == HubFxAudio::CH_ALL) {
        _mixer->stopAsync(-1, AudioStopMode::Immediate);
    } else if (ch < HubFxAudio::MAX_CHANNELS) {
        _mixer->stopAsync(ch, AudioStopMode::Immediate);
    } else {
        sendNack(HubFxError::INVALID_CHANNEL);
        return;
    }

    sendAck();
}

// ----------------------------------------------------------------------------
// Audio Volume
// Wire: [ch:u8][vol:u8] (ch 0xFF=master, vol 0-100)
// ----------------------------------------------------------------------------

void HubFxAudioServer::handleVolume(const uint8_t* payload, size_t len) {
    if (len < 2) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    if (!_mixer) {
        sendNack(HubFxError::AUDIO_ERROR);
        return;
    }

    uint8_t ch    = payload[0];
    uint8_t vol   = payload[1];
    float volume  = vol / 100.0f;

    if (ch == HubFxAudio::CH_ALL) {
        _mixer->setMasterVolumeAsync(volume);
    } else if (ch < HubFxAudio::MAX_CHANNELS) {
        _mixer->setVolumeAsync(ch, volume);
    } else {
        sendNack(HubFxError::INVALID_CHANNEL);
        return;
    }

    sendAck();
}

// ----------------------------------------------------------------------------
// Audio Fade
// Wire: [ch:u8]
// ----------------------------------------------------------------------------

void HubFxAudioServer::handleFade(const uint8_t* payload, size_t len) {
    if (len < 1) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    if (!_mixer) {
        sendNack(HubFxError::AUDIO_ERROR);
        return;
    }

    uint8_t ch = payload[0];
    if (ch >= HubFxAudio::MAX_CHANNELS) {
        sendNack(HubFxError::INVALID_CHANNEL);
        return;
    }

    _mixer->stopAsync(ch, AudioStopMode::Fade);
    sendAck();
}

// ----------------------------------------------------------------------------
// Audio Queue
// Wire: [ch:u8][vol:u8][loopCount:u16LE][behavior:u8][pathLen:u8][path:str]
// ----------------------------------------------------------------------------

void HubFxAudioServer::handleQueue(const uint8_t* payload, size_t len) {
    if (len < 6) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    if (!_mixer) {
        sendNack(HubFxError::AUDIO_ERROR);
        return;
    }

    uint8_t ch       = payload[0];
    uint8_t volPct   = payload[1];
    uint16_t loopCnt = getU16LE(&payload[2]);
    uint8_t behavior = payload[4];
    uint8_t pathLen  = payload[5];

    if (ch >= HubFxAudio::MAX_CHANNELS) {
        sendNack(HubFxError::INVALID_CHANNEL);
        return;
    }

    if (len < (size_t)(6 + pathLen) || pathLen == 0) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    char path[128];
    size_t copyLen = (pathLen < sizeof(path) - 1) ? pathLen : sizeof(path) - 1;
    memcpy(path, &payload[6], copyLen);
    path[copyLen] = '\0';

    AudioPlaybackOptions opts;
    opts.volume = volPct / 100.0f;
    opts.output = AudioOutput::Stereo;

    if (loopCnt > 0) {
        opts.loop = true;
        opts.loopCount = (int)loopCnt;
    } else {
        opts.loop = false;
        opts.loopCount = 0;
    }

    QueueLoopBehavior qBehavior = (behavior == HubFxAudio::QUEUE_STOP_NOW)
        ? QueueLoopBehavior::StopImmediate
        : QueueLoopBehavior::FinishLoop;

    if (_mixer->queueSoundAsync(ch, path, opts, qBehavior)) {
        sendAck();
    } else {
        sendNack(HubFxError::AUDIO_ERROR);
    }
}

// ----------------------------------------------------------------------------
// Audio Queue Clear
// Wire: [ch:u8] (0xFF=all)
// ----------------------------------------------------------------------------

void HubFxAudioServer::handleQueueClear(const uint8_t* payload, size_t len) {
    if (len < 1) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    if (!_mixer) {
        sendNack(HubFxError::AUDIO_ERROR);
        return;
    }

    uint8_t ch = payload[0];

    if (ch == HubFxAudio::CH_ALL) {
        _mixer->clearQueueAsync(-1);
    } else if (ch < HubFxAudio::MAX_CHANNELS) {
        _mixer->clearQueueAsync(ch);
    } else {
        sendNack(HubFxError::INVALID_CHANNEL);
        return;
    }

    sendAck();
}

// ----------------------------------------------------------------------------
// Audio Status Request
// Response v3: [masterVol:u8][flags:u8][sampleRate_Hz:u16LE][bitDepth:u8]
//              [maxCh:u8][codecNameLen:u8][codecName:str]
//              [ringFillPct:u8][ringAvailRead:u16LE][ringAvailWrite:u16LE]
//              [underruns:u32LE][consumeLoops:u32LE][consumeFrames:u32LE]
//              [activeMask:u8]
//              [per-ch: ch:u8,vol:u8,playing:u8,looping:u8,loopCount:u16LE,
//               remaining_ms:u32LE,queueLen:u8,output:u8,
//               wavRate_Hz:u16LE,wavCh:u8,wavBits:u8,fnameLen:u8,fname:str]
// ----------------------------------------------------------------------------

void HubFxAudioServer::handleStatusReq() {
    if (!_mixer) {
        sendNack(HubFxError::AUDIO_ERROR);
        return;
    }

    uint8_t buf[512];
    size_t pos = 0;

    // System header
    buf[pos++] = (uint8_t)(_mixer->masterVolume() * 100.0f);

    // Flags: bit0=initialized, bit1=i2sRunning, bit2=hasCodec, bit3=hasRingStats (v3)
    uint8_t flags = 0;
    if (_mixer->isInitialized())  flags |= 0x01;
    if (_mixer->isI2SRunning())   flags |= 0x02;
    if (_mixer->getCodec())       flags |= 0x04;
    flags |= 0x08;  // v3: has ring buffer stats
    buf[pos++] = flags;

    putU16LE(&buf[pos], AUDIO_SAMPLE_RATE);  // sampleRate_Hz
    pos += 2;
    buf[pos++] = AUDIO_BIT_DEPTH;
    buf[pos++] = AUDIO_MAX_CHANNELS;

    // Codec name (length-prefixed)
    const char* codecName = _mixer->getCodec() ? _mixer->getCodec()->getModelName() : "";
    uint8_t codecLen = (uint8_t)strlen(codecName);
    buf[pos++] = codecLen;
    if (codecLen > 0) {
        memcpy(&buf[pos], codecName, codecLen);
        pos += codecLen;
    }

    // Ring buffer stats (v3)
    buf[pos++] = (uint8_t)_mixer->getRingFillPercent();
    putU16LE(&buf[pos], (uint16_t)min(_mixer->getRingAvailableRead(), (uint32_t)65535));
    pos += 2;
    putU16LE(&buf[pos], (uint16_t)min(_mixer->getRingAvailableWrite(), (uint32_t)65535));
    pos += 2;
    putU32LE(&buf[pos], _mixer->getUnderruns());
    pos += 4;
    putU32LE(&buf[pos], _mixer->getConsumeLoops());
    pos += 4;
    putU32LE(&buf[pos], _mixer->getConsumeFrames());
    pos += 4;

    // Per-channel data (only active channels)
    uint8_t activeMask = 0;
    for (int i = 0; i < 8; i++) {
        if (_mixer->isPlaying(i) || _mixer->queueLength(i) > 0) {
            activeMask |= (1 << i);
        }
    }
    buf[pos++] = activeMask;

    for (int i = 0; i < 8; i++) {
        if (!(activeMask & (1 << i))) continue;
        if (pos > sizeof(buf) - 64) break;  // safety margin

        buf[pos++] = (uint8_t)i;
        buf[pos++] = (uint8_t)(_mixer->getChannelVolume(i) * 100.0f);
        buf[pos++] = _mixer->isPlaying(i) ? 1 : 0;
        buf[pos++] = _mixer->isLooping(i) ? 1 : 0;
        putU16LE(&buf[pos], (uint16_t)_mixer->getLoopCount(i));
        pos += 2;
        uint32_t remaining = (uint32_t)max(_mixer->remainingMs(i), 0);
        putU32LE(&buf[pos], remaining);
        pos += 4;
        buf[pos++] = (uint8_t)_mixer->queueLength(i);
        buf[pos++] = (uint8_t)_mixer->getOutput(i);

        // WAV format info
        putU16LE(&buf[pos], (uint16_t)_mixer->getSampleRate(i));
        pos += 2;
        buf[pos++] = (uint8_t)_mixer->getNumChannels(i);
        buf[pos++] = (uint8_t)_mixer->getBitsPerSample(i);

        // Filename (length-prefixed)
        const char* fname = _mixer->getFilename(i);
        uint8_t fnameLen = fname ? (uint8_t)strlen(fname) : 0;
        buf[pos++] = fnameLen;
        if (fnameLen > 0) {
            memcpy(&buf[pos], fname, fnameLen);
            pos += fnameLen;
        }
    }

    sendRawPacket(HubFxPacket::AUDIO_STATUS_RESP, currentTag(), buf, pos);
}


// ============================================================================
// ========================  STORAGE SERVER  ==================================
// ============================================================================

CommandHandleResult HubFxStorageServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
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

        // --- Flash (0x99) ---
        case HubFxPacket::FLASH_STATUS_REQ:
            handleFlashStatusReq();
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

void HubFxStorageServer::handleConfigReload() {
    if (!_configReloadCb) {
        sendNack(HubFxError::CONFIG_ERROR);
        return;
    }

    uint8_t err = _configReloadCb();
    if (err == SerialError::OK) {
        sendAck();
    } else {
        sendNack(err);
    }
}

void HubFxStorageServer::handleConfigGet() {
    if (!_configGetCb) {
        sendNack(HubFxError::CONFIG_ERROR);
        return;
    }

    bool loaded = false;
    uint16_t fileSize = 0;
    _configGetCb(loaded, fileSize);

    // Response: [loaded:u8][size:u16LE][reserved:u8]
    uint8_t buf[4];
    buf[0] = loaded ? 1 : 0;
    putU16LE(&buf[1], fileSize);
    buf[3] = 0;  // reserved

    sendRawPacket(HubFxPacket::CONFIG_GET_RESP, currentTag(), buf, 4);
}


// ============================================================================
// SD Card Handlers
// ============================================================================

void HubFxStorageServer::handleSdInit(const uint8_t* payload, size_t len) {
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

void HubFxStorageServer::handleSdStatusReq() {
    if (!sd().isInitialized()) {
        uint8_t buf[1] = { 0 };  // initialized = false
        sendRawPacket(HubFxPacket::SD_STATUS_RESP, currentTag(), buf, 1);
        return;
    }

    // Enhanced response: [initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE][freeSpace_MB:u32LE][fatType:u8]
    StorageInfo info;
    uint8_t err = sd().getStorageInfo(info);

    if (err != SdError::OK) {
        uint8_t buf[1] = { 0 };
        sendRawPacket(HubFxPacket::SD_STATUS_RESP, currentTag(), buf, 1);
        return;
    }

    uint8_t buf[14];
    buf[0] = info.initialized ? 1 : 0;
    putU32LE(&buf[1], info.cardSize_MB);
    putU32LE(&buf[5], info.totalSpace_MB);
    putU32LE(&buf[9], info.freeSpace_MB);
    buf[13] = info.fatType;

    sendRawPacket(HubFxPacket::SD_STATUS_RESP, currentTag(), buf, 14);
}


// ============================================================================
// Flash Handlers
// ============================================================================

void HubFxStorageServer::handleFlashStatusReq() {
    if (!flash().isInitialized()) {
        uint8_t buf[1] = { 0 };  // initialized = false
        sendRawPacket(HubFxPacket::FLASH_STATUS_REQ, currentTag(), buf, 1);
        return;
    }

    // Response: [initialized:u8][totalBytes:u32LE][usedBytes:u32LE][freeBytes:u32LE]
    FlashStorageInfo info;
    uint8_t err = flash().getStorageInfo(info);

    if (err != FlashError::OK) {
        uint8_t buf[1] = { 0 };
        sendRawPacket(HubFxPacket::FLASH_STATUS_REQ, currentTag(), buf, 1);
        return;
    }

    uint8_t buf[13];
    buf[0] = info.initialized ? 1 : 0;
    putU32LE(&buf[1], info.totalBytes);
    putU32LE(&buf[5], info.usedBytes);
    putU32LE(&buf[9], info.freeBytes);

    sendRawPacket(HubFxPacket::FLASH_STATUS_REQ, currentTag(), buf, 13);
}


// ============================================================================
// File Operations (SD or Flash target)
// ============================================================================

void HubFxStorageServer::handleFileList(const uint8_t* payload, size_t len) {
    char path[128];
    uint8_t target = HubFxStorage::TARGET_SD;
    if (!extractPathAndTarget(payload, len, path, sizeof(path), target)) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    if (target == HubFxStorage::TARGET_FLASH) {
        if (!flash().isInitialized()) { sendNack(HubFxError::FILE_IO_ERROR); return; }

        StreamWriter stream(*this, currentTag());
        stream.begin(0);

        flash().listDirectory(path, [&stream](const FileEntry& entry) {
            if (entry.isDirectory) {
                stream.printf("d  %9s  %s/\n", "-", entry.name);
            } else {
                stream.printf("-  %9lu  %s\n", (unsigned long)entry.size, entry.name);
            }
            return true;
        });

        stream.end();
    } else {
        if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }

        StreamWriter stream(*this, currentTag());
        stream.begin(0);

        sd().listDirectory(path, [&stream](const FileEntry& entry) {
            if (entry.isDirectory) {
                stream.printf("d  %9s  %s/\n", "-", entry.name);
            } else {
                stream.printf("-  %9lu  %s\n", (unsigned long)entry.size, entry.name);
            }
            return true;
        });

        stream.end();
    }
}

void HubFxStorageServer::handleFileDelete(const uint8_t* payload, size_t len) {
    char path[128];
    uint8_t target = HubFxStorage::TARGET_SD;
    if (!extractPathAndTarget(payload, len, path, sizeof(path), target)) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    uint8_t err;
    if (target == HubFxStorage::TARGET_FLASH) {
        if (!flash().isInitialized()) { sendNack(HubFxError::FILE_IO_ERROR); return; }
        err = flash().removeFile(path);
        if (err == FlashError::OK) sendAck();
        else sendNack(mapFlashError(err));
    } else {
        if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }
        err = sd().removeFile(path);
        if (err == SdError::OK) sendAck();
        else sendNack(mapSdError(err));
    }
}

void HubFxStorageServer::handleFileMkdir(const uint8_t* payload, size_t len) {
    char path[128];
    uint8_t target = HubFxStorage::TARGET_SD;
    if (!extractPathAndTarget(payload, len, path, sizeof(path), target)) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    uint8_t err;
    if (target == HubFxStorage::TARGET_FLASH) {
        if (!flash().isInitialized()) { sendNack(HubFxError::FILE_IO_ERROR); return; }
        err = flash().makeDirectory(path);
        if (err == FlashError::OK) sendAck();
        else sendNack(mapFlashError(err));
    } else {
        if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }
        err = sd().makeDirectory(path);
        if (err == SdError::OK) sendAck();
        else sendNack(mapSdError(err));
    }
}

void HubFxStorageServer::handleFileInfo(const uint8_t* payload, size_t len) {
    char path[128];
    uint8_t target = HubFxStorage::TARGET_SD;
    if (!extractPathAndTarget(payload, len, path, sizeof(path), target)) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    FileEntry entry;
    uint8_t err;

    if (target == HubFxStorage::TARGET_FLASH) {
        if (!flash().isInitialized()) { sendNack(HubFxError::FILE_IO_ERROR); return; }
        err = flash().getFileInfo(path, entry);
    } else {
        if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }
        err = sd().getFileInfo(path, entry);
    }

    // FILE_INFO_RESP: [exists:u8][isDir:u8][size:u32LE]
    uint8_t buf[6];
    if (err == SdError::OK || err == FlashError::OK) {
        buf[0] = 1;
        buf[1] = entry.isDirectory ? 1 : 0;
        putU32LE(&buf[2], entry.size);
    } else {
        buf[0] = 0;
        buf[1] = 0;
        putU32LE(&buf[2], 0);
    }

    sendRawPacket(HubFxPacket::FILE_INFO_RESP, currentTag(), buf, 6);
}

void HubFxStorageServer::handleFileDownload(const uint8_t* payload, size_t len) {
    char path[128];
    uint8_t target = HubFxStorage::TARGET_SD;
    if (!extractPathAndTarget(payload, len, path, sizeof(path), target)) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    if (target == HubFxStorage::TARGET_FLASH) {
        if (!flash().isInitialized()) { sendNack(HubFxError::FILE_IO_ERROR); return; }

        flash().lock();
        LFSFile file;
        uint8_t err = flash().openRead(path, file);
        if (err != FlashError::OK) {
            flash().unlock();
            sendNack(mapFlashError(err));
            return;
        }

        uint32_t fileSize = (uint32_t)file.size();
        flash().unlock();

        StreamWriter stream(*this, currentTag());
        stream.begin(fileSize);

        uint8_t readBuf[StreamProtocol::MAX_CHUNK_DATA];
        while (true) {
            flash().lock();
            int n = file.read(readBuf, sizeof(readBuf));
            flash().unlock();

            if (n <= 0) break;
            if (!stream.write(readBuf, (size_t)n)) break;
        }

        flash().lock();
        file.close();
        flash().unlock();

        stream.end();
    } else {
        if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }

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

        StreamWriter stream(*this, currentTag());
        stream.begin(fileSize);

        uint8_t readBuf[StreamProtocol::MAX_CHUNK_DATA];
        while (true) {
            sd().lock();
            int n = file.read(readBuf, sizeof(readBuf));
            sd().unlock();

            if (n <= 0) break;
            if (!stream.write(readBuf, (size_t)n)) break;
        }

        sd().lock();
        file.close();
        sd().unlock();

        stream.end();
    }
}


// ============================================================================
// Upload Handlers
// ============================================================================

void HubFxStorageServer::handleUploadBegin(const uint8_t* payload, size_t len) {
    if (_upload.active) {
        sendNack(HubFxError::UPLOAD_IN_PROGRESS);
        return;
    }

    // FILE_UPLOAD_BEGIN: [size:u32LE][pathLen:u8][path:str][target:u8?]
    if (len < 5) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    uint32_t expectedSize = getU32LE(payload);

    // Extract path from payload + 4 (skip size)
    char path[128];
    uint8_t target = HubFxStorage::TARGET_SD;
    if (!extractPathAndTarget(payload + 4, len - 4, path, sizeof(path), target)) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    // Open file for writing on the target storage
    uint8_t err;
    if (target == HubFxStorage::TARGET_FLASH) {
        if (!flash().isInitialized()) { sendNack(HubFxError::FILE_IO_ERROR); return; }
        flash().lock();
        err = flash().openWrite(path, _upload.flashFile, true);
        flash().unlock();
    } else {
        if (!sd().isInitialized()) { sendNack(HubFxError::SD_NOT_INITIALIZED); return; }
        sd().lock();
        err = sd().openWrite(path, _upload.sdFile, true);
        sd().unlock();
    }

    if (target == HubFxStorage::TARGET_FLASH ? err != FlashError::OK : err != SdError::OK) {
        sendNack(target == HubFxStorage::TARGET_FLASH ? mapFlashError(err) : mapSdError(err));
        return;
    }

    // Initialize upload state
    _upload.active = true;
    _upload.target = target;
    strncpy(_upload.path, path, sizeof(_upload.path) - 1);
    _upload.path[sizeof(_upload.path) - 1] = '\0';
    _upload.expectedSize = expectedSize;
    _upload.bytesReceived = 0;
    _upload.expectedSeq = 0;

    sendAck();
}

void HubFxStorageServer::handleUploadData(const uint8_t* payload, size_t len) {
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
        sendNack(SerialError::CRC_ERROR);
        return;
    }

    // Write data to storage
    size_t written = 0;
    if (_upload.target == HubFxStorage::TARGET_FLASH) {
        flash().lock();
        written = _upload.flashFile.write(data, dataLen);
        flash().unlock();
    } else {
        sd().lock();
        written = _upload.sdFile.write(data, dataLen);
        sd().unlock();
    }

    if (written != dataLen) {
        // I/O error — abort upload
        if (_upload.target == HubFxStorage::TARGET_FLASH) {
            flash().lock();
            _upload.flashFile.close();
            flash().unlock();
        } else {
            sd().lock();
            _upload.sdFile.close();
            sd().unlock();
        }
        _upload.active = false;
        sendNack(HubFxError::FILE_IO_ERROR);
        return;
    }

    _upload.bytesReceived += dataLen;
    _upload.expectedSeq++;
    sendAck();
}

void HubFxStorageServer::handleUploadEnd() {
    if (!_upload.active) {
        sendNack(HubFxError::NO_UPLOAD_ACTIVE);
        return;
    }

    // Sync and close
    if (_upload.target == HubFxStorage::TARGET_FLASH) {
        flash().lock();
        _upload.flashFile.flush();
        _upload.flashFile.close();
        flash().unlock();
    } else {
        sd().lock();
        _upload.sdFile.sync();
        _upload.sdFile.close();
        sd().unlock();
    }

    _upload.active = false;

    // Verify size if expected size was specified
    if (_upload.expectedSize > 0 && _upload.bytesReceived != _upload.expectedSize) {
        sendNack(HubFxError::FILE_IO_ERROR);
        return;
    }

    sendAck();
}

void HubFxStorageServer::handleUploadCancel() {
    if (!_upload.active) {
        sendAck();  // Nothing to cancel
        return;
    }

    if (_upload.target == HubFxStorage::TARGET_FLASH) {
        flash().lock();
        _upload.flashFile.close();
        // LittleFS remove
        LittleFS.remove(_upload.path);
        flash().unlock();
    } else {
        sd().lock();
        _upload.sdFile.close();
        sd().getSd().remove(_upload.path);
        sd().unlock();
    }

    _upload.active = false;
    sendAck();
}


// ============================================================================
// Storage Helpers
// ============================================================================

uint8_t HubFxStorageServer::mapSdError(uint8_t sdErr) {
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

uint8_t HubFxStorageServer::mapFlashError(uint8_t flashErr) {
    switch (flashErr) {
        case FlashError::OK:              return SerialError::OK;
        case FlashError::NOT_INITIALIZED: return HubFxError::FILE_IO_ERROR;
        case FlashError::NOT_FOUND:       return HubFxError::FILE_NOT_FOUND;
        case FlashError::IO_ERROR:        return HubFxError::FILE_IO_ERROR;
        case FlashError::IS_DIRECTORY:    return HubFxError::FILE_NOT_FOUND;
        case FlashError::ALREADY_EXISTS:  return HubFxError::FILE_ALREADY_EXISTS;
        default:                          return SerialError::INTERNAL_ERROR;
    }
}

bool HubFxStorageServer::extractPathAndTarget(const uint8_t* payload, size_t len,
                                               char* pathBuf, size_t pathBufSize,
                                               uint8_t& target) {
    if (len < 1) return false;

    uint8_t pathLen = payload[0];
    if (pathLen == 0 || len < 1u + pathLen) return false;
    if (pathLen >= pathBufSize) return false;

    memcpy(pathBuf, payload + 1, pathLen);
    pathBuf[pathLen] = '\0';

    // Optional target byte after path
    size_t afterPath = 1 + pathLen;
    if (len > afterPath) {
        target = payload[afterPath];
    } else {
        target = HubFxStorage::TARGET_SD;  // default
    }

    return true;
}

bool HubFxStorageServer::extractPath(const uint8_t* payload, size_t len,
                                      char* pathBuf, size_t pathBufSize) {
    if (len < 1) return false;

    uint8_t pathLen = payload[0];
    if (pathLen == 0 || len < 1u + pathLen) return false;
    if (pathLen >= pathBufSize) return false;

    memcpy(pathBuf, payload + 1, pathLen);
    pathBuf[pathLen] = '\0';
    return true;
}


// ============================================================================
// ========================  AUDIO CLIENT  ====================================
// ============================================================================

void HubFxAudioClient::onModulePacket(uint8_t type, uint8_t tag,
                                       const uint8_t* payload, size_t len) {
    switch (type) {
        case HubFxPacket::AUDIO_STATUS_RESP: {
            if (len < 7) break;  // minimum header

            size_t pos = 0;
            _lastStatus = {};  // reset

            _lastStatus.masterVolPct = payload[pos++];

            uint8_t flags = payload[pos++];
            _lastStatus.initialized  = (flags & 0x01) != 0;
            _lastStatus.i2sRunning   = (flags & 0x02) != 0;
            _lastStatus.hasCodec     = (flags & 0x04) != 0;
            _lastStatus.hasRingStats = (flags & 0x08) != 0;

            _lastStatus.sampleRate_Hz = getU16LE(&payload[pos]); pos += 2;
            _lastStatus.bitDepth      = payload[pos++];
            _lastStatus.maxChannels   = payload[pos++];

            // Codec name
            if (pos < len) {
                uint8_t codecLen = payload[pos++];
                if (codecLen > 0 && pos + codecLen <= len) {
                    size_t copyLen = (codecLen < sizeof(_lastStatus.codecName) - 1)
                                   ? codecLen : sizeof(_lastStatus.codecName) - 1;
                    memcpy(_lastStatus.codecName, &payload[pos], copyLen);
                    _lastStatus.codecName[copyLen] = '\0';
                    pos += codecLen;
                }
            }

            // Ring buffer stats (v3)
            if (_lastStatus.hasRingStats && pos + 15 <= len) {
                _lastStatus.ringFillPct     = payload[pos++];
                _lastStatus.ringAvailRead   = getU16LE(&payload[pos]); pos += 2;
                _lastStatus.ringAvailWrite  = getU16LE(&payload[pos]); pos += 2;
                _lastStatus.underruns       = getU32LE(&payload[pos]); pos += 4;
                _lastStatus.consumeLoops    = getU32LE(&payload[pos]); pos += 4;
                _lastStatus.consumeFrames   = getU32LE(&payload[pos]); pos += 4;
            }

            // Per-channel data
            if (pos < len) {
                _lastStatus.activeMask = payload[pos++];
                _lastStatus.activeCount = 0;

                for (int i = 0; i < 8 && pos + 15 <= len; i++) {
                    if (!(_lastStatus.activeMask & (1 << i))) continue;

                    HubFxAudioChannelInfo& ch = _lastStatus.channels[_lastStatus.activeCount];
                    ch.channel     = payload[pos++];
                    ch.volumePct   = payload[pos++];
                    ch.playing     = payload[pos++] != 0;
                    ch.looping     = payload[pos++] != 0;
                    ch.loopCount   = getU16LE(&payload[pos]); pos += 2;
                    ch.remaining_ms = getU32LE(&payload[pos]); pos += 4;
                    ch.queueLen    = payload[pos++];
                    ch.output      = payload[pos++];

                    // WAV info
                    if (pos + 4 <= len) {
                        ch.wavRate_Hz  = getU16LE(&payload[pos]); pos += 2;
                        ch.wavChannels = payload[pos++];
                        ch.wavBits     = payload[pos++];
                    }

                    // Filename
                    if (pos < len) {
                        uint8_t fnameLen = payload[pos++];
                        if (fnameLen > 0 && pos + fnameLen <= len) {
                            size_t copyLen = (fnameLen < sizeof(ch.filename) - 1)
                                           ? fnameLen : sizeof(ch.filename) - 1;
                            memcpy(ch.filename, &payload[pos], copyLen);
                            ch.filename[copyLen] = '\0';
                            pos += fnameLen;
                        }
                    }

                    _lastStatus.activeCount++;
                }
            }

            if (tag != CoreProtocol::TAG_ASYNC) {
                _resultQueue.resolve(tag, CommandResult::Ack());
            }
            if (_statusCallback) _statusCallback(_lastStatus);
            break;
        }

        default:
            break;
    }
}

// --- Playback Control ---

CommandResult HubFxAudioClient::play(uint8_t channel, const char* path,
                                      uint8_t volumePct, uint8_t output,
                                      uint8_t loopMode, uint16_t loopCount) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[7 + 127];
    payload[0] = channel;
    payload[1] = volumePct;
    payload[2] = output;
    payload[3] = loopMode;
    putU16LE(&payload[4], loopCount);
    payload[6] = (uint8_t)pathLen;
    memcpy(&payload[7], path, pathLen);

    return sendCommand(HubFxPacket::AUDIO_PLAY, payload, 7 + pathLen);
}

CommandResult HubFxAudioClient::stop(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(HubFxPacket::AUDIO_STOP, payload, 1);
}

CommandResult HubFxAudioClient::fade(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(HubFxPacket::AUDIO_FADE, payload, 1);
}

// --- Volume Control ---

CommandResult HubFxAudioClient::setVolume(uint8_t channel, uint8_t volumePct) {
    uint8_t payload[2] = { channel, volumePct };
    return sendCommand(HubFxPacket::AUDIO_VOLUME, payload, 2);
}

CommandResult HubFxAudioClient::setMasterVolume(uint8_t volumePct) {
    uint8_t payload[2] = { HubFxAudio::CH_ALL, volumePct };
    return sendCommand(HubFxPacket::AUDIO_VOLUME, payload, 2);
}

// --- Queue Control ---

CommandResult HubFxAudioClient::queueSound(uint8_t channel, const char* path,
                                            uint8_t volumePct, uint16_t loopCount,
                                            uint8_t behavior) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[6 + 127];
    payload[0] = channel;
    payload[1] = volumePct;
    putU16LE(&payload[2], loopCount);
    payload[4] = behavior;
    payload[5] = (uint8_t)pathLen;
    memcpy(&payload[6], path, pathLen);

    return sendCommand(HubFxPacket::AUDIO_QUEUE, payload, 6 + pathLen);
}

CommandResult HubFxAudioClient::queueClear(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(HubFxPacket::AUDIO_QUEUE_CLEAR, payload, 1);
}

// --- Status ---

CommandResult HubFxAudioClient::requestStatus() {
    return sendCommand(HubFxPacket::AUDIO_STATUS_REQ, nullptr, 0);
}


// ============================================================================
// ========================  STORAGE CLIENT  ==================================
// ============================================================================

void HubFxStorageClient::onModulePacket(uint8_t type, uint8_t tag,
                                         const uint8_t* payload, size_t len) {
    switch (type) {
        case HubFxPacket::CONFIG_GET_RESP: {
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

// --- Config Commands ---

CommandResult HubFxStorageClient::configReload() {
    return sendCommand(HubFxPacket::CONFIG_RELOAD, nullptr, 0);
}

CommandResult HubFxStorageClient::configGet() {
    return sendCommand(HubFxPacket::CONFIG_GET, nullptr, 0);
}

// --- SD Card Commands ---

CommandResult HubFxStorageClient::sdInit(uint8_t speed_mhz) {
    uint8_t payload[1] = { speed_mhz };
    return sendCommand(HubFxPacket::SD_INIT, payload, 1);
}

CommandResult HubFxStorageClient::sdStatus() {
    return sendCommand(HubFxPacket::SD_STATUS_REQ, nullptr, 0);
}

// --- Flash Commands ---

CommandResult HubFxStorageClient::flashStatus() {
    return sendCommand(HubFxPacket::FLASH_STATUS_REQ, nullptr, 0);
}

// --- File Operations ---

CommandResult HubFxStorageClient::fileList(const char* path, uint8_t target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127 + 1];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    size_t totalLen = 1 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = target;
    }

    return sendCommand(HubFxPacket::FILE_LIST, payload, totalLen);
}

CommandResult HubFxStorageClient::fileDelete(const char* path, uint8_t target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127 + 1];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    size_t totalLen = 1 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = target;
    }

    return sendCommand(HubFxPacket::FILE_DELETE, payload, totalLen);
}

CommandResult HubFxStorageClient::fileMkdir(const char* path, uint8_t target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127 + 1];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    size_t totalLen = 1 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = target;
    }

    return sendCommand(HubFxPacket::FILE_MKDIR, payload, totalLen);
}

CommandResult HubFxStorageClient::fileInfo(const char* path, uint8_t target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127 + 1];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    size_t totalLen = 1 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = target;
    }

    return sendCommand(HubFxPacket::FILE_INFO, payload, totalLen);
}

CommandResult HubFxStorageClient::fileDownload(const char* path, uint8_t target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[1 + 127 + 1];
    payload[0] = (uint8_t)pathLen;
    memcpy(&payload[1], path, pathLen);

    size_t totalLen = 1 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = target;
    }

    return sendCommand(HubFxPacket::FILE_DOWNLOAD, payload, totalLen);
}

// --- Upload Commands ---

CommandResult HubFxStorageClient::uploadBegin(const char* path, uint32_t size, uint8_t target) {
    size_t pathLen = strlen(path);
    if (pathLen > 127) pathLen = 127;

    uint8_t payload[4 + 1 + 127 + 1];
    putU32LE(payload, size);
    payload[4] = (uint8_t)pathLen;
    memcpy(&payload[5], path, pathLen);

    size_t totalLen = 5 + pathLen;
    if (target != HubFxStorage::TARGET_SD) {
        payload[totalLen++] = target;
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
