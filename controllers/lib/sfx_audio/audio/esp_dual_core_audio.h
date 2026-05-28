/*
 * esp_dual_core_audio.h — ESP32-S3 dual-core audio bring-up helper.
 *
 *   Encapsulates the two-phase boot dance every ESP32-S3 audio board
 *   needs (HubFX 8-channel rev, future audio-capable expanders, …):
 *
 *       using Mixer = AudioMixer<EspI2SOutput, TAS5825PCodec>;
 *       static EspDualCoreAudio<Mixer> audio;
 *       audio.begin({.i2sDout=…, .i2sBclk=…, .i2sLrclk=…});
 *
 *   The helper takes a single template parameter — the mixer — and
 *   recovers the codec type via `TMixer::Codec`.  Codec-specific
 *   bring-up (I²C probe, supply-voltage selection, activate-to-PLAY)
 *   lives in a `CodecAdapter` trait the board specializes next to its
 *   pin map:
 *
 *       template <>
 *       struct CodecAdapter<TAS5825PCodec> {
 *           static constexpr bool kHasCodec = true;
 *           static bool probe() {
 *               return TAS5825PCodec::instance().begin(Wire,
 *                   Gpio::I2C_SDA, Gpio::I2C_SCL,
 *                   AUDIO_SAMPLE_RATE, Codec::SUPPLY_VOLTAGE);
 *           }
 *           static bool activate() {
 *               return TAS5825PCodec::instance().activate();
 *           }
 *       };
 *
 *   Boards with a passive DAC (PCM5102 / UDA1334 / …) leave the
 *   primary template in effect — `if constexpr (kHasCodec)` strips
 *   probe + activate entirely at compile time.
 *
 *   All dispatch is static (template-monomorphized) — no virtual
 *   dispatch, no `std::function`, no heap allocations.
 *
 *   Phase 1 (Core 0): optional `CodecAdapter::probe()`, mixer alloc.
 *   Core 1 task    : `TMixer::beginI2S()`, launch producer task,
 *                    enter consumer loop.
 *   Phase 2 (Core 0): wait for I²S clocks live, settle 50 ms,
 *                    optional `CodecAdapter::activate()`.
 *
 *   Build gate: `SFX_HAS_AUDIO` + ESP32 platform.  No-op on Pico
 *   (the class is not defined there).
 */

#ifndef SFX_AUDIO_ESP_DUAL_CORE_AUDIO_H
#define SFX_AUDIO_ESP_DUAL_CORE_AUDIO_H

#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <serial/diag_log.h>

#include "audio_mixer.h"
#include "audio_config.h"

// ─── CodecAdapter trait ────────────────────────────────────────────────
//
//   Primary template: "no codec" semantics (passive DAC / DAC chip that
//   needs no software bring-up).  `if constexpr (kHasCodec)` strips the
//   probe + activate paths at compile time.
//
//   Boards with a controllable codec specialize this trait next to
//   their pin map — the specialization picks up board-local pins,
//   supply voltage, etc., without leaking those into the helper.
//
template <typename TCodec>
struct CodecAdapter {
    static constexpr bool kHasCodec = false;
    static bool probe()    { return true; }
    static bool activate() { return true; }
};

template <MixerLike TMixer>
class EspDualCoreAudio {
public:
    /// Codec recovered from the mixer's nested typedef — single source
    /// of truth so the user can't accidentally pair the wrong adapter
    /// with their mixer instantiation.
    using CodecType = typename TMixer::Codec;
    using Adapter   = CodecAdapter<CodecType>;

    struct Config {
        int      i2sDout            = -1;
        int      i2sBclk            = -1;
        int      i2sLrclk           = -1;
        uint32_t i2sReadyTimeoutMs  = 2000;
    };

    EspDualCoreAudio() = default;
    EspDualCoreAudio(const EspDualCoreAudio&) = delete;
    EspDualCoreAudio& operator=(const EspDualCoreAudio&) = delete;

    /// Phase 1 + Core 1 task + Phase 2.  Returns true once the mixer is
    /// running on Core 1 AND (if `Adapter::kHasCodec`) the codec reached
    /// PLAY.  A false return still leaves the mixer producing into I²S
    /// if it came up — the codec just won't be audible until the
    /// caller retries (e.g. via a config-reload hook).
    bool begin(const Config& cfg) {
        _cfg = cfg;
        auto& mixer = TMixer::instance();

        // ── Phase 1A — codec probe (compiled out for passive DAC) ────
        bool codecProbed = true;
        if constexpr (Adapter::kHasCodec) {
            codecProbed = Adapter::probe();
            if (codecProbed) {
                SFX_LOG_INFO("Audio codec: probed, in DEEP_SLEEP");
            } else {
                SFX_LOG_WARN("Audio codec: probe failed — output disabled");
            }
        }

        // ── Phase 1B — mixer channels + ring buffer ───────────────────
        if (!mixer.begin(cfg.i2sDout, cfg.i2sBclk, cfg.i2sLrclk)) {
            SFX_LOG_ERROR("Audio mixer Phase 1 failed");
            return false;
        }
        _audioInitialized.store(true, std::memory_order_release);
        SFX_LOG_INFO("Audio mixer Phase 1 ready — handing off to Core 1");

        // ── Core 1 task launch ────────────────────────────────────────
        xTaskCreatePinnedToCore(&EspDualCoreAudio::core1TaskEntry,
                                "AudioConsumer",
                                /*stackSize=*/8192,
                                this,
                                configMAX_PRIORITIES - 1,
                                &_core1TaskHandle,
                                /*coreId=*/1);
        mixer.setConsumerTaskHandle(_core1TaskHandle);

        // ── Phase 2 — wait for I²S clocks, activate codec ────────────
        if constexpr (!Adapter::kHasCodec) {
            return true;   // passive DAC — mixer alone is enough
        } else {
            if (!codecProbed) return false;
            if (!awaitI2SReady(_cfg.i2sReadyTimeoutMs)) return false;
            vTaskDelay(pdMS_TO_TICKS(50));   // BCLK / LRCLK settle

            if (Adapter::activate()) {
                SFX_LOG_INFO("Audio codec: PLAY state — output live");
                return true;
            }
            SFX_LOG_WARN("Audio codec: activate failed — not in PLAY");
            return false;
        }
    }

    /// Block until Core 1 reports I²S clocks live, or timeout.  Exposed
    /// for boards that want to drive their codec activation manually
    /// instead of through `CodecAdapter::activate()`.
    bool awaitI2SReady(uint32_t timeout_ms) {
        const uint32_t waitStart = millis();
        while (!_i2sReady.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (millis() - waitStart > timeout_ms) {
                SFX_LOG_ERROR("Audio: timeout waiting for Core 1 I²S init");
                return false;
            }
        }
        return true;
    }

    // Read-only diagnostics — useful for STATUS payload contributors.
    bool audioInitialized() const { return _audioInitialized.load(std::memory_order_acquire); }
    bool i2sReady()         const { return _i2sReady.load(std::memory_order_acquire); }
    bool core1Ready()       const { return _core1Ready.load(std::memory_order_acquire); }
    uint32_t loop1Count()   const { return _loop1Count.load(std::memory_order_relaxed); }
    TaskHandle_t core1TaskHandle() const { return _core1TaskHandle; }

private:
    /// FreeRTOS entry point — Phase 2 prelude, then forever-consumer.
    static void core1TaskEntry(void* arg) {
        auto* self = static_cast<EspDualCoreAudio*>(arg);
        self->_core1Ready.store(true, std::memory_order_release);
        SFX_LOG_INFO("Core 1 audio task started — waiting for mixer init");

        while (!self->_audioInitialized.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        auto& mixer = TMixer::instance();
        if (!mixer.beginI2S()) {
            SFX_LOG_ERROR("Core 1: I²S init failed — audio disabled");
            while (true) {
                self->_loop1Count.fetch_add(1, std::memory_order_relaxed);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        SFX_LOG_INFO("Core 1: I²S running — codec can be activated");
        self->_i2sReady.store(true, std::memory_order_release);

        // Producer stays on Core 1 (same core as I²S consumer).  An
        // experiment moving it to Core 0 (2026-05-27, build #362) cut
        // peak SD read latency (88-110 ms → 14-55 ms — same core as
        // SDMMC ISR helps) BUT increased ring underruns 10× because
        // the producer didn't get enough CPU time on Core 0 (loopTask
        // + USB + SDMMC ISR contention).  Producer on Core 1
        // cooperates naturally with the consumer (consumer blocks on
        // I²S DMA write → producer runs), keeping the ring fed.
        mixer.startProducerTask(/*core=*/1,
                                /*priority=*/configMAX_PRIORITIES - 2,
                                /*stackSize=*/8192);

        while (true) {
            self->_loop1Count.fetch_add(1, std::memory_order_relaxed);
            mixer.consume();
        }
    }

    Config _cfg{};
    std::atomic<bool>     _audioInitialized {false};
    std::atomic<bool>     _i2sReady         {false};
    std::atomic<bool>     _core1Ready       {false};
    std::atomic<uint32_t> _loop1Count       {0};
    TaskHandle_t          _core1TaskHandle  = nullptr;
};

#endif  // SFX_PLATFORM_ESP32 && SFX_HAS_AUDIO
#endif  // SFX_AUDIO_ESP_DUAL_CORE_AUDIO_H
