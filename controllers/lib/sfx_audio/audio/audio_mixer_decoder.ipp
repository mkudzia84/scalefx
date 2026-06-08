/*
 * AudioMixer<TI2S,TCodec>::DecoderWorker — the Core-0 decode-prefetch task,
 * its own .ipp (one class per file).
 *
 * Owns the FreeRTOS decoder task lifecycle (notify / start / stop) + taskFunc,
 * which on its 5 ms tick walks the mixer's active channels and refills any
 * hungry SPSC ring, guarding each refill with the decoderBusy seq_cst
 * handshake (closes the rapid-toggle use-after-free).  taskFunc reaches the
 * mixer via AudioMixer::instance().  ESP32-only.  #included by audio_mixer.h
 * after audio_mixer.ipp (shares its preamble in the one TU).
 */

#if defined(SFX_HAS_AUDIO)
#if SFX_PLATFORM_ESP32

// ============================================================================
//  DECODER TASK (Phase 6, feature/audio-decode-prefetch, 2026-05-28)
// ============================================================================
//
// Owns the entire decode pipeline on Core 0.  Producer task on Core 1
// only reads from the per-channel SPSC rings.  This is the structural
// fix for the play-start under+ spike and the 5-channel sustained
// stutter, both of which were rooted in synchronous libhelix decode
// blocking the producer task for ~30-100 ms.
//
// Wake sources:
//   - xTaskNotifyGive from notifyDecoder() (play() / stop() / restart)
//   - kDecoderTickMs (5 ms) periodic timeout — catches channels whose
//     rings are draining without a wake (steady-state mixing)
//
// Each pass walks AUDIO_MAX_CHANNELS and calls refillDrainBuffer for
// any channel with availableWrite() >= kRefillThreshold (currently
// half the ring).  refillDrainBuffer itself is now lock-free against
// the producer — see audio_mixer.ipp line ~890 for the SPSC write
// implementation.

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::DecoderWorker::notify() {
    // Extract-to-local (Rule 15): the handle can be nulled by stop()
    // on Core 0 between a check and a use; load once and act on the snapshot.
    auto h = _decoderTaskHandle.load(std::memory_order_acquire);
    if (h) xTaskNotifyGive(h);
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::DecoderWorker::taskFunc(void* /*param*/) {
    auto& mixer = AudioMixer::instance();
    MIXER_LOG("Decoder task started on core %d (priority %d)",
              xPortGetCoreID(), uxTaskPriorityGet(nullptr));

    constexpr uint32_t kRefillThresholdFrames = (uint32_t)WAV_BUF_FRAMES / 2;

    while (mixer._decoder._decoderRunning.load(std::memory_order_acquire)) {
        // Wait for a wake or the periodic tick.  Tick = 5 ms keeps the
        // worst-case latency on idle-but-active channels short.
        ulTaskNotifyTake(/*clearOnExit*/ pdTRUE, pdMS_TO_TICKS(kDecoderTickMs));

        for (int i = 0; i < AUDIO_MAX_CHANNELS; ++i) {
            Channel& ch = mixer._channels[i];
            WavState& ws = ch.wav;
            if (!ws.active.load(std::memory_order_acquire)) continue;
            if (ws.sourceExhausted.load(std::memory_order_acquire)) continue;

            // Claim the channel so a concurrent teardown waits for us to leave
            // refill before freeing `source`.  RE-CHECK active after the claim
            // (seq_cst) — if a teardown cleared active just before we claimed,
            // skip: it owns the source now.  This handshake fixes the
            // rapid-toggle use-after-free in refillDrainBuffer (LoadProhibited@0).
            ws.decoderBusy.store(true, std::memory_order_seq_cst);
            if (ws.active.load(std::memory_order_seq_cst) && ws.source) {
                const bool needs = ws.needsPrefill.load(std::memory_order_relaxed);
                const bool hungry = ws.availableWrite() >= kRefillThresholdFrames;
                if (needs || hungry) {
                    mixer.refillDrainBuffer(ch);
                }
            }
            ws.decoderBusy.store(false, std::memory_order_release);
        }
    }

    MIXER_LOG("Decoder task exiting");
    mixer._decoder._decoderExited.store(true, std::memory_order_release);
    vTaskSuspend(nullptr);
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::DecoderWorker::start(AudioMixer& m) {
    if (_decoderRunning.load(std::memory_order_acquire)) return true;
    if (!m._initialized.load(std::memory_order_acquire)) {
        MIXER_ERROR("startDecoderTask() called before begin()");
        return false;
    }

    _decoderExited .store(false, std::memory_order_release);
    _decoderRunning.store(true,  std::memory_order_release);

    TaskHandle_t newHandle = nullptr;
    BaseType_t result = xTaskCreatePinnedToCore(
        taskFunc, "AudioDecoder",
        _decoderStackSize, nullptr,
        _decoderPriority, &newHandle,
        _decoderCore
    );
    if (result != pdPASS) {
        _decoderRunning.store(false, std::memory_order_release);
        MIXER_ERROR("Failed to create decoder task (err=%d)", result);
        return false;
    }
    _decoderTaskHandle.store(newHandle, std::memory_order_release);

    MIXER_LOG("Decoder task created: core=%d priority=%d stack=%d",
              _decoderCore, _decoderPriority, _decoderStackSize);
    return true;
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::DecoderWorker::stop() {
    if (!_decoderRunning.load(std::memory_order_acquire)) return;

    MIXER_LOG("Stopping decoder task...");
    _decoderRunning.store(false, std::memory_order_release);
    // Wake it so it sees the flag and exits.  Null the published handle BEFORE
    // the wait+delete so a concurrent notifyDecoder() on the producer can't pick
    // it up again (it loads acquire and skips on null).
    TaskHandle_t handle = _decoderTaskHandle.exchange(nullptr, std::memory_order_acq_rel);
    if (handle) xTaskNotifyGive(handle);
    if (handle) {
        for (int i = 0; i < 40; ++i) {
            if (_decoderExited.load(std::memory_order_acquire)) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!_decoderExited.load(std::memory_order_acquire)) {
            MIXER_WARN("Decoder task did not exit cleanly after 200 ms — forcing delete");
        }
        vTaskDelete(handle);
    }
    MIXER_LOG("Decoder task stopped");
}

#endif // SFX_PLATFORM_ESP32
#endif // SFX_HAS_AUDIO
