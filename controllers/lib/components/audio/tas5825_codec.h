/**
 * @file tas5825_codec.h
 * @brief TI TAS5825M Digital Audio Amplifier Driver
 * 
 * Driver for Texas Instruments TAS5825M stereo Class-D audio amplifier with I2C control.
 * Based on the bassowl-hat project by Dario Murgia.
 * 
 * Features:
 * - I2C control interface (address 0x4C)
 * - Book/Page register architecture
 * - Digital volume control with 0.5dB steps
 * - Supports I2S and Left-Justified audio formats
 * - Built-in DSP with configurable EQ/DRC
 * - Fault monitoring and protection
 * - Multiple supply voltage configurations (12V, 15V, 20V, 24V)
 * 
 * Default Configuration:
 * - Sample Rate: 44.1kHz (configurable)
 * - Audio Format: I2S 16-bit
 * - Volume: 0dB (register 0x4C = 0x30)
 * - Supply: 20V configuration
 */

#ifndef TAS5825_CODEC_H
#define TAS5825_CODEC_H

#include "audio_codec.h"
#include <Wire.h>

// TAS5825M I2C Address
#define TAS5825M_I2C_ADDR  0x4C

// TAS5825M Register Definitions
#define TAS5825M_REG_PAGE         0x00  // Page Select
#define TAS5825M_REG_BOOK         0x7F  // Book Select
#define TAS5825M_REG_DEVICE_CTRL  0x03  // Device Control
#define TAS5825M_REG_SIG_CH_CTRL  0x28  // Signal Channel Control
#define TAS5825M_REG_DIGITAL_VOL  0x4C  // Digital Volume Control
#define TAS5825M_REG_ANALOG_GAIN  0x54  // Analog Gain
#define TAS5825M_REG_SDOUT_SEL    0x30  // Serial Data Output Select
#define TAS5825M_REG_CLK_CFG      0x60  // Clock Configuration
#define TAS5825M_REG_DSP_MISC     0x62  // DSP Miscellaneous
#define TAS5825M_REG_AGAIN_L      0x53  // Analog Gain Left
#define TAS5825M_REG_AGAIN_R      0x54  // Analog Gain Right
#define TAS5825M_REG_FAULT_CLEAR  0x78  // Fault Clear

// Book/Page values
#define TAS5825M_BOOK_00    0x00
#define TAS5825M_PAGE_00    0x00

// Device Control Register Values
#define TAS5825M_CTRL_HIZ         0x02  // High-Z mode (standby)
#define TAS5825M_CTRL_PLAY        0x03  // Play mode
#define TAS5825M_CTRL_MUTE        0x11  // Mute

// Digital Volume Values (0x4C register)
#define TAS5825M_VOL_MUTE    0x00  // Mute (-100dB)
#define TAS5825M_VOL_0DB     0x30  // 0dB (48 decimal)
#define TAS5825M_VOL_MIN     0x00  // Minimum volume
#define TAS5825M_VOL_MAX     0xCF  // Maximum volume (+24dB, 207 decimal)

// Analog Gain Values for different supply voltages
// 12V supply: 0x10 (-8.0dB, 11.74 Vpeak)
// 15V supply: 0x0C (-5.05dB, 14.73 Vpeak)
// 20V supply: 0x07 (-3.05dB, 19.73 Vpeak)
// 24V supply: 0x05 (-2.05dB, 23.72 Vpeak)
#define TAS5825M_AGAIN_12V   0x10
#define TAS5825M_AGAIN_15V   0x0C
#define TAS5825M_AGAIN_20V   0x07
#define TAS5825M_AGAIN_24V   0x05

/**
 * @brief Supply voltage configuration for TAS5825M
 */
enum TAS5825M_SupplyVoltage {
    TAS5825M_12V = 0,
    TAS5825M_15V = 1,
    TAS5825M_20V = 2,
    TAS5825M_24V = 3
};

/**
 * @class TAS5825Codec
 * @brief TI TAS5825M audio codec driver implementing AudioCodec interface
 * 
 * This driver provides full control over the TAS5825M digital audio amplifier
 * including initialization, volume control, mute, and fault monitoring.
 */
class TAS5825Codec : public AudioCodec {
public:
    /**
     * @brief Construct a new TAS5825 Codec object
     */
    TAS5825Codec();

    /**
     * @brief Initialize the TAS5825M codec
     * @param wire I2C interface (Wire or Wire1)
     * @param sda I2C SDA pin
     * @param scl I2C SCL pin
     * @param sample_rate Sample rate in Hz (default: 44100)
     * @param supply_voltage Supply voltage configuration (default: 20V)
     * @return true if initialization successful
     */
    bool begin(TwoWire& wire, int sda, int scl, uint32_t sample_rate = 44100,
               TAS5825M_SupplyVoltage supply_voltage = TAS5825M_20V);

    // AudioCodec interface implementation
    bool begin(uint32_t sample_rate = 44100) override;
    void reset() override;
    void setVolume(float volume) override;
    void setMute(bool mute) override;
    bool isInitialized() const override { return initialized; }
    const char* getModelName() const override { return "TAS5825M"; }

    /**
     * @brief Set digital volume in dB
     * @param db Volume in dB (-100.0 to +24.0)
     */
    void setVolumeDB(float db);

    /**
     * @brief Clear fault status
     * @return true if successful
     */
    bool clearFaults();

    /**
     * @brief Dump all important registers via serial
     */
    void dumpRegisters() override;
    
#if AUDIO_DEBUG
    // Debug methods
    bool testCommunication() override;
    uint16_t readRegisterCache(uint8_t reg) const override;
    bool writeRegisterDebug(uint8_t reg, uint16_t value) override;
    void printStatus() override;
    void reinitialize(uint32_t sample_rate = 44100) override;
    void* getCommunicationInterface() override { return i2c; }
#endif // AUDIO_DEBUG

private:
    TwoWire* i2c;
    int sdaPin;
    int sclPin;
    uint32_t sampleRate;
    TAS5825M_SupplyVoltage supplyVoltage;
    bool initialized;
    bool muted;
    uint8_t currentVolume;  // Current volume register value (0-207)

    /**
     * @brief Write a single byte to TAS5825M register
     * @param reg Register address
     * @param value Value to write
     * @return true if successful
     */
    bool writeRegister(uint8_t reg, uint8_t value);

    /**
     * @brief Read a single byte from TAS5825M register
     * @param reg Register address
     * @param value Pointer to store read value
     * @return true if successful
     */
    bool readRegister(uint8_t reg, uint8_t* value);

    /**
     * @brief Select Book and Page
     * @param book Book number
     * @param page Page number
     * @return true if successful
     */
    bool selectBookPage(uint8_t book, uint8_t page);

    /**
     * @brief Initialize DSP coefficients (from PPC3 configuration)
     * @return true if successful
     */
    bool initDSPCoefficients();

    /**
     * @brief Configure analog gain based on supply voltage
     * @return true if successful
     */
    bool configureAnalogGain();

    /**
     * @brief Get analog gain value for current supply voltage
     * @return Analog gain register value
     */
    uint8_t getAnalogGainValue() const;
};

#endif // TAS5825_CODEC_H
