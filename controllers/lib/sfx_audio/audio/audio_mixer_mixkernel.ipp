/*
 * AudioMixer<TI2S,TCodec>::MixKernel — the mixing kernel.
 *
 * One class, one file.  Produces mixed frames from a mixer's channels into the
 * output ring: produceFrame (single-frame fallback), produceBlock (int16/Q15 +
 * esp-dsp SIMD), produceBlockFloat (float32 + esp-dsp SIMD), and produceLoop
 * (the dispatch — int16-vs-float chosen at compile time via `if constexpr`).
 * Each takes the AudioMixer by reference for the channel array + the EOF
 * orchestration hooks (status + checkAndPlayNextQueued).  AudioMixer::produce()
 * (the entry that calls produceLoop) lives in audio_mixer.ipp.
 *
 * #included by audio_mixer.h after audio_mixer.ipp (shares its preamble, one TU).
 */

#if defined(SFX_HAS_AUDIO)

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::MixKernel::produceFrame(AudioMixer& m) {
    // Alias the mixer fields this kernel reads/writes so the hot-path body
    // below stays byte-identical to the pre-extraction code.
    auto& _channels            = m._channels;
    auto& _channelPlaying      = m._channelPlaying;
    auto& _channelRemainingSec = m._channelRemainingSec;
    auto& _masterVolume        = m._masterVolume;
    if (ringBuf().isFull()) return false;

    int32_t mixL = 0;
    int32_t mixR = 0;
    bool hasAudio = false;

    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        Channel& ch = _channels[i];
        WavState& ws = ch.wav;

        if (!ws.active || ch.mute || ch.volume <= 0.0f) continue;

        int16_t trackL16 = 0;
        int16_t trackR16 = 0;

        // Get one resampled int16 stereo frame from the drain buffer.
        if (!ch.wav.readSample(trackL16, trackR16)) {
            ws.destroySafe();   // waits out the decoder, then frees
            ws.active.store(false, std::memory_order_release);
            _channelPlaying[i] = false;
            _channelRemainingSec[i] = 0.0f;
            m.checkAndPlayNextQueued(i);
            continue;
        }

        // Arm the auto tail fade-out once playback-remaining drops below
        // threshold (same logic as the float kernel — float scalar work
        // ≤ 1 op per channel per frame, not worth fixed-point-ifying).
        if (ch.fadeOutTriggerFrames > 0 && !ch.fading) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            // Clamp the subtraction — MP3 framesRead() can exceed the
            // estimated totalFrames, and an unsigned underflow here would
            // make framesLeft huge and the fade-out never arm.
            const uint32_t consumed = (srcRead < ws.totalFrames) ? srcRead : ws.totalFrames;
            const uint32_t framesLeft =
                ws.totalFrames - consumed + ws.availableRead();
            if (framesLeft <= ch.fadeOutTriggerFrames) {
                ch.fading               = true;
                ch.fadeVolume           = 1.0f;
                ch.fadeStep             = ch.fadeOutStep;
                ch.fadeOutTriggerFrames = 0;
            }
        }

        // Fade ramp update — float arithmetic, one mul/add/cmp per
        // channel per frame.  Cheaper than maintaining a Q15 ramp +
        // converting back.  Combined effectiveVolume converted to Q15
        // ONCE for the per-sample multiply below.
        float effectiveVolume = ch.volume;
        if (ch.fading) {
            effectiveVolume *= ch.fadeVolume;
            ch.fadeVolume -= ch.fadeStep;
            if (ch.fadeStep > 0.0f && ch.fadeVolume <= 0.0f) {
                ch.fadeVolume = 0.0f;
                ch.fading = false;
                ws.destroySafe();   // waits out the decoder, then frees
                ws.active.store(false, std::memory_order_release);
                _channelPlaying[i] = false;
                m.checkAndPlayNextQueued(i);
                continue;
            } else if (ch.fadeStep < 0.0f && ch.fadeVolume >= 1.0f) {
                ch.fadeVolume = 1.0f;
                ch.fading = false;
            }
        }

        hasAudio = true;

        // ── Q15 per-channel kernel ───────────────────────────────────
        // sample(int16) × volQ15(int32 in [0, 32768]) >> 15 keeps the
        // value within int16 range when vol ≤ 1.  Pan multiplies fold
        // into the same shape (panL/R also Q15 ≤ 32768).  When pan +
        // routing collapses to "ALL with vol only" the cost is one
        // multiply per channel; with pan it's two.
        const int32_t volQ15 = (int32_t)(effectiveVolume * 32768.0f);
        int32_t L = ((int32_t)trackL16 * volQ15) >> 15;
        int32_t R = ((int32_t)trackR16 * volQ15) >> 15;

        if (ch.outputChannels == AudioChannel::ALL) {
            const int32_t panLQ15 = (int32_t)(ch.panL * 32768.0f);
            const int32_t panRQ15 = (int32_t)(ch.panR * 32768.0f);
            L = (L * panLQ15) >> 15;
            R = (R * panRQ15) >> 15;
        } else {
            if (!(ch.outputChannels & AudioChannel::CH1)) L = 0;
            if (!(ch.outputChannels & AudioChannel::CH2)) R = 0;
        }

        mixL += L;
        mixR += R;

        // Update remaining-time status — once per channel per frame, off
        // the hot multiply path.
        if (ws.sampleRate_Hz > 0) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            const uint32_t consumed = (srcRead < ws.totalFrames) ? srcRead : ws.totalFrames;
            const uint32_t framesLeft =
                ws.totalFrames - consumed + ws.availableRead();
            _channelRemainingSec[i] = (float)framesLeft / (float)ws.sampleRate_Hz;
        }
    }

    // Skip silence — keeps the ring near-empty while idle so a fresh
    // play() reaches output within milliseconds.
    if (!hasAudio) return false;

    // Master volume in Q15.  master=1.0 → mvolQ15=32768 → mix unchanged.
    const int32_t mvolQ15 = (int32_t)(_masterVolume * 32768.0f);
    mixL = (mixL * mvolQ15) >> 15;
    mixR = (mixR * mvolQ15) >> 15;

    // Clamp to full int16 range — matches the legacy float code's
    // saturation at ±1.0 before the output-gain step below.
    if (mixL >  32767) mixL =  32767;
    if (mixL < -32768) mixL = -32768;
    if (mixR >  32767) mixR =  32767;
    if (mixR < -32768) mixR = -32768;

    // Output headroom scaling — preserves the legacy `* MAX_AMPLITUDE`
    // step (24000 / 32768 ≈ 0.732, −2.7 dB) so transients don't slam
    // the DAC.  Final int16 written to the SPSC ring.
    mixL = (mixL * MAX_AMPLITUDE) >> 15;
    mixR = (mixR * MAX_AMPLITUDE) >> 15;

    ringBuf().write((int16_t)mixL, (int16_t)mixR);
    return true;
}

// ============================================================================
//  BLOCK-MODE PRODUCER (Phase 5, feature/idf-component-build, 2026-05-28)
// ============================================================================
//
// produceBlock(N) — N frames per call.  The per-channel scale step
// (sample × volQ15 → scaled int16) runs through esp-dsp's
// `dsps_mulc_s16_ae32` (hand-tuned Xtensa LX7 assembly, ~3-5× faster
// than scalar at typical block sizes).  Accumulation goes into a
// stack-resident int32 mix buffer; the compiler auto-vectorises the
// int16→int32 widen-add loop on LX7.
//
// Block size cap = 256 frames (5.3 ms @ 48 kHz):
//   - Long enough to amortise dsps_mulc_s16_ae32's setup cost.
//   - Short enough that per-block fade granularity stays imperceptible
//     (10 steps in a 50 ms fade; threshold of audibility ≈ 50 steps).
//   - Stack budget: 256 × 2 (L/R) × 4 (int32 mix) + 256 × 2 × 2 (int16
//     track) = 3 KB, well under the 8 KB producer task stack.
//
// Falls back to a scalar Q15 path when esp-dsp isn't in the build
// (SFX_HAS_ESP_DSP = 0) — same arithmetic, just no SIMD speedup.

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::MixKernel::produceBlock(AudioMixer& m, int maxFrames) {
    auto& _channels            = m._channels;
    auto& _channelPlaying      = m._channelPlaying;
    auto& _channelRemainingSec = m._channelRemainingSec;
    auto& _masterVolume        = m._masterVolume;
    constexpr int kBlockMax = 256;

    const uint32_t freeSlots = ringBuf().availableWrite();
    int N = maxFrames;
    if (N > (int)freeSlots) N = (int)freeSlots;
    if (N > kBlockMax)      N = kBlockMax;
    if (N <= 0) return 0;

    // Stack scratch.  Zeroing mixL/mixR once costs ~2 KB of memsets per
    // block — negligible vs the per-sample multiply work it saves.
    int32_t mixL[kBlockMax] = {};
    int32_t mixR[kBlockMax] = {};
    int16_t trackL[kBlockMax];
    int16_t trackR[kBlockMax];
    bool hasAudio = false;

    for (int i = 0; i < AUDIO_MAX_CHANNELS; ++i) {
        Channel& ch = _channels[i];
        WavState& ws = ch.wav;
        if (!ws.active || ch.mute || ch.volume <= 0.0f) continue;

        // Pull up to N resampled int16 samples from this channel's
        // drain.  EOF mid-block terminates the channel but we still
        // mix what we got into the block (no audible click).
        int got = 0;
        bool sourceDied = false;
        for (int j = 0; j < N; ++j) {
            int16_t l = 0, r = 0;
            if (!ch.wav.readSample(l, r)) {
                sourceDied = true;
                break;
            }
            trackL[j] = l;
            trackR[j] = r;
            got = j + 1;
        }
        if (sourceDied && got == 0) {
            ws.destroySafe();   // waits out the decoder, then frees
            ws.active.store(false, std::memory_order_release);
            _channelPlaying[i] = false;
            _channelRemainingSec[i] = 0.0f;
            m.checkAndPlayNextQueued(i);
            continue;
        }
        if (got == 0) continue;
        hasAudio = true;

        // Arm tail fade-out — same logic as produceFrame, just at
        // block cadence.
        if (ch.fadeOutTriggerFrames > 0 && !ch.fading) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            // Clamp the subtraction — see produceFrame; an MP3's framesRead()
            // can exceed the estimated totalFrames and underflow this.
            const uint32_t consumed = (srcRead < ws.totalFrames) ? srcRead : ws.totalFrames;
            const uint32_t framesLeft =
                ws.totalFrames - consumed + ws.availableRead();
            if (framesLeft <= ch.fadeOutTriggerFrames) {
                ch.fading               = true;
                ch.fadeVolume           = 1.0f;
                ch.fadeStep             = ch.fadeOutStep;
                ch.fadeOutTriggerFrames = 0;
            }
        }

        // Per-block fade — one volume value for the whole block.
        // Advance ch.fadeVolume by `got` steps for the next block.
        float effectiveVolume = ch.volume;
        bool  fadeOutComplete = false;
        if (ch.fading) {
            effectiveVolume *= ch.fadeVolume;
            ch.fadeVolume -= ch.fadeStep * (float)got;
            if (ch.fadeStep > 0.0f && ch.fadeVolume <= 0.0f) {
                ch.fadeVolume = 0.0f;
                ch.fading = false;
                fadeOutComplete = true;   // tear down AFTER mixing this block
            } else if (ch.fadeStep < 0.0f && ch.fadeVolume >= 1.0f) {
                ch.fadeVolume = 1.0f;
                ch.fading = false;
            }
        }

        // Apply vol + pan as a single Q15 multiply per side.  pan ×
        // vol fits in Q15 because both are in [0, 1].
        int16_t volLQ15, volRQ15;
        bool muteL = false, muteR = false;
        // Q15 unit is 32768 but int16 saturates at 32767, so a unity
        // (vol×pan == 1.0) product must clamp to 32767 before the narrowing
        // cast — otherwise it wraps to -32768 and inverts the audio phase.
        auto toVolQ15 = [](float f) -> int16_t {
            int32_t v = (int32_t)(f * 32768.0f);
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            return (int16_t)v;
        };
        if (ch.outputChannels == AudioChannel::ALL) {
            volLQ15 = toVolQ15(effectiveVolume * ch.panL);
            volRQ15 = toVolQ15(effectiveVolume * ch.panR);
        } else {
            const int16_t volQ15 = toVolQ15(effectiveVolume);
            volLQ15 = (ch.outputChannels & AudioChannel::CH1) ? volQ15 : (muteL = true, int16_t{0});
            volRQ15 = (ch.outputChannels & AudioChannel::CH2) ? volQ15 : (muteR = true, int16_t{0});
        }

        // ── Hot kernel ────────────────────────────────────────────────
#if SFX_HAS_ESP_DSP
        if (muteL) std::memset(trackL, 0, (size_t)got * sizeof(int16_t));
        else       dsps_mulc_s16_ae32(trackL, trackL, got, volLQ15, 1, 1);
        if (muteR) std::memset(trackR, 0, (size_t)got * sizeof(int16_t));
        else       dsps_mulc_s16_ae32(trackR, trackR, got, volRQ15, 1, 1);
#else
        for (int j = 0; j < got; ++j) {
            trackL[j] = (int16_t)(((int32_t)trackL[j] * (int32_t)volLQ15) >> 15);
            trackR[j] = (int16_t)(((int32_t)trackR[j] * (int32_t)volRQ15) >> 15);
        }
#endif

        // int16 → int32 widen + accumulate.  Compiler auto-vectorises
        // this on LX7 — no esp-dsp variant is needed (and one doesn't
        // exist for the s16→s32 mac shape).
        for (int j = 0; j < got; ++j) {
            mixL[j] += (int32_t)trackL[j];
            mixR[j] += (int32_t)trackR[j];
        }

        // Remaining-sec status — once per channel per block, off the
        // per-sample hot path.
        if (ws.sampleRate_Hz > 0) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            const uint32_t consumed = (srcRead < ws.totalFrames) ? srcRead : ws.totalFrames;
            const uint32_t framesLeft =
                ws.totalFrames - consumed + ws.availableRead();
            _channelRemainingSec[i] = (float)framesLeft / (float)ws.sampleRate_Hz;
        }

        // Deferred teardown for completed fade-outs / mid-block EOFs.
        if (fadeOutComplete || sourceDied) {
            ws.destroySafe();   // waits out the decoder, then frees
            ws.active.store(false, std::memory_order_release);
            _channelPlaying[i] = false;
            if (sourceDied) _channelRemainingSec[i] = 0.0f;
            m.checkAndPlayNextQueued(i);
        }
    }

    if (!hasAudio) return 0;

    // Master volume in Q15, clamp to int16, headroom × MAX_AMPLITUDE,
    // write to ring.  Per-sample loop — small enough that LX7 auto-vec
    // handles it; esp-dsp doesn't have a s32→s16 saturating-scale op
    // that fits this shape.
    const int32_t mvolQ15 = (int32_t)(_masterVolume * 32768.0f);
    for (int j = 0; j < N; ++j) {
        int32_t L = (mixL[j] * mvolQ15) >> 15;
        int32_t R = (mixR[j] * mvolQ15) >> 15;
        if (L >  32767) L =  32767; else if (L < -32768) L = -32768;
        if (R >  32767) R =  32767; else if (R < -32768) R = -32768;
        L = (L * (int32_t)MAX_AMPLITUDE) >> 15;
        R = (R * (int32_t)MAX_AMPLITUDE) >> 15;
        ringBuf().write((int16_t)L, (int16_t)R);
    }

    return N;
}

// ============================================================================
//  FLOAT-KERNEL BLOCK PRODUCER (Phase 5b experiment, 2026-05-28)
// ============================================================================
//
// Parallel to `produceBlock()`.  Same block-mode architecture
// (kBlockMax = 256 frames, per-block fade granularity, deferred
// teardown), but the per-channel scale + accumulate run in float32
// via esp-dsp's `dsps_mulc_f32_ae32` + `dsps_add_f32_ae32`.
//
// Trade-offs vs the int16 / Q15 kernel:
//   + Simpler arithmetic — no Q15 conversion, no overflow management,
//     no int32 widening accumulate.  Master volume + clamp + headroom
//     are one float multiply + clamp each.
//   + Pan + volume combine cleanly as a single float multiplier.
//   − 2× memory bandwidth on the per-block scratch (4 B vs 2 B per
//     sample → 8 KB per block-side scratch vs 4 KB).  Tight on the
//     LX7 L1 cache.
//   − int16→float conversion at the drain-read boundary (the drain
//     buffer is still int16 so we don't need to also flip its
//     storage).  One multiply + load per sample.
//   − Float SIMD lanes are 4-wide vs int16's 8-wide on the Xtensa
//     ext, so each SIMD instruction processes half as many lanes
//     per cycle (compensated by simpler kernel body).
//
// The outcome of `produceBlockFloat` vs `produceBlock` on the same
// gun-during-engine 2-channel trace decides the production kernel.

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::MixKernel::produceBlockFloat(AudioMixer& m, int maxFrames) {
#if !SFX_HAS_ESP_DSP
    // No esp-dsp = no float SIMD; fall through to the int16 kernel.
    return produceBlock(m, maxFrames);
#else
    auto& _channels            = m._channels;
    auto& _channelPlaying      = m._channelPlaying;
    auto& _channelRemainingSec = m._channelRemainingSec;
    auto& _masterVolume        = m._masterVolume;
    constexpr int kBlockMax = 256;

    const uint32_t freeSlots = ringBuf().availableWrite();
    int N = maxFrames;
    if (N > (int)freeSlots) N = (int)freeSlots;
    if (N > kBlockMax)      N = kBlockMax;
    if (N <= 0) return 0;

    // Float scratch — 4 KB per side (vs 1 KB for int16).  Total
    // stack: 4 × (mixL/mixR/trackL/trackR) × 4 B × 256 = 4 KB.
    // Fits the 8 KB producer task stack with comfortable headroom.
    float mixL[kBlockMax] = {};
    float mixR[kBlockMax] = {};
    float trackL[kBlockMax];
    float trackR[kBlockMax];
    bool hasAudio = false;

    for (int i = 0; i < AUDIO_MAX_CHANNELS; ++i) {
        Channel& ch = _channels[i];
        WavState& ws = ch.wav;
        if (!ws.active || ch.mute || ch.volume <= 0.0f) continue;

        // Pull up to N samples, convert int16 → float [-1, +1] inline.
        // Conversion: one int→float + one fmul per sample.  At 256
        // samples × 2 ch × ~6 active channels worst case, that's
        // ~3 k float conversions per block — small fraction of the
        // mix kernel itself, but a real cost we're trading for
        // simpler kernel math.
        constexpr float kInt16ToFloat = 1.0f / 32768.0f;
        int got = 0;
        bool sourceDied = false;
        for (int j = 0; j < N; ++j) {
            int16_t l = 0, r = 0;
            if (!ch.wav.readSample(l, r)) {
                sourceDied = true;
                break;
            }
            trackL[j] = (float)l * kInt16ToFloat;
            trackR[j] = (float)r * kInt16ToFloat;
            got = j + 1;
        }
        if (sourceDied && got == 0) {
            ws.destroySafe();   // waits out the decoder, then frees
            ws.active.store(false, std::memory_order_release);
            _channelPlaying[i] = false;
            _channelRemainingSec[i] = 0.0f;
            m.checkAndPlayNextQueued(i);
            continue;
        }
        if (got == 0) continue;
        hasAudio = true;

        // Arm tail fade-out (same logic as produceBlock).
        if (ch.fadeOutTriggerFrames > 0 && !ch.fading) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            // Clamp the subtraction — an MP3's framesRead() can exceed the
            // estimated totalFrames and underflow this unsigned difference.
            const uint32_t consumed = (srcRead < ws.totalFrames) ? srcRead : ws.totalFrames;
            const uint32_t framesLeft =
                ws.totalFrames - consumed + ws.availableRead();
            if (framesLeft <= ch.fadeOutTriggerFrames) {
                ch.fading               = true;
                ch.fadeVolume           = 1.0f;
                ch.fadeStep             = ch.fadeOutStep;
                ch.fadeOutTriggerFrames = 0;
            }
        }

        // Per-block fade.
        float effectiveVolume = ch.volume;
        bool  fadeOutComplete = false;
        if (ch.fading) {
            effectiveVolume *= ch.fadeVolume;
            ch.fadeVolume -= ch.fadeStep * (float)got;
            if (ch.fadeStep > 0.0f && ch.fadeVolume <= 0.0f) {
                ch.fadeVolume = 0.0f;
                ch.fading = false;
                fadeOutComplete = true;
            } else if (ch.fadeStep < 0.0f && ch.fadeVolume >= 1.0f) {
                ch.fadeVolume = 1.0f;
                ch.fading = false;
            }
        }

        // Combine vol + pan into a single float multiplier per side.
        float volL, volR;
        if (ch.outputChannels == AudioChannel::ALL) {
            volL = effectiveVolume * ch.panL;
            volR = effectiveVolume * ch.panR;
        } else {
            volL = (ch.outputChannels & AudioChannel::CH1) ? effectiveVolume : 0.0f;
            volR = (ch.outputChannels & AudioChannel::CH2) ? effectiveVolume : 0.0f;
        }

        // ── Float SIMD hot kernel ──────────────────────────────────────
        // dsps_mulc_f32_ae32: track[i] = track[i] * volX
        // dsps_add_f32_ae32:  mix[i]   = track[i] + mix[i]
        dsps_mulc_f32_ae32(trackL, trackL, got, volL, 1, 1);
        dsps_mulc_f32_ae32(trackR, trackR, got, volR, 1, 1);
        dsps_add_f32_ae32 (trackL, mixL,   mixL, got, 1, 1, 1);
        dsps_add_f32_ae32 (trackR, mixR,   mixR, got, 1, 1, 1);

        // Remaining-sec status.
        if (ws.sampleRate_Hz > 0) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            const uint32_t consumed = (srcRead < ws.totalFrames) ? srcRead : ws.totalFrames;
            const uint32_t framesLeft =
                ws.totalFrames - consumed + ws.availableRead();
            _channelRemainingSec[i] = (float)framesLeft / (float)ws.sampleRate_Hz;
        }

        if (fadeOutComplete || sourceDied) {
            ws.destroySafe();   // waits out the decoder, then frees
            ws.active.store(false, std::memory_order_release);
            _channelPlaying[i] = false;
            if (sourceDied) _channelRemainingSec[i] = 0.0f;
            m.checkAndPlayNextQueued(i);
        }
    }

    if (!hasAudio) return 0;

    // Master volume + clamp + headroom + write to ring — float scalar
    // loop.  Could use dsps_mulc_f32_ae32 for the master-vol step but
    // the inline loop is simple enough that LX7 auto-vec handles it
    // and we'd need a separate pass for the clamp + int conversion.
    const float masterAndHeadroom = _masterVolume * (float)MAX_AMPLITUDE;
    for (int j = 0; j < N; ++j) {
        float L = mixL[j] * _masterVolume;
        float R = mixR[j] * _masterVolume;
        if (L >  1.0f) L =  1.0f; else if (L < -1.0f) L = -1.0f;
        if (R >  1.0f) R =  1.0f; else if (R < -1.0f) R = -1.0f;
        ringBuf().write((int16_t)(L * (float)MAX_AMPLITUDE),
                        (int16_t)(R * (float)MAX_AMPLITUDE));
    }
    (void)masterAndHeadroom;  // reserved for the fused dsps variant

    return N;
#endif  // SFX_HAS_ESP_DSP
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::MixKernel::produceLoop(AudioMixer& m, int maxFrames) {
    // Produce frames into the ring buffer FIRST — time-critical.  The ring is
    // a small core-to-core FIFO (~85 ms) that must stay fed to prevent
    // consumer underruns.  Block-mode (produceBlock / produceBlockFloat) calls
    // esp-dsp's SIMD kernel per channel; produceFrame() stays the single-frame
    // fallback (tests + Pico without esp-dsp).
    //
    // int16-vs-float kernel is selected at COMPILE TIME below — `if constexpr`
    // replaces the old SFX_AUDIO_KERNEL_FLOAT `#if`, so both kernels are
    // type-checked and the dead one is discarded by the compiler.
    //
    // Phase 6: the per-cycle refill loop is GONE — refillDrainBuffer is owned
    // by the decoder task on Core 0; this loop only mixes from the ring.
    int produced = 0;
    while (produced < maxFrames) {
        int got;
        if constexpr (SFX_AUDIO_KERNEL_FLOAT)
            got = produceBlockFloat(m, maxFrames - produced);
        else
            got = produceBlock(m, maxFrames - produced);
        if (got <= 0) break;
        produced += got;
    }
    return produced;
}

// ============================================================================
//  I2S OUTPUT — CONSUMER (Core 1)
// ============================================================================


#endif // SFX_HAS_AUDIO
