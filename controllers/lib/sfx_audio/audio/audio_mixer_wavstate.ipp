/*
 * AudioMixer<TI2S,TCodec>::WavState — the per-channel SPSC decode ring.
 *
 * One class, one file.  WavState owns the lock-free decode ring (its
 * read/write index helpers are inline in audio_mixer.h); the two CROSS-CORE
 * seq_cst methods live here:
 *   destroySafe() — clears `active`, waits out the decoder's in-flight refill
 *                   (the decoderBusy handshake that closes the rapid-toggle
 *                   use-after-free), then runs the source dtor.
 *   readSample()  — the producer-side resampled int16 read (advances readIdx
 *                   with release).
 * The decoder WRITE side (refillDrainBuffer) is an AudioMixer method (it owns
 * the global SD-read telemetry) and lives in audio_mixer.ipp.
 *
 * #included by audio_mixer.h after audio_mixer.ipp (shares its preamble, one TU).
 */

#if defined(SFX_HAS_AUDIO)

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::WavState::destroySafe() {
    WavState& ws = *this;   // keep the seq_cst body below byte-identical
    // Mark the channel dead FIRST (seq_cst pairs with the decoder claiming
    // decoderBusy then re-checking active) ...
    ws.active.store(false, std::memory_order_seq_cst);
    // ... then wait (bounded) for the decoder to leave any in-flight refill on
    // this channel before we run the destructor.  A refill decodes
    // ≤ kMaxRefillFrames (~7 ms of MP3), so this spin is short in practice; the
    // cap prevents a hang if the decoder task is wedged.  Runs on the producer
    // / command task (NEVER the decoder), so it can't self-deadlock.
    // seq_cst load pairs with the decoder's seq_cst active.store/busy.store so
    // the active-store above can't reorder past this busy-load (Peterson-style
    // mutual exclusion — acquire alone would allow the StoreLoad reorder).
    for (uint32_t i = 0; i < kTeardownWaitMs &&
                         ws.decoderBusy.load(std::memory_order_seq_cst); ++i) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (ws.source) { ws.source->~IAudioSource(); ws.source = nullptr; }  // inlined dtor
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::WavState::readSample(int16_t& outL, int16_t& outR) {
    WavState& ws = *this;   // keep the producer-side body below byte-identical
    constexpr uint32_t kRingMask = (uint32_t)WAV_BUF_FRAMES - 1;

    // SPSC read side (Phase 6).  availableRead() does an acquire load
    // of writeIdx that pairs with the decoder task's release on
    // commitWrite; readIdx is loaded relaxed because the producer is
    // its sole writer.
    const uint32_t avail = ws.availableRead();
    const bool     act   = ws.active.load(std::memory_order_acquire);
    if (avail < 2 && act) {
        // Drain ran below the linear-interp threshold (need 2 samples
        // for one interpolated output).  Could be a transient (decoder
        // hasn't filled yet) or a real source EOF.
#if SFX_AUDIO_PACE_TELEMETRY
        ws.starves.fetch_add(1, std::memory_order_relaxed);
#endif
        // If the decoder marked the source done, tear down NOW even if
        // there's 1 sample remaining — we can't interp from a single
        // sample (no neighbor) and waiting for avail==0 stalls forever
        // when resampleRatio < 1 (24 kHz→48 kHz upsamples advance 0
        // frames every other output sample, so the avail-1 clamp on
        // commitRead pins advance=0 once avail==1).  Verified 2026-05-28
        // against the Phase 6 trace: ch0 init.mp3 (24 kHz mono) stuck
        // active with empty drain for 1+ s after natural EOF.
        if (ws.sourceExhausted.load(std::memory_order_acquire)) {
            if (avail > 0) ws.commitRead(avail);  // drain the residue
            outL = outR = 0;
            return false;
        }
        outL = outR = 0;
        return true;        // transient drain; keep channel alive
    }

    const uint32_t rIdx     = ws.readIdx.load(std::memory_order_relaxed);
    const uint32_t pos0     = rIdx       & kRingMask;
    const uint32_t pos1     = (rIdx + 1) & kRingMask;
    const int32_t  fracQ15  = (int32_t)(ws.resampleFrac * 32768.0f);
    const int32_t  aL = ws.bufL[pos0];
    const int32_t  bL = ws.bufL[pos1];
    const int32_t  aR = ws.bufR[pos0];
    const int32_t  bR = ws.bufR[pos1];
    outL = (int16_t)(aL + (((bL - aL) * fracQ15) >> 15));
    outR = (int16_t)(aR + (((bR - aR) * fracQ15) >> 15));

    // Advance resampler — float frac → int advance, then publish the
    // commit to the decoder.  We never advance past `avail - 1` (the
    // interp needs sample [rIdx+1] to be valid too); clamping here
    // prevents a brief over-read at the drain boundary.
    ws.resampleFrac += ws.resampleRatio;
    uint32_t advance = (uint32_t)ws.resampleFrac;
    if (advance > avail - 1) advance = avail - 1;
    if (advance > 0) ws.commitRead(advance);
    ws.resampleFrac -= (float)advance;
    return true;
}

#endif // SFX_HAS_AUDIO
