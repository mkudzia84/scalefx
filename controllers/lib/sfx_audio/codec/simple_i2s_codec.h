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

#include <Arduino.h>
#include "../audio/audio_config.h"

/**
 * Simple I2S DAC implementation
 * 
 * For codecs that don't need control interface:
 *  - No I2C/SPI required
 *  - Automatic configuration from I2S signals
 *  - Fixed or pin-controlled volume/mute
 */
class SimpleI2SCodec {
public:
    static SimpleI2SCodec& instance() {
        static SimpleI2SCodec inst;
        return inst;
    }

    // Delete copy/move
    SimpleI2SCodec(const SimpleI2SCodec&) = delete;
    SimpleI2SCodec& operator=(const SimpleI2SCodec&) = delete;
    SimpleI2SCodec(SimpleI2SCodec&&) = delete;
    SimpleI2SCodec& operator=(SimpleI2SCodec&&) = delete;

    // Configuration (call before begin())
    void setModelName(const char* name) { modelName = name; }
    void setMutePin(int8_t pin) { mutPin = pin; }
    void setGainPin(int8_t pin) { gainPin = pin; }
    
    // Codec interface
    bool begin(uint32_t sample_rate = 44100);
    void reset();
    void setVolume(float volume);
    void setMute(bool mute);
    bool isInitialized() const { return initialized; }
    const char* getModelName() const { return modelName; }
    
private:
    SimpleI2SCodec() = default;

    const char* modelName = "I2S-DAC";
    int8_t mutPin = -1;
    int8_t gainPin = -1;
    bool initialized = false;
    bool currentlyMuted = false;
    float currentVolume = 1.0f;
};

#endif // SIMPLE_I2S_CODEC_H
