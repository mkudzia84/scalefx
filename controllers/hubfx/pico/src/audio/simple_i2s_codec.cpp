/*
 * Simple I2S DAC Codec Driver - Implementation
 */

#include "simple_i2s_codec.h"
#include "../debug_config.h"

// Simple I2S codec log macro - takes runtime name as first arg
#if AUDIO_DEBUG
#define I2S_LOG(name, fmt, ...) Serial.printf("[%s] " fmt "\n", name, ##__VA_ARGS__)
#else
#define I2S_LOG(name, fmt, ...)
#endif

SimpleI2SCodec::SimpleI2SCodec(const char* model_name, int8_t mute_pin, int8_t gain_pin)
    : modelName(model_name), mutPin(mute_pin), gainPin(gain_pin),
      initialized(false), currentlyMuted(false), currentVolume(1.0f) {
}

bool SimpleI2SCodec::begin(uint32_t sample_rate) {
    I2S_LOG(modelName, "Initializing simple I2S codec...");
    
    // Configure control pins if provided
    if (mutPin >= 0) {
        pinMode(mutPin, OUTPUT);
        digitalWrite(mutPin, HIGH);  // Unmuted (typical active-low mute)
        I2S_LOG(modelName, "Mute control on GPIO%d", mutPin);
    }
    
    if (gainPin >= 0) {
        pinMode(gainPin, OUTPUT);
        digitalWrite(gainPin, HIGH);  // High gain (codec-dependent)
        I2S_LOG(modelName, "Gain control on GPIO%d", gainPin);
    }
    
    initialized = true;
    I2S_LOG(modelName, "Initialization complete (auto-configure from I2S)");
    I2S_LOG(modelName, "Sample rate: %lu Hz", sample_rate);
    
    return true;
}

void SimpleI2SCodec::reset() {
    // Simple codecs don't have reset - just reinitialize pins
    if (mutPin >= 0) {
        digitalWrite(mutPin, HIGH);  // Unmuted
    }
    currentlyMuted = false;
    currentVolume = 1.0f;
}

void SimpleI2SCodec::setVolume(float volume) {
    currentVolume = constrain(volume, 0.0f, 1.0f);
    
    // Simple codecs typically don't have software volume control
    // Volume must be handled in mixer or with external hardware
    I2S_LOG(modelName, "Volume set to %.0f%% (handled by mixer)", currentVolume * 100.0f);
    
    // Some codecs have gain pin (e.g., MAX98357 has GAIN slot)
    // This is a simple on/off example
    if (gainPin >= 0) {
        digitalWrite(gainPin, volume > 0.5f ? HIGH : LOW);
    }
}

void SimpleI2SCodec::setMute(bool mute) {
    currentlyMuted = mute;
    
    if (mutPin >= 0) {
        // Typical: mute is active-low (LOW = muted, HIGH = unmuted)
        digitalWrite(mutPin, mute ? LOW : HIGH);
        I2S_LOG(modelName, "%s", mute ? "Muted" : "Unmuted");
    } else {
        I2S_LOG(modelName, "Mute requested but no mute pin configured");
    }
}
