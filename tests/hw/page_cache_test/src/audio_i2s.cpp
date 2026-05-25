/*
 * audio_i2s.cpp — i2s_std-backed TX channel.
 *
 * The new I2S driver (ESP-IDF 5.x) is event-driven, has explicit
 * channel allocation, and replaces the legacy `driver/i2s.h` API.
 * Setup is three steps:
 *
 *   1. `i2s_new_channel(&chan_cfg, &tx, nullptr)` — allocate a TX channel
 *   2. `i2s_channel_init_std_mode(tx, &std_cfg)` — clock + slot config
 *   3. `i2s_channel_enable(tx)` — start DMA
 *
 * Sample-rate-paced writes via `i2s_channel_write(tx, buf, bytes,
 * &written, portMAX_DELAY)` block until DMA accepts everything.
 */

#include "audio_i2s.h"
#include <Arduino.h>
#include <driver/i2s_std.h>
#include <esp_err.h>

bool AudioI2S::begin(int bclk, int ws, int dout, uint32_t sampleRate) {
    if (_running) return true;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                             I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;     // 6 descriptors
    chan_cfg.dma_frame_num = 512;   // 512 stereo16 frames per buffer
    chan_cfg.auto_clear    = true;  // zero-fill on under-run (prevents
                                    // codec PLL drift between play bursts)

    i2s_chan_handle_t tx = nullptr;
    if (i2s_new_channel(&chan_cfg, &tx, nullptr) != ESP_OK) {
        Serial.printf("[I2S] i2s_new_channel failed\n");
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)bclk,
            .ws   = (gpio_num_t)ws,
            .dout = (gpio_num_t)dout,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK) {
        Serial.printf("[I2S] i2s_channel_init_std_mode failed\n");
        i2s_del_channel(tx);
        return false;
    }
    if (i2s_channel_enable(tx) != ESP_OK) {
        Serial.printf("[I2S] i2s_channel_enable failed\n");
        i2s_del_channel(tx);
        return false;
    }

    _tx       = tx;
    _running  = true;
    Serial.printf("[I2S] running: %u Hz / 16-bit stereo on BCLK=%d WS=%d DOUT=%d\n",
                  (unsigned)sampleRate, bclk, ws, dout);
    return true;
}

void AudioI2S::end() {
    if (!_running) return;
    auto tx = (i2s_chan_handle_t)_tx;
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    _tx      = nullptr;
    _running = false;
}

size_t AudioI2S::writeBlocking(const int16_t* interleaved, size_t frames) {
    return writeBytesBlocking(reinterpret_cast<const uint8_t*>(interleaved),
                               frames * sizeof(int16_t) * 2);
}

size_t AudioI2S::writeBytesBlocking(const uint8_t* buf, size_t bytes) {
    if (!_running || !buf || bytes == 0) return 0;
    size_t written = 0;
    if (i2s_channel_write((i2s_chan_handle_t)_tx, buf, bytes,
                          &written, portMAX_DELAY) != ESP_OK) {
        return written;
    }
    return written;
}
