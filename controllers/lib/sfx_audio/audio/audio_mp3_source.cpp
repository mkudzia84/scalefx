/*
 * audio_mp3_source.cpp — MP3-from-PSRAM impl.
 */

#include "audio_mp3_source.h"
#include <platform/sfx_platform.h>   // SFX_MILLIS()

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)

#include <cstring>
#include <serial/diag_log.h>

#include "mp3dec.h"   // chmorgan/esp-libhelix-mp3 IDF component (pub/ is its include dir)

#define MP3_LOG(fmt, ...)   SFX_LOG_INFO ("[Mp3Src] " fmt, ##__VA_ARGS__)
#define MP3_WARN(fmt, ...)  SFX_LOG_WARN ("[Mp3Src] " fmt, ##__VA_ARGS__)
#define MP3_ERROR(fmt, ...) SFX_LOG_ERROR("[Mp3Src] " fmt, ##__VA_ARGS__)


// ── ID3v2 skip helper ──────────────────────────────────────────────
//
// Common encoders (ffmpeg, LAME) prepend an ID3v2 tag.  libhelix
// doesn't parse it and MP3FindSyncWord will skip past leading bytes
// looking for 0xFF, but a long ID3v2 frame can be 100+ KB which is a
// lot of "wasted" bitstream scanning.  Compute the skip length up
// front and seek past it.
namespace {

uint32_t id3v2HeaderSkipBytes(const uint8_t* data, uint32_t bytes) {
    if (bytes < 10) return 0;
    if (data[0] != 'I' || data[1] != 'D' || data[2] != '3') return 0;
    // size is 4 bytes, each holds 7 bits (synchsafe).
    const uint32_t sz = ((uint32_t)data[6] << 21) |
                        ((uint32_t)data[7] << 14) |
                        ((uint32_t)data[8] <<  7) |
                        ((uint32_t)data[9]      );
    return 10u + sz;
}

}   // anon


// ── open() ─────────────────────────────────────────────────────────

bool Mp3PsramSource::open(const char* path) {
    _asset = AudioAssetCache::instance().acquire(path);
    if (!_asset.isValid()) {
        MP3_ERROR("acquire failed: %s", path);
        return false;
    }
    if (_asset.format() != AssetFormat::Mp3) {
        MP3_ERROR("not a .mp3 file: %s", path);
        _asset.reset();
        return false;
    }

    // Block briefly on enough head bytes (~32 KB) to skip any leading
    // ID3v2 tag and decode the first frame for format extraction.
    constexpr uint32_t kHeaderHeadBytes = 32 * 1024;
    const uint32_t waitStart = SFX_MILLIS();
    while (!_asset.hasHead(kHeaderHeadBytes) && !_asset.isFailed()) {
        if (SFX_MILLIS() - waitStart > 2000) {
            MP3_ERROR("head-bytes wait timeout: %s (loaded=%u)",
                      path, (unsigned)_asset.loadedBytes());
            _asset.reset();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (_asset.isFailed()) {
        MP3_ERROR("asset load failed: %s", path);
        _asset.reset();
        return false;
    }

    // Lease a decoder + PCM scratch from the pool.
    _lease = Mp3DecoderPool::instance().acquire();
    if (!_lease.isValid()) {
        MP3_ERROR("decoder pool exhausted: %s", path);
        _asset.reset();
        return false;
    }
    _pcmBuf = _lease.pcmBuf();

    // Skip ID3v2 tag if present, then find the first sync word.
    _bitPos = id3v2HeaderSkipBytes(_asset.data(), _asset.loadedBytes());
    if (!findNextSync()) {
        MP3_ERROR("no MP3 sync word found in %s", path);
        _lease.reset();
        _asset.reset();
        return false;
    }
    _bitPosLoopBase = _bitPos;

    // Decode the first frame to latch format.
    if (!decodeNextFrame()) {
        MP3_ERROR("first frame decode failed: %s", path);
        _lease.reset();
        _asset.reset();
        return false;
    }

    MP3FrameInfo info;
    MP3GetLastFrameInfo(static_cast<HMP3Decoder>(_lease.decoder()), &info);
    _sampleRate_Hz = (uint32_t)info.samprate;
    _numChannels   = (uint16_t)info.nChans;
    if (_sampleRate_Hz == 0 || _numChannels == 0 || _numChannels > 2) {
        MP3_ERROR("invalid first frame: %s (rate=%u ch=%u)",
                  path, (unsigned)_sampleRate_Hz, (unsigned)_numChannels);
        _lease.reset();
        _asset.reset();
        return false;
    }

    // _totalFrames estimate — without VBRI/Xing parsing we don't
    // know exactly.  Use byte-rate × duration approximation:
    //   approxFrames = totalBytes × 8 / bitrate × sampleRate
    // bitrate from MP3FrameInfo is bps; this is rough for VBR.
    if (info.bitrate > 0) {
        const uint64_t durationMs = (uint64_t)(_asset.totalBytes() - _bitPosLoopBase) * 8000ULL
                                    / (uint64_t)info.bitrate;
        _totalFrames = (uint32_t)((durationMs * _sampleRate_Hz) / 1000ULL);
    } else {
        _totalFrames = UINT32_MAX;   // unknown; loop check uses bytes-left instead
    }

    _framesRead    = 0;
    _exhausted     = false;
    _loopCount     = _loopCountInit;
    _open          = true;

    MP3_LOG("opened %s — %uHz/%uch/16b, bitrate=%u, est %u frames, %u KB (loaded %u%%)",
            path,
            (unsigned)_sampleRate_Hz, (unsigned)_numChannels,
            (unsigned)info.bitrate, (unsigned)_totalFrames,
            (unsigned)(_asset.totalBytes() / 1024),
            (unsigned)(_asset.loadedBytes() * 100u /
                       (_asset.totalBytes() ? _asset.totalBytes() : 1)));
    return true;
}

Mp3PsramSource::~Mp3PsramSource() {
    _lease.reset();
    _asset.reset();
    _open = false;
}


// ── Sync / decode helpers ──────────────────────────────────────────

bool Mp3PsramSource::findNextSync() {
    const uint32_t loaded = _asset.loadedBytes();
    if (_bitPos >= loaded) return false;
    const int avail = (int)(loaded - _bitPos);
    const int off = MP3FindSyncWord(
        const_cast<unsigned char*>(_asset.data() + _bitPos), avail);
    if (off < 0) return false;
    _bitPos += (uint32_t)off;
    return true;
}

bool Mp3PsramSource::decodeNextFrame() {
    // Bounded retry on invalid headers — a naive seek (or stream noise) can
    // land on a FALSE sync word (compressed audio is full of 0xFF bytes), and
    // recursing per false sync overflowed the producer stack and crashed the
    // firmware.  Skip-and-resync in a LOOP, capped, instead.
    for (int attempts = 0; attempts < 128; ++attempts) {
        const uint32_t loaded = _asset.loadedBytes();
        if (_bitPos >= loaded) {
            // Loader behind us — wait for next call.
            _pcmLen = 0;
            _pcmPos = 0;
            return false;
        }
        int avail = (int)(loaded - _bitPos);
        unsigned char* ptr = const_cast<unsigned char*>(_asset.data() + _bitPos);
        const int res = MP3Decode(static_cast<HMP3Decoder>(_lease.decoder()),
                                  &ptr, &avail, _pcmBuf, 0);
        if (res == ERR_MP3_NONE) {
            // ptr is advanced by helix; recompute _bitPos from the new ptr.
            _bitPos = (uint32_t)(ptr - _asset.data());
            MP3FrameInfo info;
            MP3GetLastFrameInfo(static_cast<HMP3Decoder>(_lease.decoder()), &info);
            // outputSamps counts INTERLEAVED samples (i.e. nChans × frames).
            _pcmLen = info.outputSamps / (info.nChans ? info.nChans : 2);
            _pcmPos = 0;
            return _pcmLen > 0;
        }
        if (res == ERR_MP3_INDATA_UNDERFLOW || res == ERR_MP3_MAINDATA_UNDERFLOW) {
            // Need more bytes; not fatal.  Caller retries.
            _pcmLen = 0;
            _pcmPos = 0;
            return false;
        }
        if (res == ERR_MP3_INVALID_FRAMEHEADER) {
            // Garbage / false sync — skip one byte and resync (bounded loop).
            _bitPos++;
            if (!findNextSync()) { _pcmLen = 0; _pcmPos = 0; return false; }
            continue;
        }
        MP3_WARN("MP3Decode err=%d @ byte %u", res, (unsigned)_bitPos);
        _pcmLen = 0;
        _pcmPos = 0;
        return false;
    }
    MP3_WARN("decodeNextFrame: too many invalid headers @ byte %u", (unsigned)_bitPos);
    _pcmLen = 0;
    _pcmPos = 0;
    return false;
}

void Mp3PsramSource::rewindForLoop() {
    // Reset decoder via fresh lease (cheap — pool reuses the slot).
    _lease.reset();
    _lease = Mp3DecoderPool::instance().acquire();
    _pcmBuf = _lease.isValid() ? _lease.pcmBuf() : nullptr;
    _bitPos = _bitPosLoopBase;
    _pcmLen = 0;
    _pcmPos = 0;
    _framesRead = 0;
}


// ── readFrames ─────────────────────────────────────────────────────

uint32_t Mp3PsramSource::readFrames(float* outL, float* outR,
                                    uint32_t maxFrames) {
    if (!_open || _exhausted) return 0;
    if (_asset.isFailed() || !_lease.isValid()) {
        _exhausted = true;
        return 0;
    }

    constexpr float kInv = 1.0f / 32768.0f;
    uint32_t delivered = 0;
    while (delivered < maxFrames) {
        // ── Drain PCM scratch if non-empty ──────────────────────────
        if (_pcmPos < _pcmLen) {
            const uint32_t want      = maxFrames - delivered;
            const uint32_t scratchOk = (uint32_t)(_pcmLen - _pcmPos);
            const uint32_t take      = (want < scratchOk) ? want : scratchOk;
            // helix outputs interleaved L/R (or mono).  numChannels was
            // latched at open() — assume it's stable for the file.
            if (_numChannels == 2) {
                const int16_t* src = _pcmBuf + _pcmPos * 2;
                for (uint32_t i = 0; i < take; ++i) {
                    outL[delivered + i] = src[i * 2]     * kInv;
                    outR[delivered + i] = src[i * 2 + 1] * kInv;
                }
            } else {
                const int16_t* src = _pcmBuf + _pcmPos;
                for (uint32_t i = 0; i < take; ++i) {
                    const float v = src[i] * kInv;
                    outL[delivered + i] = v;
                    outR[delivered + i] = v;
                }
            }
            _pcmPos     += (int)take;
            _framesRead += take;
            delivered   += take;
            continue;
        }

        // ── PCM scratch drained — decode next frame ─────────────────
        const uint32_t loaded = _asset.loadedBytes();
        const bool fullyLoaded = (loaded == _asset.totalBytes());

        // End-of-stream check: we're past the last sync AND the file
        // is fully loaded.  Found by attempting findNextSync().
        if (_bitPos >= loaded) {
            if (!fullyLoaded) break;     // loader behind — return short

            // EOF — loop or exhaust
            if (!_loop) { _exhausted = true; break; }
            if (_loopCount != LOOP_INFINITE) {
                if (_loopCount <= 0) {
                    _loop = false;
                    _exhausted = true;
                    break;
                }
                _loopCount--;
            }
            rewindForLoop();
            if (!_lease.isValid()) { _exhausted = true; break; }
            continue;
        }

        // Try to find next sync (handles small mid-stream noise).
        if (!findNextSync()) {
            if (!fullyLoaded) break;
            // No more syncs in the file — same path as EOF above.
            if (!_loop) { _exhausted = true; break; }
            if (_loopCount != LOOP_INFINITE) {
                if (_loopCount <= 0) { _loop = false; _exhausted = true; break; }
                _loopCount--;
            }
            rewindForLoop();
            if (!_lease.isValid()) { _exhausted = true; break; }
            continue;
        }

        if (!decodeNextFrame()) {
            // Could be underflow (need more bytes) — return short.
            break;
        }
    }

    return delivered;
}

bool Mp3PsramSource::seekFrame(uint32_t frame) {
    // Hybrid seek: O(1) proportional byte jump to ~kPrimeFrames BEFORE the
    // target (exact for CBR engine sounds, coarse for VBR), then a SHORT
    // decode-walk to prime the bit reservoir and land accurately.  A full
    // decode-walk from the start is correct but too slow (decoding 20 s of MP3
    // ≈ 1.25 s of CPU); the proportional jump caps the decode to ~kPrimeFrames
    // (~30 ms) regardless of distance.  decodeNextFrame()'s bounded resync loop
    // makes a false-sync landing safe (the earlier crash was its recursion).
    if (!_open) return false;
    if (frame == 0) { rewindForLoop(); return _lease.isValid(); }
    if (_totalFrames == 0 || _totalFrames == UINT32_MAX || frame >= _totalFrames)
        return false;

    const uint32_t base   = _bitPosLoopBase;
    const uint32_t total  = _asset.totalBytes();
    const uint32_t loaded = _asset.loadedBytes();
    if (total <= base) return false;

    // Decode ~16 MP3 frames (~0.37 s @48k) before the target to rebuild the
    // reservoir so the target decodes cleanly.
    const uint32_t kPrimeFrames = 1152u * 16u;
    uint32_t coarse = (frame > kPrimeFrames) ? (frame - kPrimeFrames) : 0;

    // Fresh decoder context.
    _lease.reset();
    _lease  = Mp3DecoderPool::instance().acquire();
    _pcmBuf = _lease.isValid() ? _lease.pcmBuf() : nullptr;
    if (!_lease.isValid()) return false;
    _exhausted = false;
    _pcmLen = 0;
    _pcmPos = 0;

    if (coarse == 0) {
        _bitPos = base;                    // prime from the very start
    } else {
        uint32_t est = base + (uint32_t)(((uint64_t)coarse * (total - base)) / _totalFrames);
        if (est >= loaded) { _bitPos = base; coarse = 0; }   // not resident — walk from start
        else {
            _bitPos = est;
            if (!findNextSync()) { _bitPos = base; coarse = 0; }
        }
    }
    _framesRead = coarse;

    // Short decode-walk up to the target.  On a cold/underflowing frame,
    // decodeNextFrame() returns false WITHOUT advancing — skip a byte + resync
    // so we make progress (guard-bounded; never stalls or crashes).
    const uint32_t guardMax = (frame - coarse) / 256 + 96;
    uint32_t guard = 0;
    while (_framesRead < frame && guard++ < guardMax) {
        if (_bitPos >= loaded) break;      // EOF / loader behind
        if (!findNextSync())   break;
        if (!decodeNextFrame()) {          // underflow/bad cold frame — skip ahead
            _bitPos++;
            continue;
        }
        const uint32_t got = (uint32_t)_pcmLen;
        if (_framesRead + got <= frame) {
            _framesRead += got;
            _pcmPos = _pcmLen;             // discard the whole primed frame
        } else {
            _pcmPos = (int)(frame - _framesRead);  // partial — keep remainder
            _framesRead = frame;
        }
    }
    MP3_LOG("seek -> frame %u (byte %u, coarse %u)",
            (unsigned)_framesRead, (unsigned)_bitPos, (unsigned)coarse);
    return true;
}


// ── Diag ───────────────────────────────────────────────────────────

int Mp3PsramSource::bufferFillPercent() const {
    if (!_open) return 0;
    const uint32_t tot = _asset.totalBytes();
    return tot ? (int)(_asset.loadedBytes() * 100u / tot) : 0;
}

uint32_t Mp3PsramSource::bufferFrames() const {
    if (!_open) return 0;
    // Approximation: stereo frames worth of scratch left + a rough
    // estimate from remaining bytes × samples/byte.  Used only for
    // status display; doesn't need to be exact.
    const uint32_t scratchLeft = (uint32_t)((_pcmLen > _pcmPos) ? (_pcmLen - _pcmPos) : 0);
    return scratchLeft;
}

// Size + alignment assertion for placement-new in mixer channel storage.
static_assert(sizeof(Mp3PsramSource)  <= kAudioSourceMaxSize,
              "Grow kAudioSourceMaxSize (Mp3PsramSource)");
static_assert(alignof(Mp3PsramSource) <= kAudioSourceAlign, "alignment");

#endif  // SFX_PLATFORM_ESP32 && SFX_HAS_AUDIO
