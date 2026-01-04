/*
 * Simple I2S DAC Codec Driver
 * 
 * Generic driver for simple I2S DAC chips without control interface:
 *  - PCM5102 (TI)
 *  - PT8211 (Princeton Technology)
 *  - UDA1334 (NXP)
 *  - MAX98357 (Maxim, with I2S + SD control)
 * 
 * These codecs typically don't require I2C/SPI configuration - they
 * automatically configure themselves based on I2S clock signals.
 */

#ifndef SIMPLE_I2S_CODEC_H
#define SIMPLE_I2S_CODEC_H

#include "audio_codec.h"

/**
 * Simple I2S DAC implementation
 * 
 * For codecs that don't need control interface:
 *  - No I2C/SPI required
 *  - Automatic configuration from I2S signals
 *  - Fixed or pin-controlled volume/mute
 */
class SimpleI2SCodec : public AudioCodec {
public:
    /**
     * Create simple I2S codec
     * 
     * @param model_name Codec model string (e.g., "PCM5102", "PT8211")
     * @param mute_pin Optional GPIO pin for hardware mute control (-1 = none)
     * @param gain_pin Optional GPIO pin for gain control (-1 = none)
     */
    SimpleI2SCodec(const char* model_name = "I2S-DAC", int8_t mute_pin = -1, int8_t gain_pin = -1);
    
    // AudioCodec interface
    bool begin(uint32_t sample_rate = 44100) override;
    void reset() override;
    void setVolume(float volume) override;
    void setMute(bool mute) override;
    bool isInitialized() const override { return initialized; }
    const char* getModelName() const override { return modelName; }
    
private:
    const char* modelName;
    int8_t mutPin;
    int8_t gainPin;
    bool initialized;
    bool currentlyMuted;
    float currentVolume;
};

#endif // SIMPLE_I2S_CODEC_H
