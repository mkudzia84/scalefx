/**
 * ESP32 I2S Output — ESP32-S3 Implementation
 *
 * Wraps the ESP-IDF legacy I2S driver (v4.4). Uses bulk i2s_write()
 * for efficient DMA transfers. Satisfies the TI2S template concept
 * for AudioMixer<TI2S, TCodec>.
 *
 * The batch buffer is allocated from internal SRAM (DMA-capable).
 * I2S DMA descriptors require SRAM — PSRAM is not DMA-accessible.
 */

#ifndef ESP_I2S_OUTPUT_H
#define ESP_I2S_OUTPUT_H

#include "audio_ring_buffer.h"
#include "audio_config.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <driver/i2s.h>
#include <driver/gpio.h>
#include "audio_log.h"

class EspI2SOutput {
public:
    static EspI2SOutput& instance() {
        static EspI2SOutput inst;
        return inst;
    }

    // Delete copy/move
    EspI2SOutput(const EspI2SOutput&) = delete;
    EspI2SOutput& operator=(const EspI2SOutput&) = delete;
    EspI2SOutput(EspI2SOutput&&) = delete;
    EspI2SOutput& operator=(EspI2SOutput&&) = delete;

    bool begin(const I2SPinConfig& pins, uint32_t sampleRate, uint8_t bitDepth) {
        if (_running) return true;

        // ESP32-S3: Legacy I2S driver (IDF 4.4)
        i2s_config_t i2s_cfg = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
            .sample_rate = sampleRate,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count = 8,
            .dma_buf_len = 512,     // 8 × 512 samples = 16 KB DMA total
            .use_apll = false,
            .tx_desc_auto_clear = true,
        };

        esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, nullptr);
        if (err != ESP_OK) {
            MIXER_ERROR("i2s_driver_install failed: %d", err);
            return false;
        }

        i2s_pin_config_t pin_cfg = {
            .mck_io_num = I2S_PIN_NO_CHANGE,
            .bck_io_num = (int)pins.bclkPin,
            .ws_io_num  = (int)pins.lrclkPin,
            .data_out_num = (int)pins.dataPin,
            .data_in_num  = I2S_PIN_NO_CHANGE,
        };

        err = i2s_set_pin(I2S_NUM_0, &pin_cfg);
        if (err != ESP_OK) {
            MIXER_ERROR("i2s_set_pin failed: %d", err);
            i2s_driver_uninstall(I2S_NUM_0);
            return false;
        }

        _running = true;
        return true;
    }

    void end() {
        if (!_running) return;
        i2s_stop(I2S_NUM_0);
        i2s_driver_uninstall(I2S_NUM_0);
        _running = false;
    }

    size_t writeSamples(const StereoFrame* frames, size_t count) {
        // Batch into _batchBuf in internal SRAM, then DMA write.
        // Process in chunks of BATCH_FRAMES to stay within SRAM batch buffer.
        size_t totalWritten = 0;
        while (count > 0) {
            size_t chunk = (count > BATCH_FRAMES) ? BATCH_FRAMES : count;
            // Interleave into int16 pairs: [L0, R0, L1, R1, ...]
            for (size_t i = 0; i < chunk; i++) {
                _batchBuf[i * 2]     = frames[i].left;
                _batchBuf[i * 2 + 1] = frames[i].right;
            }
            size_t bytesWritten = 0;
            i2s_write(I2S_NUM_0, _batchBuf, chunk * 4, &bytesWritten, portMAX_DELAY);
            totalWritten += bytesWritten / 4;
            frames += chunk;
            count  -= chunk;
        }
        return totalWritten;
    }

    void writeSilence() {
        int16_t silence[2] = {0, 0};
        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_0, silence, 4, &bytesWritten, portMAX_DELAY);
    }

    bool isRunning() const { return _running; }

    const char* backendName() const { return "ESP-IDF-I2S"; }

private:
    EspI2SOutput() = default;

    static constexpr size_t BATCH_FRAMES = 512;  // 512 stereo frames = 2 KB
    int16_t _batchBuf[BATCH_FRAMES * 2];          // Interleaved L/R in internal SRAM
    bool _running = false;
};

#endif // ARDUINO_ARCH_ESP32
#endif // ESP_I2S_OUTPUT_H
