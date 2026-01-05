/*
 * Audio Codec Interface - Abstract Base Class
 * 
 * Generic interface for I2S audio codecs with I2C/SPI control.
 * Allows different codec implementations (WM8960, PCM5102, CS4344, etc.)
 * to be used interchangeably with the audio mixer.
 */

#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <Arduino.h>
#include "audio_config.h"

/**
 * Abstract base class for audio codec drivers
 * 
 * Codecs must implement I2S audio output with optional control interface (I2C/SPI).
 * Minimum requirement: initialize codec hardware and configure I2S parameters.
 */
class AudioCodec {
public:
    virtual ~AudioCodec() = default;
    
    /**
     * Initialize codec hardware
     * 
     * @param sample_rate Audio sample rate in Hz (e.g., 44100, 48000)
     * @return true if successful, false otherwise
     */
    virtual bool begin(uint32_t sample_rate = 44100) = 0;
    
    /**
     * Reset codec to default state
     */
    virtual void reset() = 0;
    
    /**
     * Set master output volume
     * 
     * @param volume Volume level from 0.0 (mute) to 1.0 (max)
     */
    virtual void setVolume(float volume) = 0;
    
    /**
     * Mute/unmute audio output
     * 
     * @param mute true to mute, false to unmute
     */
    virtual void setMute(bool mute) = 0;
    
    /**
     * Check if codec is initialized
     * 
     * @return true if codec is ready, false otherwise
     */
    virtual bool isInitialized() const = 0;
    
    /**
     * Get codec name/model for identification
     * 
     * @return Codec model string (e.g., "WM8960", "PCM5102")
     */
    virtual const char* getModelName() const = 0;
    
    // Optional features (default implementations do nothing)
    
    /**
     * Enable/disable speaker output (if codec has separate speaker amp)
     * 
     * @param enable true to enable, false to disable
     */
    virtual void enableSpeakers(bool enable) { (void)enable; }
    
    /**
     * Enable/disable headphone output (if codec has separate headphone amp)
     * 
     * @param enable true to enable, false to disable
     */
    virtual void enableHeadphones(bool enable) { (void)enable; }
    
    /**
     * Set headphone-specific volume (if codec supports separate control)
     * 
     * @param volume Volume level (codec-specific scale)
     */
    virtual void setHeadphoneVolume(uint8_t volume) { (void)volume; }
    
    /**
     * Set speaker-specific volume (if codec supports separate control)
     * 
     * @param volume Volume level (codec-specific scale)
     */
    virtual void setSpeakerVolume(uint8_t volume) { (void)volume; }
    
    /**
     * Print codec registers or status for debugging
     */
    virtual void dumpRegisters() {}
    
#if AUDIO_DEBUG
    // ========================================================================
    // DEBUG METHODS (Compiled when AUDIO_DEBUG=1 in audio_config.h)
    // ========================================================================
    
    /**
     * Test I2C/SPI communication with codec
     * @return true if communication successful
     */
    virtual bool testCommunication() { return false; }
    
    /**
     * Read from codec register cache (if supported)
     * @param reg Register address
     * @return Register value or 0xFFFF if not supported
     */
    virtual uint16_t readRegisterCache(uint8_t reg) const { (void)reg; return 0xFFFF; }
    
    /**
     * Write to codec register with debug logging
     * @param reg Register address
     * @param value Value to write
     * @return true if successful
     */
    virtual bool writeRegisterDebug(uint8_t reg, uint16_t value) { (void)reg; (void)value; return false; }
    
    /**
     * Print comprehensive codec status
     */
    virtual void printStatus() {}
    
    /**
     * Reinitialize codec with current settings
     * @param sample_rate Sample rate (default: current rate)
     */
    virtual void reinitialize(uint32_t sample_rate = 44100) { (void)sample_rate; }
    
    /**
     * Get I2C/SPI interface pointer for bus scanning
     * @return Pointer to communication interface (TwoWire* or SPIClass*)
     */
    virtual void* getCommunicationInterface() { return nullptr; }
#endif // AUDIO_DEBUG
};

#endif // AUDIO_CODEC_H
