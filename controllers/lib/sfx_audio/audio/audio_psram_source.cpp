/*
 * audio_psram_source.cpp — WAV-from-PSRAM impl.
 *
 *  Hot path (readFrames):
 *      1. If asset failed → mark exhausted, return 0.
 *      2. If not enough loaded yet → return 0 (mixer covers gap).
 *      3. Decode min(want, dataLeft, loadedAhead) frames.
 *      4. On EOF: handle loop wrap (offset reset + loopCount--), else
 *         mark exhausted.
 */

#include "audio_psram_source.h"

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)

#include <cstring>
#include <Arduino.h>            // millis()
#include <serial/diag_log.h>

#define PSRAM_LOG(fmt, ...)   SFX_LOG_INFO ("[PsramSrc] " fmt, ##__VA_ARGS__)
#define PSRAM_WARN(fmt, ...)  SFX_LOG_WARN ("[PsramSrc] " fmt, ##__VA_ARGS__)
#define PSRAM_ERROR(fmt, ...) SFX_LOG_ERROR("[PsramSrc] " fmt, ##__VA_ARGS__)


// ── In-memory WAV header parser (independent of SdFile) ─────────────
//
// Identical semantics to parseWavHeaderLocked() in audio_source.cpp,
// but operates on a byte buffer.  Returns false on any mismatch.
namespace {

bool parseWavHeaderInMemory(const uint8_t* buf, uint32_t bufBytes,
                            uint32_t& outSampleRate_Hz,
                            uint16_t& outNumChannels,
                            uint16_t& outBitsPerSample,
                            uint32_t& outDataStart,
                            uint32_t& outDataBytes,
                            uint32_t& outTotalFrames) {
    if (bufBytes < 44) return false;
    if (memcmp(buf, "RIFF", 4) != 0)       return false;
    if (memcmp(buf + 8,  "WAVE", 4) != 0)  return false;
    if (memcmp(buf + 12, "fmt ", 4) != 0)  return false;

    const uint16_t audioFormat = buf[20] | (buf[21] << 8);
    if (audioFormat != 1) return false;     // PCM only

    outNumChannels   = buf[22] | (buf[23] << 8);
    outSampleRate_Hz = buf[24] | (buf[25] << 8) |
                       (buf[26] << 16) | (buf[27] << 24);
    outBitsPerSample = buf[34] | (buf[35] << 8);
    if (outNumChannels < 1 || outNumChannels > 2)           return false;
    if (outBitsPerSample != 8 && outBitsPerSample != 16)    return false;

    // Walk chunks starting at offset 12 to find "data".
    uint32_t pos = 12;
    while (pos + 8 <= bufBytes) {
        const uint32_t chunkSize = buf[pos+4] | (buf[pos+5] << 8) |
                                   (buf[pos+6] << 16) | (buf[pos+7] << 24);
        if (memcmp(buf + pos, "data", 4) == 0) {
            outDataStart   = pos + 8;
            outDataBytes   = chunkSize;
            const uint32_t bpf = outNumChannels * (outBitsPerSample / 8);
            outTotalFrames = bpf ? (chunkSize / bpf) : 0;
            return true;
        }
        pos += 8 + chunkSize;
    }
    return false;
}

}   // anon


// ── WavPsramSource ──────────────────────────────────────────────────

bool WavPsramSource::open(const char* path) {
    _asset = AudioAssetCache::instance().acquire(path);
    if (!_asset.isValid()) {
        PSRAM_ERROR("acquire failed: %s", path);
        return false;
    }
    if (_asset.format() != AssetFormat::Wav) {
        PSRAM_ERROR("not a .wav file: %s", path);
        _asset.reset();
        return false;
    }

    // Block briefly on enough head bytes (≥ 4 KB) to parse the header
    // robustly — typical RIFF/fmt/data chunk layout fits in ~50 bytes,
    // but data chunks can be preceded by 'LIST', 'JUNK', etc., so we
    // wait for a comfortable margin.
    constexpr uint32_t kHeaderHeadBytes = 4 * 1024;
    const uint32_t waitStart = millis();
    while (!_asset.hasHead(kHeaderHeadBytes) && !_asset.isFailed()) {
        if (millis() - waitStart > 2000) {
            PSRAM_ERROR("head-bytes wait timeout: %s (loaded=%u)",
                        path, (unsigned)_asset.loadedBytes());
            _asset.reset();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (_asset.isFailed()) {
        PSRAM_ERROR("asset load failed: %s", path);
        _asset.reset();
        return false;
    }

    if (!parseWavHeaderInMemory(_asset.data(), _asset.loadedBytes(),
                                _sampleRate_Hz, _numChannels,
                                _bitsPerSample, _dataStart,
                                _dataBytes, _totalFrames)) {
        PSRAM_ERROR("WAV header parse failed: %s", path);
        _asset.reset();
        return false;
    }

    _bytesPerFrame = (uint8_t)(_numChannels * (_bitsPerSample / 8));
    if (_bytesPerFrame == 0) {
        PSRAM_ERROR("invalid format: %s (bpf=0)", path);
        _asset.reset();
        return false;
    }

    _readBytes     = _dataStart;
    _framesRead    = 0;
    _exhausted     = false;
    _loopCount     = _loopCountInit;
    _open          = true;

    PSRAM_LOG("opened %s — %uHz/%uch/%ub, %u frames, %u KB (loaded %u%%)",
              path,
              (unsigned)_sampleRate_Hz, (unsigned)_numChannels,
              (unsigned)_bitsPerSample, (unsigned)_totalFrames,
              (unsigned)(_asset.totalBytes() / 1024),
              (unsigned)(_asset.loadedBytes() * 100u /
                         (_asset.totalBytes() ? _asset.totalBytes() : 1)));
    return true;
}

WavPsramSource::~WavPsramSource() {
    _asset.reset();
    _open = false;
}

uint32_t WavPsramSource::readFrames(float* outL, float* outR,
                                    uint32_t maxFrames) {
    if (!_open || _exhausted) return 0;
    if (_asset.isFailed()) { _exhausted = true; return 0; }

    uint32_t delivered = 0;
    while (delivered < maxFrames) {
        const uint32_t loaded = _asset.loadedBytes();
        // Bytes available from current read position up to either
        // the end of the data chunk OR the loader's current frontier.
        const uint32_t dataEnd      = _dataStart + _dataBytes;
        const uint32_t loadFrontier = (loaded < dataEnd) ? loaded : dataEnd;

        // ── EOF? (data exhausted; loop wrap or terminate) ───────────
        if (_framesRead >= _totalFrames) {
            if (!_loop) { _exhausted = true; break; }
            if (_loopCount != LOOP_INFINITE) {
                if (_loopCount <= 0) {
                    _loop = false;
                    _exhausted = true;
                    break;
                }
                _loopCount--;
            }
            _readBytes  = _dataStart;
            _framesRead = 0;
            continue;
        }

        // ── Loader hasn't caught up yet? ────────────────────────────
        if (_readBytes >= loadFrontier) {
            // No bytes available right now; return what we have.
            // Mixer covers the gap with silence; we'll resume next call.
            break;
        }

        const uint32_t bytesAhead = loadFrontier - _readBytes;
        uint32_t framesAvail      = bytesAhead / _bytesPerFrame;
        const uint32_t fileLeft   = _totalFrames - _framesRead;
        if (framesAvail > fileLeft) framesAvail = fileLeft;
        const uint32_t want       = maxFrames - delivered;
        const uint32_t framesThisRound = (want < framesAvail) ? want : framesAvail;
        if (framesThisRound == 0) break;   // no whole frame available

        decodePcmBatch(_asset.data() + _readBytes,
                       (int)framesThisRound,
                       _numChannels, _bitsPerSample,
                       outL, outR, (int)delivered);

        _readBytes  += framesThisRound * _bytesPerFrame;
        _framesRead += framesThisRound;
        delivered   += framesThisRound;
    }

    return delivered;
}

bool WavPsramSource::seekFrame(uint32_t frame) {
    if (!_open || frame > _totalFrames) return false;
    _readBytes  = _dataStart + frame * _bytesPerFrame;
    _framesRead = frame;
    _exhausted  = false;
    return true;
}

int WavPsramSource::bufferFillPercent() const {
    if (!_open) return 0;
    const uint32_t tot = _asset.totalBytes();
    return tot ? (int)(_asset.loadedBytes() * 100u / tot) : 0;
}

uint32_t WavPsramSource::bufferFrames() const {
    if (!_open) return 0;
    const uint32_t loaded = _asset.loadedBytes();
    const uint32_t dataEnd      = _dataStart + _dataBytes;
    const uint32_t loadFrontier = (loaded < dataEnd) ? loaded : dataEnd;
    if (_readBytes >= loadFrontier) return 0;
    return (loadFrontier - _readBytes) / (_bytesPerFrame ? _bytesPerFrame : 1);
}

// Size + alignment assertion for placement-new in mixer channel storage.
static_assert(sizeof(WavPsramSource)  <= kAudioSourceMaxSize,
              "Grow kAudioSourceMaxSize (WavPsramSource)");
static_assert(alignof(WavPsramSource) <= kAudioSourceAlign, "alignment");

#endif  // SFX_PLATFORM_ESP32 && SFX_HAS_AUDIO
