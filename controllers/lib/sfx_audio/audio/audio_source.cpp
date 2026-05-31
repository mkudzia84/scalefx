/*
 * audio_source.cpp — implementations.
 *
 * Both sources share the same WAV header parser + PCM→float decode
 * kernel as free helpers (declared in audio_source.h).  The streaming
 * source mirrors the legacy refillWavBuffer behaviour exactly so
 * Stage 2 is a drop-in refactor — same SD lock cadence, same loop-
 * wrap-prefetch, same per-channel float buffer.
 */

#include "audio_source.h"

#if defined(SFX_HAS_AUDIO) && SFX_PLATFORM_ESP32

#include <cstring>
#include "audio_log.h"
#include "audio_config.h"

// ── Shared helpers ───────────────────────────────────────────────────

bool parseWavHeaderLocked(SdFile&    file,
                          uint32_t&  outSampleRate_Hz,
                          uint16_t&  outNumChannels,
                          uint16_t&  outBitsPerSample,
                          uint32_t&  outDataStart,
                          uint32_t&  outTotalFrames) {
    uint8_t header[44];
    if (file.read(header, 44) != 44) return false;

    if (memcmp(header, "RIFF", 4) != 0)        return false;
    if (memcmp(header + 8,  "WAVE", 4) != 0)   return false;
    if (memcmp(header + 12, "fmt ", 4) != 0)   return false;

    const uint16_t audioFormat = header[20] | (header[21] << 8);
    if (audioFormat != 1) return false;     // PCM only

    outNumChannels   = header[22] | (header[23] << 8);
    outSampleRate_Hz = header[24] | (header[25] << 8) |
                       (header[26] << 16) | (header[27] << 24);
    outBitsPerSample = header[34] | (header[35] << 8);

    if (outNumChannels < 1 || outNumChannels > 2)              return false;
    if (outBitsPerSample != 8 && outBitsPerSample != 16)       return false;

    file.seek(12);
    while (file.available()) {
        uint8_t chunkHeader[8];
        if (file.read(chunkHeader, 8) != 8) return false;

        const uint32_t chunkSize = chunkHeader[4] | (chunkHeader[5] << 8) |
                                   (chunkHeader[6] << 16) | (chunkHeader[7] << 24);

        if (memcmp(chunkHeader, "data", 4) == 0) {
            outDataStart   = file.position();
            const uint32_t bytesPerFrame =
                outNumChannels * (outBitsPerSample / 8);
            outTotalFrames = chunkSize / bytesPerFrame;
            return true;
        }
        file.seek(file.position() + chunkSize);
    }
    return false;
}

void decodePcmBatch(const uint8_t* src, int frames,
                    uint16_t numChannels, uint16_t bitsPerSample,
                    float* dstL, float* dstR, int dstFrameOffset) {
    if (bitsPerSample == 16) {
        const int16_t* buf16 = reinterpret_cast<const int16_t*>(src);
        constexpr float kInv = 1.0f / 32768.0f;
        if (numChannels == 2) {
            for (int i = 0; i < frames; ++i) {
                dstL[dstFrameOffset + i] = buf16[i * 2]     * kInv;
                dstR[dstFrameOffset + i] = buf16[i * 2 + 1] * kInv;
            }
        } else {
            for (int i = 0; i < frames; ++i) {
                const float v = buf16[i] * kInv;
                dstL[dstFrameOffset + i] = v;
                dstR[dstFrameOffset + i] = v;
            }
        }
    } else {  // 8-bit unsigned (offset 128)
        constexpr float kInv = 1.0f / 128.0f;
        if (numChannels == 2) {
            for (int i = 0; i < frames; ++i) {
                dstL[dstFrameOffset + i] = (src[i * 2]     - 128) * kInv;
                dstR[dstFrameOffset + i] = (src[i * 2 + 1] - 128) * kInv;
            }
        } else {
            for (int i = 0; i < frames; ++i) {
                const float v = (src[i] - 128) * kInv;
                dstL[dstFrameOffset + i] = v;
                dstR[dstFrameOffset + i] = v;
            }
        }
    }
}


// ── WavStreamSource ──────────────────────────────────────────────────

WavStreamSource::~WavStreamSource() { closeFile(); }

void WavStreamSource::closeFile() {
    if (_file) {
        SdCardModule& sd = SdCardModule::instance();
        sd.lock();
        _file.close();
        sd.unlock();
    }
    _open = false;
}

bool WavStreamSource::open(const char* path, const StreamScratch& scratch) {
    if (!scratch.sdReadBuf || scratch.sdReadBufSize <= 0) {
        MIXER_ERROR("Stream: open(%s) with invalid scratch", path);
        return false;
    }
    _scratch = scratch;

    SdCardModule& sd = SdCardModule::instance();
    if (!sd.isInitialized()) {
        MIXER_ERROR("Stream: SD not initialized (%s)", path);
        return false;
    }

    sd.lock();
    uint8_t err = sd.openRead(path, _file);
    if (err != 0) {
        sd.unlock();
        MIXER_ERROR("Stream: openRead failed: %s (err=%u: %s)", path, (unsigned)err,
                    err == 1 ? "NOT_INITIALIZED" :
                    err == 2 ? "NOT_FOUND" :
                    err == 3 ? "IO_ERROR" :
                    err == 4 ? "IS_DIRECTORY" : "?");
        return false;
    }
    if (!parseWavHeaderLocked(_file, _sampleRate_Hz, _numChannels,
                              _bitsPerSample, _dataStart, _totalFrames)) {
        _file.close();
        sd.unlock();
        MIXER_ERROR("Stream: invalid WAV: %s", path);
        return false;
    }
    _openEpoch = sd.mountEpoch();   // captured under the lock — pairs with reads
    sd.unlock();

    _framesRead = 0;
    _exhausted  = false;
    _open       = true;
    return true;
}

uint32_t WavStreamSource::readFrames(float* outL, float* outR,
                                     uint32_t maxFrames) {
    if (!_open || _exhausted) return 0;

    // SD remount guard (Rule 56): if SD_INIT remounted the card since we opened,
    // `_file` points at a torn-down VFS volume — abort as EOF rather than read a
    // recycled handle (use-after-unmount). Cheap atomic compare per call.
    if (SdCardModule::instance().mountEpoch() != _openEpoch) {
        _exhausted = true;
        MIXER_WARN("Stream: SD remounted under open file — aborting playback");
        return 0;
    }

    const int bytesPerFrame = _numChannels * (_bitsPerSample / 8);
    uint32_t  delivered     = 0;

    while (delivered < maxFrames) {
        // ── Loop wrap / EOS check ────────────────────────────────────
        int framesLeft = (int)(_totalFrames - _framesRead);
        if (framesLeft <= 0) {
            if (!_loop) {
                _exhausted = true;
                break;
            }
            if (_loopCount != LOOP_INFINITE) {
                if (_loopCount <= 0) {
                    _loop      = false;
                    _exhausted = true;
                    break;
                }
                _loopCount--;
            }
            SdCardModule::instance().lock();
            _file.seek(_dataStart);
            SdCardModule::instance().unlock();
            _framesRead = 0;
            framesLeft  = (int)_totalFrames;
            if (framesLeft <= 0) {
                _exhausted = true;
                break;
            }
        }

        // ── Batch size: min(want, framesLeft, sdReadBuf capacity) ────
        int want         = (int)(maxFrames - delivered);
        int batchFrames  = (want < framesLeft) ? want : framesLeft;
        int bytesToRead  = batchFrames * bytesPerFrame;
        if (bytesToRead > _scratch.sdReadBufSize) {
            bytesToRead  = _scratch.sdReadBufSize;
            batchFrames  = bytesToRead / bytesPerFrame;
        }

        // ── SD read + decode straight into outL/outR ─────────────────
        SdCardModule::instance().lock();
        int bytesRead = _file.read(_scratch.sdReadBuf, bytesToRead);
        SdCardModule::instance().unlock();

        int framesGot = bytesRead / bytesPerFrame;
        if (framesGot <= 0) break;

        decodePcmBatch(_scratch.sdReadBuf, framesGot,
                       _numChannels, _bitsPerSample,
                       outL, outR, (int)delivered);

        _framesRead += framesGot;
        delivered   += framesGot;

        // Short read → SD slow-path; defer remaining work to next call
        // so we don't starve other channels.
        if (bytesRead < bytesToRead) break;
    }

    return delivered;
}

bool WavStreamSource::seekFrame(uint32_t frame) {
    if (!_open || frame >= _totalFrames) return false;
    const uint32_t bytesPerFrame = _numChannels * (_bitsPerSample / 8);
    SdCardModule& sd = SdCardModule::instance();
    sd.lock();
    _file.seek(_dataStart + frame * bytesPerFrame);
    sd.unlock();
    _framesRead = frame;
    return true;
}


// ── WavPreloadSource ─────────────────────────────────────────────────

WavPreloadSource::~WavPreloadSource() { freeBuffers(); }

void WavPreloadSource::freeBuffers() {
    if (_bufL) { sfxPsramFree(_bufL); _bufL = nullptr; }
    if (_bufR) { sfxPsramFree(_bufR); _bufR = nullptr; }
}

bool WavPreloadSource::openAndPreload(const char* path,
                                      uint8_t* sdScratchBuf,
                                      uint32_t sdScratchSize,
                                      uint32_t maxPreloadFrames) {
    if (!sdScratchBuf || sdScratchSize == 0) {
        MIXER_ERROR("Preload: openAndPreload(%s) with invalid scratch", path);
        return false;
    }

    SdCardModule& sd = SdCardModule::instance();
    if (!sd.isInitialized()) {
        MIXER_ERROR("Preload: SD not initialized (%s)", path);
        return false;
    }

    SdFile file;
    sd.lock();
    if (sd.openRead(path, file) != 0) {
        sd.unlock();
        MIXER_ERROR("Preload: openRead failed: %s", path);
        return false;
    }
    uint32_t dataStart = 0;
    if (!parseWavHeaderLocked(file, _sampleRate_Hz, _numChannels,
                              _bitsPerSample, dataStart, _totalFrames)) {
        file.close();
        sd.unlock();
        MIXER_ERROR("Preload: invalid WAV: %s", path);
        return false;
    }
    sd.unlock();

    if (_totalFrames == 0) {
        sd.lock(); file.close(); sd.unlock();
        return false;
    }
    if (maxPreloadFrames > 0 && _totalFrames > maxPreloadFrames) {
        MIXER_WARN("Preload: %s has %u frames > cap %u — caller falls back to streaming",
                   path, (unsigned)_totalFrames, (unsigned)maxPreloadFrames);
        sd.lock(); file.close(); sd.unlock();
        return false;
    }

    constexpr size_t kPreloadSafetyMarginBytes = 1 * 1024 * 1024;
    const uint32_t   bytesPerBuf = _totalFrames * sizeof(float);
    const uint32_t   needed      = bytesPerBuf * 2;
    const size_t     psramFree   = sfxPsramFree_bytes();
    if (psramFree > 0 &&
        (size_t)needed + kPreloadSafetyMarginBytes > psramFree) {
        MIXER_WARN("Preload: would leave PSRAM under safety margin "
                   "(need=%u, free=%u, margin=%u) — streaming %s",
                   (unsigned)needed, (unsigned)psramFree,
                   (unsigned)kPreloadSafetyMarginBytes, path);
        sd.lock(); file.close(); sd.unlock();
        return false;
    }

    _bufL = static_cast<float*>(sfxPsramMalloc(bytesPerBuf));
    _bufR = static_cast<float*>(sfxPsramMalloc(bytesPerBuf));
    if (!_bufL || !_bufR) {
        MIXER_ERROR("Preload: PSRAM alloc failed (%u B each, free=%u) for %s",
                    (unsigned)bytesPerBuf, (unsigned)sfxPsramFree_bytes(), path);
        freeBuffers();
        sd.lock(); file.close(); sd.unlock();
        return false;
    }

    const int    bytesPerFrame = _numChannels * (_bitsPerSample / 8);
    uint32_t     framesDecoded = 0;
    while (framesDecoded < _totalFrames) {
        const uint32_t framesLeft  = _totalFrames - framesDecoded;
        const uint32_t maxBatch    = (uint32_t)(sdScratchSize / bytesPerFrame);
        const uint32_t batchFrames =
            (framesLeft > maxBatch) ? maxBatch : framesLeft;
        const int      batchBytes  = (int)(batchFrames * bytesPerFrame);

        sd.lock();
        int bytesRead = file.read(sdScratchBuf, batchBytes);
        sd.unlock();
        if (bytesRead != batchBytes) {
            MIXER_ERROR("Preload: short read on %s (%d/%d B at frame %u)",
                        path, bytesRead, batchBytes, (unsigned)framesDecoded);
            freeBuffers();
            sd.lock(); file.close(); sd.unlock();
            return false;
        }

        decodePcmBatch(sdScratchBuf, (int)batchFrames,
                       _numChannels, _bitsPerSample,
                       _bufL, _bufR, (int)framesDecoded);
        framesDecoded += batchFrames;
    }

    sd.lock(); file.close(); sd.unlock();

    _framesRead = 0;
    _exhausted  = false;

    MIXER_LOG("Preload: cached %s (%u frames, %u KB, %.1f s @ %u Hz)",
              path, (unsigned)_totalFrames,
              (unsigned)(bytesPerBuf * 2 / 1024),
              (double)_totalFrames / (double)_sampleRate_Hz,
              (unsigned)_sampleRate_Hz);
    return true;
}

uint32_t WavPreloadSource::readFrames(float* outL, float* outR,
                                      uint32_t maxFrames) {
    if (!_bufL || !_bufR || _exhausted || _totalFrames == 0) return 0;

    uint32_t delivered = 0;
    while (delivered < maxFrames) {
        const uint32_t framesLeft = _totalFrames - _framesRead;
        if (framesLeft == 0) {
            if (!_loop) {
                _exhausted = true;
                break;
            }
            if (_loopCount != LOOP_INFINITE) {
                if (_loopCount <= 0) {
                    _loop      = false;
                    _exhausted = true;
                    break;
                }
                _loopCount--;
            }
            _framesRead = 0;
            continue;
        }

        const uint32_t want = maxFrames - delivered;
        const uint32_t take = (framesLeft < want) ? framesLeft : want;

        memcpy(outL + delivered, _bufL + _framesRead, take * sizeof(float));
        memcpy(outR + delivered, _bufR + _framesRead, take * sizeof(float));
        _framesRead += take;
        delivered   += take;
    }
    return delivered;
}

bool WavPreloadSource::seekFrame(uint32_t frame) {
    if (frame >= _totalFrames) return false;
    _framesRead = frame;
    _exhausted  = false;
    return true;
}

#endif  // SFX_HAS_AUDIO && SFX_PLATFORM_ESP32
