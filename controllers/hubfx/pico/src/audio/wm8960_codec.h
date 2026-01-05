/*
 * WM8960 Audio Codec Driver
 * 
 * I2C control driver for Wolfson/Cirrus WM8960 stereo codec
 * Used with Waveshare WM8960 Audio HAT and compatible boards
 * 
 * Features:
 *  - I2C configuration (address 0x1A)
 *  - DAC path initialization
 *  - Speaker/headphone output control
 *  - Volume control
 *  - I2S interface setup (Slave mode, PLL from BCLK - no MCLK required)
 */

#ifndef WM8960_CODEC_H
#define WM8960_CODEC_H

#include <Arduino.h>
#include <Wire.h>
#include "audio_codec.h"

// WM8960 I2C Address
#define WM8960_I2C_ADDR 0x1A

// WM8960 Register Map
#define WM8960_REG_LINVOL       0x00  // Left Input Volume
#define WM8960_REG_RINVOL       0x01  // Right Input Volume
#define WM8960_REG_LOUT1        0x02  // LOUT1 Volume
#define WM8960_REG_ROUT1        0x03  // ROUT1 Volume
#define WM8960_REG_CLOCK1       0x04  // Clocking (1)
#define WM8960_REG_DACCTL1      0x05  // ADC & DAC Control (1)
#define WM8960_REG_DACCTL2      0x06  // ADC & DAC Control (2)
#define WM8960_REG_IFACE1       0x07  // Audio Interface (1)
#define WM8960_REG_CLOCK2       0x08  // Clocking (2)
#define WM8960_REG_IFACE2       0x09  // Audio Interface (2)
#define WM8960_REG_LDAC         0x0A  // Left DAC Volume
#define WM8960_REG_RDAC         0x0B  // Right DAC Volume

#define WM8960_REG_RESET        0x0F  // Reset

#define WM8960_REG_3D           0x10  // 3D Control
#define WM8960_REG_ALC1         0x11  // ALC1
#define WM8960_REG_ALC2         0x12  // ALC2
#define WM8960_REG_ALC3         0x13  // ALC3
#define WM8960_REG_NOISEGATE    0x14  // Noise Gate
#define WM8960_REG_LADC         0x15  // Left ADC Volume
#define WM8960_REG_RADC         0x16  // Right ADC Volume

#define WM8960_REG_ADDCTL1      0x17  // Additional Control (1)
#define WM8960_REG_ADDCTL2      0x18  // Additional Control (2)
#define WM8960_REG_POWER1       0x19  // Power Mgmt (1)
#define WM8960_REG_POWER2       0x1A  // Power Mgmt (2)
#define WM8960_REG_ADDCTL3      0x1B  // Additional Control (3)
#define WM8960_REG_ANTICLICK    0x1C  // Anti-Pop (1)
#define WM8960_REG_ANTIPOP2     0x1D  // Anti-Pop (2)

#define WM8960_REG_LINPATH      0x20  // Left Input Mixer
#define WM8960_REG_RINPATH      0x21  // Right Input Mixer
#define WM8960_REG_LOUTMIX      0x22  // Left Out Mix
#define WM8960_REG_ROUTMIX      0x25  // Right Out Mix

#define WM8960_REG_MONOMIX1     0x26  // Mono Out Mix (1)
#define WM8960_REG_MONOMIX2     0x27  // Mono Out Mix (2)
#define WM8960_REG_LOUT2        0x28  // LOUT2 Volume (Speaker)
#define WM8960_REG_ROUT2        0x29  // ROUT2 Volume (Speaker)
#define WM8960_REG_MONOOUT      0x2A  // Mono Out Volume

#define WM8960_REG_INBMIX1      0x2B  // Input Boost Mixer (1)
#define WM8960_REG_INBMIX2      0x2C  // Input Boost Mixer (2)
#define WM8960_REG_BYPASS1      0x2D  // Bypass (1)
#define WM8960_REG_BYPASS2      0x2E  // Bypass (2)

#define WM8960_REG_POWER3       0x2F  // Power Mgmt (3)
#define WM8960_REG_ADDCTL4      0x30  // Additional Control (4)
#define WM8960_REG_CLASSD1      0x31  // Class D Control (1)
#define WM8960_REG_CLASSD3      0x33  // Class D Control (3)
#define WM8960_REG_PLL1         0x34  // PLL N
#define WM8960_REG_PLL2         0x35  // PLL K (1)
#define WM8960_REG_PLL3         0x36  // PLL K (2)
#define WM8960_REG_PLL4         0x37  // PLL K (3)

// Audio interface format
#define WM8960_FMT_I2S          0x02
#define WM8960_FMT_LEFT_J       0x01
#define WM8960_FMT_RIGHT_J      0x00
#define WM8960_FMT_DSP          0x03

// Word length
#define WM8960_WL_16BIT         0x00
#define WM8960_WL_20BIT         0x01
#define WM8960_WL_24BIT         0x02
#define WM8960_WL_32BIT         0x03

class WM8960Codec : public AudioCodec {
public:
    WM8960Codec();
    
    /**
     * Initialize WM8960 codec with I2C interface
     * 
     * @param wire I2C interface to use (Wire or Wire1)
     * @param sda_pin I2C SDA pin (e.g. GP4)
     * @param scl_pin I2C SCL pin (e.g. GP5)
     * @param sample_rate Audio sample rate (default: 44100)
     * @return true if successful, false otherwise
     */
    bool begin(TwoWire& wire, uint8_t sda_pin, uint8_t scl_pin, uint32_t sample_rate = 44100);
    
    // AudioCodec interface implementation
    bool begin(uint32_t sample_rate = 44100) override;
    void reset() override;
    void setVolume(float volume) override;
    void setMute(bool mute) override;
    bool isInitialized() const override { return initialized; }
    const char* getModelName() const override { return "WM8960"; }
    
    // WM8960-specific features
    void enableSpeakers(bool enable) override;
    void enableHeadphones(bool enable) override;
    void setHeadphoneVolume(uint8_t volume) override;
    void setSpeakerVolume(uint8_t volume) override;
    void dumpRegisters() override;
    
#if AUDIO_DEBUG
    // Debug methods (AudioCodec interface)
    bool testCommunication() override;
    uint16_t readRegisterCache(uint8_t reg) const override;
    bool writeRegisterDebug(uint8_t reg, uint16_t value) override;
    void printStatus() override;
    void reinitialize(uint32_t sample_rate = 44100) override;
    void* getCommunicationInterface() override { return wire; }
    
    // Recovery methods
    bool recoverI2C();
#endif // AUDIO_DEBUG
    
private:
    TwoWire* wire;
    bool initialized;
    
    // Register cache (WM8960 is write-only, cache for read-modify-write)
    uint16_t regCache[56];
    
    /**
     * Write to WM8960 register
     * WM8960 uses 9-bit register addresses and 9-bit data
     * Transmitted as: [7-bit I2C addr][R/W=0][9-bit reg addr][9-bit data]
     */
    bool writeRegister(uint8_t reg, uint16_t value);
    
    /**
     * Update cached register bits
     */
    void updateBits(uint8_t reg, uint16_t mask, uint16_t value);
    
    /**
     * Initialize power management
     */
    void initPower();
    
    /**
     * Initialize clocking (I2S slave mode with PLL from BCLK - no MCLK required)
     */
    void initClock(uint32_t sample_rate);
    
    /**
     * Initialize audio interface (I2S format)
     */
    void initInterface();
    
    /**
     * Initialize DAC path
     */
    void initDAC();
    
    /**
     * Initialize output mixers
     */
    void initOutputs();
};

#endif // WM8960_CODEC_H
