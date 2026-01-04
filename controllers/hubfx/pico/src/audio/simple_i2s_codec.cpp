/*
 * Simple I2S DAC Codec Driver - Implementation
 */

#include "simple_i2s_codec.h"

SimpleI2SCodec::SimpleI2SCodec(const char* model_name, int8_t mute_pin, int8_t gain_pin)
    : modelName(model_name), mutPin(mute_pin), gainPin(gain_pin),
      initialized(false), currentlyMuted(false), currentVolume(1.0f) {
}

bool SimpleI2SCodec::begin(uint32_t sample_rate) {
    Serial.printf("[%s] Initializing simple I2S codec...\n", modelName);
    
    // Configure control pins if provided
    if (mutPin >= 0) {
        pinMode(mutPin, OUTPUT);
        digitalWrite(mutPin, HIGH);  // Unmuted (typical active-low mute)
        Serial.printf("[%s] Mute control on GPIO%d\n", modelName, mutPin);
    }
    
    if (gainPin >= 0) {
        pinMode(gainPin, OUTPUT);
        digitalWrite(gainPin, HIGH);  // High gain (codec-dependent)
        Serial.printf("[%s] Gain control on GPIO%d\n", modelName, gainPin);
    }
    
    initialized = true;
    Serial.printf("[%s] Initialization complete (auto-configure from I2S)\n", modelName);
    Serial.printf("[%s] Sample rate: %lu Hz\n", modelName, sample_rate);
    
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
    Serial.printf("[%s] Volume set to %.0f%% (handled by mixer)\n", 
                  modelName, currentVolume * 100.0f);
    
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
        Serial.printf("[%s] %s\n", modelName, mute ? "Muted" : "Unmuted");
    } else {
        Serial.printf("[%s] Mute requested but no mute pin configured\n", modelName);
    }
}
