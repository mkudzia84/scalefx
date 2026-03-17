/*
 * Audio Protocol Server — Template Implementation
 *
 * All AudioServerT<TMixer> method bodies. Included from audio_server.h.
 *
 * Wire formats match the HubFX audio protocol defined in
 * sfx_serial/serial/hubfx/hubfx.h (HubFxPacket namespace).
 */

#include <platform/diag_log.h>

#define AUDIO_SRV_LOG(fmt, ...) SFX_LOG_DEBUG("[AudioSrv] " fmt, ##__VA_ARGS__)

// ============================================================================
// Packet Dispatch
// ============================================================================

template <typename TMixer>
CommandHandleResult AudioServerT<TMixer>::handleModulePacket(
        uint8_t type, const uint8_t* payload, size_t len) {
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
// AUDIO_PLAY (0x84)
// Wire: [ch:u8][vol:u8][output:u8][loopMode:u8][loopCount:u16LE][pathLen:u8][path:str]
// ============================================================================

template <typename TMixer>
void AudioServerT<TMixer>::handlePlay(const uint8_t* payload, size_t len) {
    if (len < 7) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch       = payload[0];
    uint8_t volPct   = payload[1];
    uint8_t output   = payload[2];
    uint8_t loopMode = payload[3];
    uint16_t loopCnt = CoreProtocol::getU16LE(&payload[4]);
    uint8_t pathLen  = payload[6];

    if (ch >= HubFxAudio::MAX_CHANNELS) { sendNack(HubFxError::INVALID_CHANNEL); return; }
    if (len < (size_t)(7 + pathLen) || pathLen == 0) {
        sendNack(SerialError::MISSING_PARAMETER); return;
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
            opts.loop = true; opts.loopCount = LOOP_INFINITE; break;
        case HubFxAudio::LOOP_FINITE:
            opts.loop = true; opts.loopCount = (int)loopCnt; break;
        default:
            opts.loop = false; opts.loopCount = 0; break;
    }

    AUDIO_SRV_LOG("Play ch%d: %s vol=%d%% loop=%d", ch, path, volPct, loopMode);

    if (mixer().playAsync(ch, path, opts)) {
        sendAck();
    } else {
        SFX_LOG_ERROR("[AudioSrv] playAsync failed ch%d: %s", ch, path);
        sendNack(HubFxError::AUDIO_ERROR);
    }
}

// ============================================================================
// AUDIO_STOP (0x85)
// Wire: [ch:u8] (0xFF = all channels)
// ============================================================================

template <typename TMixer>
void AudioServerT<TMixer>::handleStop(const uint8_t* payload, size_t len) {
    if (len < 1) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch = payload[0];
    if (ch == HubFxAudio::CH_ALL) {
        mixer().stopAsync(-1, AudioStopMode::Immediate);
    } else if (ch < HubFxAudio::MAX_CHANNELS) {
        mixer().stopAsync(ch, AudioStopMode::Immediate);
    } else {
        sendNack(HubFxError::INVALID_CHANNEL);
        return;
    }
    sendAck();
}

// ============================================================================
// AUDIO_VOLUME (0x86)
// Wire: [ch:u8][vol:u8]  (ch 0xFF = master volume, 0-7 = channel, vol 0-100)
// ============================================================================

template <typename TMixer>
void AudioServerT<TMixer>::handleVolume(const uint8_t* payload, size_t len) {
    if (len < 2) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch  = payload[0];
    uint8_t vol = payload[1];
    float volume = vol / 100.0f;

    if (ch == HubFxAudio::CH_ALL) {
        mixer().setMasterVolumeAsync(volume);
    } else if (ch < HubFxAudio::MAX_CHANNELS) {
        mixer().setVolumeAsync(ch, volume);
    } else {
        sendNack(HubFxError::INVALID_CHANNEL);
        return;
    }
    sendAck();
}

// ============================================================================
// AUDIO_FADE (0x87)
// Wire: [ch:u8]
// ============================================================================

template <typename TMixer>
void AudioServerT<TMixer>::handleFade(const uint8_t* payload, size_t len) {
    if (len < 1) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch = payload[0];
    if (ch >= HubFxAudio::MAX_CHANNELS) { sendNack(HubFxError::INVALID_CHANNEL); return; }

    mixer().stopAsync(ch, AudioStopMode::Fade);
    sendAck();
}

// ============================================================================
// AUDIO_QUEUE (0x88)
// Wire: [ch:u8][vol:u8][loopCount:u16LE][behavior:u8][pathLen:u8][path:str]
// ============================================================================

template <typename TMixer>
void AudioServerT<TMixer>::handleQueue(const uint8_t* payload, size_t len) {
    if (len < 6) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch       = payload[0];
    uint8_t volPct   = payload[1];
    uint16_t loopCnt = CoreProtocol::getU16LE(&payload[2]);
    uint8_t behavior = payload[4];
    uint8_t pathLen  = payload[5];

    if (ch >= HubFxAudio::MAX_CHANNELS) { sendNack(HubFxError::INVALID_CHANNEL); return; }
    if (len < (size_t)(6 + pathLen) || pathLen == 0) {
        sendNack(SerialError::MISSING_PARAMETER); return;
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

    AUDIO_SRV_LOG("Queue ch%d: %s vol=%d%% loops=%d behavior=%d",
                  ch, path, volPct, loopCnt, behavior);

    if (mixer().queueSoundAsync(ch, path, opts, qBehavior)) {
        sendAck();
    } else {
        sendNack(HubFxError::AUDIO_ERROR);
    }
}

// ============================================================================
// AUDIO_QUEUE_CLEAR (0x89)
// Wire: [ch:u8] (0xFF = all channels)
// ============================================================================

template <typename TMixer>
void AudioServerT<TMixer>::handleQueueClear(const uint8_t* payload, size_t len) {
    if (len < 1) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch = payload[0];
    if (ch == HubFxAudio::CH_ALL) {
        mixer().clearQueueAsync(-1);
    } else if (ch < HubFxAudio::MAX_CHANNELS) {
        mixer().clearQueueAsync(ch);
    } else {
        sendNack(HubFxError::INVALID_CHANNEL);
        return;
    }
    sendAck();
}

// ============================================================================
// AUDIO_STATUS_REQ (0x8A) → AUDIO_STATUS_RESP (0x8B)
//
// Response wire format (v3):
//   System header:
//     [masterVol:u8][flags:u8][sampleRate:u16LE][bitDepth:u8][maxChannels:u8]
//     [codecNameLen:u8][codecName:str]
//   Ring buffer stats (if flags bit 3):
//     [ringFillPct:u8][availRead:u16LE][availWrite:u16LE]
//     [underruns:u32LE][consumeLoops:u32LE][consumeFrames:u32LE]
//   Channel data:
//     [activeMask:u8]
//     per-active-channel:
//       [ch:u8][vol:u8][playing:u8][looping:u8][loopCount:u16LE]
//       [remaining_ms:u32LE][queueLen:u8][output:u8]
//       [wavRate:u16LE][wavChannels:u8][wavBits:u8]
//       [filenameLen:u8][filename:str]
// ============================================================================

template <typename TMixer>
void AudioServerT<TMixer>::handleStatusReq() {
    auto& m = mixer();

    uint8_t buf[512];
    size_t pos = 0;

    // Master volume (0-100)
    buf[pos++] = (uint8_t)(m.masterVolume() * 100.0f);

    // Flags: [bit0:initialized][bit1:i2sRunning][bit2:hasCodec][bit3:hasRingStats]
    uint8_t flags = 0;
    if (m.isInitialized()) flags |= 0x01;
    if (m.isI2SRunning())  flags |= 0x02;
    flags |= 0x04;  // always have codec (template parameter)
    flags |= 0x08;  // v3: always include ring buffer stats
    buf[pos++] = flags;

    // Audio format
    CoreProtocol::putU16LE(&buf[pos], AUDIO_SAMPLE_RATE); pos += 2;
    buf[pos++] = AUDIO_BIT_DEPTH;
    buf[pos++] = AUDIO_MAX_CHANNELS;

    // Codec name (length-prefixed string)
    const char* codecName = m.getCodec().getModelName();
    uint8_t codecLen = codecName ? (uint8_t)strlen(codecName) : 0;
    if (codecLen > 31) codecLen = 31;
    buf[pos++] = codecLen;
    if (codecLen > 0) {
        memcpy(&buf[pos], codecName, codecLen);
        pos += codecLen;
    }

    // Ring buffer stats (v3)
    buf[pos++] = (uint8_t)m.getRingFillPercent();
    CoreProtocol::putU16LE(&buf[pos],
        (uint16_t)min(m.getRingAvailableRead(), (uint32_t)65535)); pos += 2;
    CoreProtocol::putU16LE(&buf[pos],
        (uint16_t)min(m.getRingAvailableWrite(), (uint32_t)65535)); pos += 2;
    CoreProtocol::putU32LE(&buf[pos], m.getUnderruns()); pos += 4;
    CoreProtocol::putU32LE(&buf[pos], m.getConsumeLoops()); pos += 4;
    CoreProtocol::putU32LE(&buf[pos], m.getConsumeFrames()); pos += 4;

    // Active channel bitmask — include channels that are playing OR have queued sounds
    uint8_t activeMask = 0;
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (m.isPlaying(i) || m.queueLength(i) > 0) {
            activeMask |= (1 << i);
        }
    }
    buf[pos++] = activeMask;

    // Per-active-channel data
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (!(activeMask & (1 << i))) continue;
        if (pos > sizeof(buf) - 64) break;  // safety margin

        buf[pos++] = (uint8_t)i;
        buf[pos++] = (uint8_t)(m.getChannelVolume(i) * 100.0f);
        buf[pos++] = m.isPlaying(i) ? 1 : 0;
        buf[pos++] = m.isLooping(i) ? 1 : 0;
        CoreProtocol::putU16LE(&buf[pos], (uint16_t)m.getLoopCount(i)); pos += 2;

        uint32_t remaining = (uint32_t)max(m.remainingMs(i), 0);
        CoreProtocol::putU32LE(&buf[pos], remaining); pos += 4;

        buf[pos++] = (uint8_t)m.queueLength(i);
        buf[pos++] = (uint8_t)m.getOutput(i);

        // WAV format info
        CoreProtocol::putU16LE(&buf[pos], (uint16_t)m.getSampleRate(i)); pos += 2;
        buf[pos++] = (uint8_t)m.getNumChannels(i);
        buf[pos++] = (uint8_t)m.getBitsPerSample(i);

        // Filename (length-prefixed)
        const char* fname = m.getFilename(i);
        uint8_t fnameLen = fname ? (uint8_t)strlen(fname) : 0;
        if (fnameLen > 127) fnameLen = 127;
        buf[pos++] = fnameLen;
        if (fnameLen > 0) {
            memcpy(&buf[pos], fname, fnameLen);
            pos += fnameLen;
        }
    }

    sendRawPacket(HubFxPacket::AUDIO_STATUS_RESP, currentTag(), buf, pos);
}
