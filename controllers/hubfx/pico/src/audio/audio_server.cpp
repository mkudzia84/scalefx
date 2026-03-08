/*
 * Audio Server Implementation
 *
 * Handles audio playback commands (0x84-0x8B):
 *   play, stop, volume, fade, queue, queue clear, status
 */

#include "audio_server.h"
#include "audio_mixer.h"
#include "audio_codec.h"
#include "audio_config.h"

using namespace CoreProtocol;

// ============================================================================
// handleModulePacket — Audio Commands (0x84-0x8B)
// ============================================================================

CommandHandleResult AudioServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
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

// ============================================================================
// Audio Command Handlers
// ============================================================================

void AudioServer::handlePlay(const uint8_t* payload, size_t len) {
    // Wire format: [ch:u8][vol:u8][output:u8][loopMode:u8][loopCount:u16LE][pathLen:u8][path:str]
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

    // Extract filename (null-terminate)
    char path[128];
    size_t copyLen = (pathLen < sizeof(path) - 1) ? pathLen : sizeof(path) - 1;
    memcpy(path, &payload[7], copyLen);
    path[copyLen] = '\0';

    // Build playback options
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

    MIXER_LOG("Play request: ch%d path=%s vol=%d%% loop=%d", ch, path, volPct, loopMode);

    if (_mixer->playAsync(ch, path, opts)) {
        sendAck();
    } else {
        MIXER_ERROR("playAsync failed for ch%d: %s", ch, path);
        sendNack(HubFxError::AUDIO_ERROR);
    }
}

void AudioServer::handleStop(const uint8_t* payload, size_t len) {
    // Wire format: [ch:u8] (0xFF=all)
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

void AudioServer::handleVolume(const uint8_t* payload, size_t len) {
    // Wire format: [ch:u8][vol:u8] (ch 0xFF=master, 0-7=channel, vol 0-100)
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

void AudioServer::handleFade(const uint8_t* payload, size_t len) {
    // Wire format: [ch:u8]
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

void AudioServer::handleQueue(const uint8_t* payload, size_t len) {
    // Wire format: [ch:u8][vol:u8][loopCount:u16LE][behavior:u8][pathLen:u8][path:str]
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

void AudioServer::handleQueueClear(const uint8_t* payload, size_t len) {
    // Wire format: [ch:u8] (0xFF=all)
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

void AudioServer::handleStatusReq() {
    if (!_mixer) {
        sendNack(HubFxError::AUDIO_ERROR);
        return;
    }

    // Build response (v2 extended format):
    //   Header:
    //     [masterVol:u8][flags:u8][sampleRate_Hz:u16LE][bitDepth:u8]
    //     [maxChannels:u8][codecNameLen:u8][codecName:str]
    //   Per-channel (only active):
    //     [activeMask:u8]
    //     [ch:u8][vol:u8][playing:u8][looping:u8][loopCount:u16LE]
    //     [remaining_ms:u32LE][queueLen:u8][output:u8]
    //     [wavRate_Hz:u16LE][wavCh:u8][wavBits:u8]
    //     [filenameLen:u8][filename:str]

    uint8_t buf[512];
    size_t pos = 0;

    // --- System header ---
    buf[pos++] = (uint8_t)(_mixer->masterVolume() * 100.0f);  // masterVol 0-100

    // Flags: bit0=initialized, bit1=i2sRunning, bit2=hasCodec
    uint8_t flags = 0;
    if (_mixer->isInitialized())  flags |= 0x01;
    if (_mixer->isI2SRunning())   flags |= 0x02;
    if (_mixer->getCodec())       flags |= 0x04;
    buf[pos++] = flags;

    // I2S config
    putU16LE(&buf[pos], AUDIO_SAMPLE_RATE);  // sampleRate_Hz
    pos += 2;
    buf[pos++] = AUDIO_BIT_DEPTH;            // bitDepth
    buf[pos++] = AUDIO_MAX_CHANNELS;         // maxChannels

    // Codec name (length-prefixed)
    const char* codecName = _mixer->getCodec() ? _mixer->getCodec()->getModelName() : "";
    uint8_t codecLen = (uint8_t)strlen(codecName);
    buf[pos++] = codecLen;
    if (codecLen > 0) {
        memcpy(&buf[pos], codecName, codecLen);
        pos += codecLen;
    }

    // --- Per-channel data ---
    uint8_t activeMask = 0;
    for (int i = 0; i < 8; i++) {
        if (_mixer->isPlaying(i) || _mixer->queueLength(i) > 0) {
            activeMask |= (1 << i);
        }
    }
    buf[pos++] = activeMask;

    for (int i = 0; i < 8; i++) {
        if (!(activeMask & (1 << i))) continue;
        if (pos > sizeof(buf) - 64) break;  // safety margin for filename

        buf[pos++] = (uint8_t)i;                                         // ch
        buf[pos++] = (uint8_t)(_mixer->getChannelVolume(i) * 100.0f);    // vol 0-100
        buf[pos++] = _mixer->isPlaying(i) ? 1 : 0;                      // playing
        buf[pos++] = _mixer->isLooping(i) ? 1 : 0;                      // looping
        putU16LE(&buf[pos], (uint16_t)_mixer->getLoopCount(i));          // loopCount
        pos += 2;
        uint32_t remaining = (uint32_t)max(_mixer->remainingMs(i), 0);    // remaining_ms
        putU32LE(&buf[pos], remaining);
        pos += 4;
        buf[pos++] = (uint8_t)_mixer->queueLength(i);                    // queueLen
        buf[pos++] = (uint8_t)_mixer->getOutput(i);                      // output

        // WAV format info
        putU16LE(&buf[pos], (uint16_t)_mixer->getSampleRate(i));          // wavRate_Hz
        pos += 2;
        buf[pos++] = (uint8_t)_mixer->getNumChannels(i);                  // wavCh
        buf[pos++] = (uint8_t)_mixer->getBitsPerSample(i);                // wavBits

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
