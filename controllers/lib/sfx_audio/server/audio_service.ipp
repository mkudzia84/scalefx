/*
 * Audio Protocol Server — Template Implementation
 *
 * All AudioServicePolicy<TMixer> method bodies. Included from audio_service.h.
 *
 * Wire formats match the audio protocol defined in
 * sfx_serial/serial/audio/audio_protocol.h (AudioPacket namespace).
 */

#include <algorithm>                       // std::min (was Arduino min() macro)
#include <serial/diag_log.h>

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)
#include "../audio/audio_asset_cache.h"   // AudioAssetCache for AUDIO_PRELOAD_STATUS_REQ
#endif

#define AUDIO_SRV_LOG(fmt, ...) SFX_LOG_DEBUG("[AudioSrv] " fmt, ##__VA_ARGS__)

// ============================================================================
// Packet Dispatch
// ============================================================================

template <MixerLike TMixer>
CommandHandleResult AudioServicePolicy<TMixer>::handle(
        uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case AudioPacket::AUDIO_PLAY:
            handlePlay(payload, len);
            return CommandHandleResult::Handled;
        case AudioPacket::AUDIO_STOP:
            handleStop(payload, len);
            return CommandHandleResult::Handled;
        case AudioPacket::AUDIO_VOLUME:
            handleVolume(payload, len);
            return CommandHandleResult::Handled;
        case AudioPacket::AUDIO_FADE:
            handleFade(payload, len);
            return CommandHandleResult::Handled;
        case AudioPacket::AUDIO_QUEUE:
            handleQueue(payload, len);
            return CommandHandleResult::Handled;
        case AudioPacket::AUDIO_QUEUE_CLEAR:
            handleQueueClear(payload, len);
            return CommandHandleResult::Handled;
        case AudioPacket::AUDIO_STATUS_REQ:
            handleStatusReq();
            return CommandHandleResult::Handled;
        case AudioPacket::AUDIO_PRELOAD_STATUS_REQ:
            handlePreloadStatusReq();
            return CommandHandleResult::Handled;
        case AudioPacket::CODEC_STATUS_REQ:
            handleCodecStatusReq();
            return CommandHandleResult::Handled;
        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// AUDIO_PLAY (0x84)
// Wire: [ch:u8][vol:u8][output:u8][loopMode:u8][loopCount:u16LE][pathLen:u8][path:str]
// ============================================================================

template <MixerLike TMixer>
void AudioServicePolicy<TMixer>::handlePlay(const uint8_t* payload, size_t len) {
    if (len < 7) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch       = payload[0];
    uint8_t volPct   = payload[1];
    uint8_t output   = payload[2];
    uint8_t loopMode = payload[3];
    uint16_t loopCnt = SfxWire::getU16LE(&payload[4]);
    uint8_t pathLen  = payload[6];

    if (ch >= AudioWire::MAX_CHANNELS) { sendNack(AudioError::INVALID_CHANNEL); return; }
    if (len < (size_t)(7 + pathLen) || pathLen == 0) {
        sendNack(SerialError::MISSING_PARAMETER); return;
    }

    char path[128];
    size_t copyLen = (pathLen < sizeof(path) - 1) ? pathLen : sizeof(path) - 1;
    memcpy(path, &payload[7], copyLen);
    path[copyLen] = '\0';

    AudioPlaybackOptions opts;
    opts.volume = volPct / 100.0f;
    opts.outputChannels = output;  // Wire format matches AudioChannel bitmask directly
    switch (loopMode) {
        case AudioWire::LOOP_INFINITE:
            opts.loop = true; opts.loopCount = LOOP_INFINITE; break;
        case AudioWire::LOOP_FINITE:
            opts.loop = true; opts.loopCount = (int)loopCnt; break;
        default:
            opts.loop = false; opts.loopCount = 0; break;
    }

    AUDIO_SRV_LOG("Play ch%d: %s vol=%d%% loop=%d", ch, path, volPct, loopMode);

    if (mixer().playAsync(ch, path, opts)) {
        sendAck();
    } else {
        SFX_LOG_ERROR("[AudioSrv] playAsync failed ch%d: %s", ch, path);
        sendNack(AudioError::AUDIO_FAILURE);
    }
}

// ============================================================================
// AUDIO_STOP (0x85)
// Wire: [ch:u8] (0xFF = all channels)
// ============================================================================

template <MixerLike TMixer>
void AudioServicePolicy<TMixer>::handleStop(const uint8_t* payload, size_t len) {
    if (len < 1) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch = payload[0];
    if (ch == AudioWire::CH_ALL) {
        mixer().stopAsync(-1, AudioStopMode::Immediate);
    } else if (ch < AudioWire::MAX_CHANNELS) {
        mixer().stopAsync(ch, AudioStopMode::Immediate);
    } else {
        sendNack(AudioError::INVALID_CHANNEL);
        return;
    }
    sendAck();
}

// ============================================================================
// AUDIO_VOLUME (0x86)
// Wire: [ch:u8][vol:u8]  (ch 0xFF = master volume, 0-7 = channel, vol 0-100)
// ============================================================================

template <MixerLike TMixer>
void AudioServicePolicy<TMixer>::handleVolume(const uint8_t* payload, size_t len) {
    if (len < 2) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch  = payload[0];
    uint8_t vol = payload[1];
    float volume = vol / 100.0f;

    if (ch == AudioWire::CH_ALL) {
        mixer().setMasterVolumeAsync(volume);
    } else if (ch < AudioWire::MAX_CHANNELS) {
        mixer().setVolumeAsync(ch, volume);
    } else {
        sendNack(AudioError::INVALID_CHANNEL);
        return;
    }
    sendAck();
}

// ============================================================================
// AUDIO_FADE (0x87)
// Wire: [ch:u8]
// ============================================================================

template <MixerLike TMixer>
void AudioServicePolicy<TMixer>::handleFade(const uint8_t* payload, size_t len) {
    if (len < 1) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch = payload[0];
    if (ch >= AudioWire::MAX_CHANNELS) { sendNack(AudioError::INVALID_CHANNEL); return; }

    mixer().stopAsync(ch, AudioStopMode::Fade);
    sendAck();
}

// ============================================================================
// AUDIO_QUEUE (0x88)
// Wire: [ch:u8][vol:u8][loopCount:u16LE][behavior:u8][pathLen:u8][path:str]
// ============================================================================

template <MixerLike TMixer>
void AudioServicePolicy<TMixer>::handleQueue(const uint8_t* payload, size_t len) {
    if (len < 6) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch       = payload[0];
    uint8_t volPct   = payload[1];
    uint16_t loopCnt = SfxWire::getU16LE(&payload[2]);
    uint8_t behavior = payload[4];
    uint8_t pathLen  = payload[5];

    if (ch >= AudioWire::MAX_CHANNELS) { sendNack(AudioError::INVALID_CHANNEL); return; }
    if (len < (size_t)(6 + pathLen) || pathLen == 0) {
        sendNack(SerialError::MISSING_PARAMETER); return;
    }

    char path[128];
    size_t copyLen = (pathLen < sizeof(path) - 1) ? pathLen : sizeof(path) - 1;
    memcpy(path, &payload[6], copyLen);
    path[copyLen] = '\0';

    AudioPlaybackOptions opts;
    opts.volume = volPct / 100.0f;
    opts.outputChannels = AudioChannel::ALL;  // Queue always uses all channels
    if (loopCnt > 0) {
        opts.loop = true;
        opts.loopCount = (int)loopCnt;
    } else {
        opts.loop = false;
        opts.loopCount = 0;
    }

    QueueLoopBehavior qBehavior = (behavior == AudioWire::QUEUE_STOP_NOW)
        ? QueueLoopBehavior::StopImmediate
        : QueueLoopBehavior::FinishLoop;

    AUDIO_SRV_LOG("Queue ch%d: %s vol=%d%% loops=%d behavior=%d",
                  ch, path, volPct, loopCnt, behavior);

    if (mixer().queueSoundAsync(ch, path, opts, qBehavior)) {
        sendAck();
    } else {
        sendNack(AudioError::AUDIO_FAILURE);
    }
}

// ============================================================================
// AUDIO_QUEUE_CLEAR (0x89)
// Wire: [ch:u8] (0xFF = all channels)
// ============================================================================

template <MixerLike TMixer>
void AudioServicePolicy<TMixer>::handleQueueClear(const uint8_t* payload, size_t len) {
    if (len < 1) { sendNack(SerialError::MISSING_PARAMETER); return; }

    uint8_t ch = payload[0];
    if (ch == AudioWire::CH_ALL) {
        mixer().clearQueueAsync(-1);
    } else if (ch < AudioWire::MAX_CHANNELS) {
        mixer().clearQueueAsync(ch);
    } else {
        sendNack(AudioError::INVALID_CHANNEL);
        return;
    }
    sendAck();
}

// ============================================================================
// AUDIO_STATUS_REQ (0x8A) → AUDIO_STATUS_RESP (0x8B)
//
// Response wire format (v4):
//   System header:
//     [masterVol:u8][flags:u8][sampleRate:u16LE][bitDepth:u8][maxChannels:u8]
//     [codecNameLen:u8][codecName:str]
//   Ring buffer stats (if flags bit 3):
//     [ringFillPct:u8][availRead:u16LE][availWrite:u16LE]
//     [underruns:u32LE][consumeLoops:u32LE][consumeFrames:u32LE]
//   Buffer capacities (if flags bit 4 — v4):
//     [wavBufCapacity:u16LE]     // WAV_BUF_FRAMES
//     [ringCapacity:u16LE]       // RING_FRAMES
//   Channel data:
//     [activeMask:u8]
//     per-active-channel:
//       [ch:u8][vol:u8][playing:u8][looping:u8][loopCount:u16LE]
//       [remaining_ms:u32LE][queueLen:u8][output:u8]
//       [wavRate:u16LE][wavChannels:u8][wavBits:u8]
//       [wavBufFillPct:u8]               // v4: per-channel WAV buffer fill %
//       [filenameLen:u8][filename:str]
// ============================================================================

template <MixerLike TMixer>
void AudioServicePolicy<TMixer>::handleStatusReq() {
    auto& m = mixer();

    uint8_t buf[512];
    size_t pos = 0;

    // Master volume (0-100)
    buf[pos++] = (uint8_t)(m.masterVolume() * 100.0f);

    // Flags: [bit0:initialized][bit1:i2sRunning][bit2:hasCodec][bit3:hasRingStats][bit4:hasBufferCaps]
    uint8_t flags = 0;
    if (m.isInitialized()) flags |= 0x01;
    if (m.isI2SRunning())  flags |= 0x02;
    flags |= 0x04;  // always have codec (template parameter)
    flags |= 0x08;  // v3: always include ring buffer stats
    flags |= 0x10;  // v4: include buffer capacities
    buf[pos++] = flags;

    // Audio format
    SfxWire::putU16LE(&buf[pos], AUDIO_SAMPLE_RATE); pos += 2;
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
    SfxWire::putU16LE(&buf[pos],
        (uint16_t)std::min(m.getRingAvailableRead(), (uint32_t)65535)); pos += 2;
    SfxWire::putU16LE(&buf[pos],
        (uint16_t)std::min(m.getRingAvailableWrite(), (uint32_t)65535)); pos += 2;
    SfxWire::putU32LE(&buf[pos], m.getUnderruns()); pos += 4;
    SfxWire::putU32LE(&buf[pos], m.getConsumeLoops()); pos += 4;
    SfxWire::putU32LE(&buf[pos], m.getConsumeFrames()); pos += 4;

    // Buffer capacities (v4)
    SfxWire::putU16LE(&buf[pos], (uint16_t)WAV_BUF_FRAMES); pos += 2;
    SfxWire::putU16LE(&buf[pos], (uint16_t)RING_FRAMES);   pos += 2;

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
        SfxWire::putU16LE(&buf[pos], (uint16_t)m.getLoopCount(i)); pos += 2;

        float remSec = m.remainingSec(i);
        uint32_t remaining_ms = (remSec >= 0.0f) ? (uint32_t)(remSec * 1000.0f) : 0;
        SfxWire::putU32LE(&buf[pos], remaining_ms); pos += 4;

        buf[pos++] = (uint8_t)m.queueLength(i);
        buf[pos++] = m.getOutputChannels(i);

        // WAV format info
        SfxWire::putU16LE(&buf[pos], (uint16_t)m.getSampleRate(i)); pos += 2;
        buf[pos++] = (uint8_t)m.getNumChannels(i);
        buf[pos++] = (uint8_t)m.getBitsPerSample(i);

        // WAV decode buffer fill % (v4)
        buf[pos++] = (uint8_t)m.getWavBufferFillPercent(i);

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

    sendRawPacket(AudioPacket::AUDIO_STATUS_RESP, currentTag(), buf, pos);
}

// ============================================================================
// CODEC_STATUS_REQ (0xAA) → CODEC_STATUS_RESP (0xAB)
// Wire: [codecType:u8][initialized:u8][i2cOk:u8][sdaPin:u8][sclPin:u8]
//       [supplyMode:u8][muted:u8][digitalVol:u8][deviceCtrl:u8][faultStatus:u8]
//       [codecNameLen:u8][codecName:str]
//       + power-telemetry tail (Rule 11 append, 2026-08-01):
//       [pvdd_mV:u16LE][againReg:u8][dieId:u8][outPeak:u16LE]
// supplyMode always reports 4 ("auto") — the manual codec_supply config
// was retired; the drivers auto-detect the rail via PVDD_ADC.
// ============================================================================

template <MixerLike TMixer>
void AudioServicePolicy<TMixer>::handleCodecStatusReq() {
    auto& codec = mixer().getCodec();
    uint8_t buf[80];
    size_t pos = 0;

    buf[pos++] = codec.getCodecType();         // 1=TAS5825M, 2=TAS5825P
    buf[pos++] = codec.isInitialized() ? 1 : 0;
    buf[pos++] = codec.testI2CConnection() ? 1 : 0;
    buf[pos++] = (uint8_t)(codec.getSdaPin() & 0xFF);
    buf[pos++] = (uint8_t)(codec.getSclPin() & 0xFF);

    buf[pos++] = sfx_audio::tas5825::SUPPLY_CODE_AUTO;
    buf[pos++] = codec.getMuted() ? 1 : 0;
    buf[pos++] = codec.getVolumeRegister();
    buf[pos++] = codec.getDeviceControlRegister();
    buf[pos++] = codec.getFaultRegister();

    // Codec model name (length-prefixed)
    const char* name = codec.getModelName();
    uint8_t nameLen = name ? (uint8_t)strlen(name) : 0;
    if (nameLen > 48) nameLen = 48;
    buf[pos++] = nameLen;
    if (nameLen > 0) {
        memcpy(&buf[pos], name, nameLen);
        pos += nameLen;
    }

    // Power-telemetry tail: live PVDD (chip's own ADC), the auto-chosen
    // analog gain step, silicon DIE_ID, and the mixed-output peak since
    // the last query (0..32767) — the client computes a watts estimate
    // from these plus speaker impedance.
    uint32_t pvdd = codec.readPvdd_mV();
    if (pvdd > 0xFFFF) pvdd = 0xFFFF;
    buf[pos++] = (uint8_t)(pvdd & 0xFF);
    buf[pos++] = (uint8_t)(pvdd >> 8);
    buf[pos++] = codec.getAgainRegister();
    buf[pos++] = codec.getDieId();
    uint16_t peak = mixer().outputPeakSinceLastRead();
    buf[pos++] = (uint8_t)(peak & 0xFF);
    buf[pos++] = (uint8_t)(peak >> 8);

    sendRawPacket(AudioPacket::CODEC_STATUS_RESP, currentTag(), buf, pos);
}


// ============================================================================
// AUDIO_PRELOAD_STATUS_REQ (0xE2) → AUDIO_PRELOAD_STATUS_RESP (0xE3)
//
// Snapshot of the AudioAssetCache: header counts + one record per
// populated entry.  Pure query — no mutation.  Payload layout
// documented in serial/audio/audio_protocol.h.  Pico build (no asset
// cache) NACKs with NOT_SUPPORTED so the client gets a clean error
// rather than a silent timeout.
// ============================================================================

template <MixerLike TMixer>
void AudioServicePolicy<TMixer>::handlePreloadStatusReq() {
#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)
    // Build the payload into a stack buffer.  Worst case sizing:
    //   16 B header
    // + kAssetCacheMaxEntries (64) × (1 + 160 path + 4 + 4 + 3 + 4×(1+24)) ≈ 64 × 276 = 17.6 KB
    // …too big for a single COBS packet (512 B limit).  We send at most
    // the first 16 entries in one packet — sufficient for every
    // realistic hub config (alerts 5 + engine 3 + gun 2-8 = 10-16);
    // headers report total counts so the client knows if any were
    // truncated.  A future "paged" variant can add a request offset.

    auto& cache = AudioAssetCache::instance();
    const auto stats = cache.stats();

    uint8_t buf[512];
    size_t  pos = 0;

    // Header (16 B fixed)
    auto putU16 = [&](uint16_t v) { SfxWire::putU16LE(buf + pos, v); pos += 2; };
    auto putU32 = [&](uint32_t v) { SfxWire::putU32LE(buf + pos, v); pos += 4; };
    putU32(stats.residentBytes);
    putU32((uint32_t)kAssetCacheBudgetBytes);
    putU16((uint16_t)(stats.entries - stats.inFlightEntries - stats.failedEntries));   // ready
    putU16((uint16_t)stats.inFlightEntries);
    putU16((uint16_t)stats.failedEntries);
    putU16((uint16_t)stats.pinnedEntries);

    // Per-entry records — visitor callback writes into `buf` via captured
    // pos.  Stop early if we'd overflow the COBS frame (~480 bytes safe).
    struct Ctx {
        uint8_t* buf;
        size_t*  pos;
        size_t   cap;
    } ctx{buf, &pos, sizeof(buf) - 4};

    cache.enumeratePreloads([](const AudioAssetCache::PreloadInfo& info,
                               void* userData) -> bool {
        auto* c = static_cast<Ctx*>(userData);
        // Compute record size first so we can early-out if it won't fit.
        const size_t pathLen = strnlen(info.path, kAssetPathMax);
        size_t rec = 1 + pathLen + 4 + 4 + 1 + 1 + 1;
        for (int i = 0; i < kMaxOwnersPerEntry; ++i) {
            const char* n = info.ownerNames[i];
            if (!n) continue;
            rec += 1 + strlen(n);
        }
        if (*c->pos + rec > c->cap) return false;   // stop iteration

        c->buf[(*c->pos)++] = (uint8_t)pathLen;
        memcpy(c->buf + *c->pos, info.path, pathLen);
        *c->pos += pathLen;
        SfxWire::putU32LE(c->buf + *c->pos, info.totalBytes);  *c->pos += 4;
        SfxWire::putU32LE(c->buf + *c->pos, info.loadedBytes); *c->pos += 4;
        c->buf[(*c->pos)++] = (uint8_t)info.status;
        c->buf[(*c->pos)++] = (uint8_t)info.format;

        uint8_t ownerCount = 0;
        const size_t ownerCountPos = (*c->pos)++;
        for (int i = 0; i < kMaxOwnersPerEntry; ++i) {
            const char* n = info.ownerNames[i];
            if (!n) continue;
            const uint8_t nameLen = (uint8_t)strlen(n);
            c->buf[(*c->pos)++] = nameLen;
            memcpy(c->buf + *c->pos, n, nameLen);
            *c->pos += nameLen;
            ++ownerCount;
        }
        c->buf[ownerCountPos] = ownerCount;
        return true;
    }, &ctx);

    sendRawPacket(AudioPacket::AUDIO_PRELOAD_STATUS_RESP, currentTag(), buf, pos);
#else
    sendNack(SerialError::NOT_SUPPORTED);
#endif
}
