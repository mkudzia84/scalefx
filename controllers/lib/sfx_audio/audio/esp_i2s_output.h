/**
 * ESP32 I2S Output — ESP32-S3 Implementation
 *
 * Wraps the ESP-IDF v5.x I2S standard-mode driver. Uses i2s_channel_write()
 * for efficient DMA transfers. Satisfies the TI2S template concept
 * for AudioMixer<TI2S, TCodec>.
 *
 * The batch buffer is allocated from internal SRAM (DMA-capable).
 * I2S DMA descriptors require SRAM — PSRAM is not DMA-accessible.
 *
 * Migration note: ESP-IDF 5.x replaced the legacy driver/i2s.h with
 * driver/i2s_std.h (standard mode), driver/i2s_pdm.h, driver/i2s_tdm.h.
 */

#ifndef ESP_I2S_OUTPUT_H
#define ESP_I2S_OUTPUT_H

#include "audio_ring_buffer.h"
#include "audio_config.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <driver/i2s_std.h>
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

        // ESP-IDF 5.x: New I2S channel-based driver
        // Step 1: Create TX channel
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        chan_cfg.dma_desc_num = 8;
        chan_cfg.dma_frame_num = 512;     // 8 × 512 frames = DMA ring buffer
        chan_cfg.auto_clear_after_cb = true;  // Zero DMA buffers after TX — prevents last-buffer loop on underrun

        esp_err_t err = i2s_new_channel(&chan_cfg, &_txHandle, nullptr);
        if (err != ESP_OK) {
            MIXER_ERROR("i2s_new_channel failed: %d", err);
            return false;
        }

        // Step 2: Configure standard mode (I2S Philips)
        // Map bitDepth to ESP-IDF enum (compile-time known, no runtime overhead)
        constexpr i2s_data_bit_width_t dataBits =
            (AUDIO_BIT_DEPTH == 32) ? I2S_DATA_BIT_WIDTH_32BIT :
            (AUDIO_BIT_DEPTH == 24) ? I2S_DATA_BIT_WIDTH_24BIT :
                                      I2S_DATA_BIT_WIDTH_16BIT;
        i2s_std_config_t std_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(dataBits, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)pins.bclkPin,
                .ws   = (gpio_num_t)pins.lrclkPin,
                .dout = (gpio_num_t)pins.dataPin,
                .din  = I2S_GPIO_UNUSED,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv   = false,
                },
            },
        };

        err = i2s_channel_init_std_mode(_txHandle, &std_cfg);
        if (err != ESP_OK) {
            MIXER_ERROR("i2s_channel_init_std_mode failed: %d", err);
            i2s_del_channel(_txHandle);
            _txHandle = nullptr;
            return false;
        }

        // Step 3: Enable the channel (starts DMA)
        err = i2s_channel_enable(_txHandle);
        if (err != ESP_OK) {
            MIXER_ERROR("i2s_channel_enable failed: %d", err);
            i2s_del_channel(_txHandle);
            _txHandle = nullptr;
            return false;
        }

        _running = true;
        return true;
    }

    void end() {
        if (!_running) return;
        if (_txHandle) {
            i2s_channel_disable(_txHandle);
            i2s_del_channel(_txHandle);
            _txHandle = nullptr;
        }
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
            i2s_channel_write(_txHandle, _batchBuf, chunk * 4, &bytesWritten, portMAX_DELAY);
            totalWritten += bytesWritten / 4;
            frames += chunk;
            count  -= chunk;
        }
        return totalWritten;
    }

    void writeSilence() {
        int16_t silence[2] = {0, 0};
        size_t bytesWritten = 0;
        i2s_channel_write(_txHandle, silence, 4, &bytesWritten, portMAX_DELAY);
    }

    bool isRunning() const { return _running; }

    const char* backendName() const { return "ESP-IDF-I2S-v5"; }

private:
    EspI2SOutput() = default;

    static constexpr size_t BATCH_FRAMES = 512;  // 512 stereo frames = 2 KB
    int16_t _batchBuf[BATCH_FRAMES * 2];          // Interleaved L/R in internal SRAM
    i2s_chan_handle_t _txHandle = nullptr;
    bool _running = false;
};

#endif // ARDUINO_ARCH_ESP32
#endif // ESP_I2S_OUTPUT_H
