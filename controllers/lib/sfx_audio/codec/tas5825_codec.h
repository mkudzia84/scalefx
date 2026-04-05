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

#include <Arduino.h>
#include <Wire.h>
#include "../audio/audio_config.h"

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
#define TAS5825M_CTRL_DEEP_SLEEP  0x00  // Deep Sleep (PLL off, no clocks needed)
#define TAS5825M_CTRL_HIZ         0x02  // High-Z mode (PLL active, needs I2S clocks)
#define TAS5825M_CTRL_PLAY        0x03  // Play mode
#define TAS5825M_CTRL_MUTE        0x11  // Mute

// Clock registers (TAS5825M datasheet §7.5.2)
#define TAS5825M_REG_CLK_SRC      0x33  // Clock source (0x00 = auto)
#define TAS5825M_REG_FS_RATE      0x34  // FS rate detect (0x00 = auto)
#define TAS5825M_REG_FS_MON       0x37  // Sample rate monitor (read-only)

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
class TAS5825Codec {
public:
    static TAS5825Codec& instance() {
        static TAS5825Codec inst;
        return inst;
    }

    // Delete copy/move (singleton)
    TAS5825Codec(const TAS5825Codec&) = delete;
    TAS5825Codec& operator=(const TAS5825Codec&) = delete;
    TAS5825Codec(TAS5825Codec&&) = delete;
    TAS5825Codec& operator=(TAS5825Codec&&) = delete;

    /**
     * @brief Phase 1: Initialize I2C, probe codec, reset, enter Deep Sleep.
     *
     * Puts the codec in Deep Sleep (PLL off) — safe to call before I2S
     * clocks are running. Does NOT enter PLAY mode.
     * Call activate() after I2S clocks are running to transition to PLAY.
     *
     * @param wire I2C interface (Wire or Wire1)
     * @param sda I2C SDA pin
     * @param scl I2C SCL pin
     * @param sample_rate Sample rate in Hz (default: AUDIO_SAMPLE_RATE)
     * @param supply_voltage Supply voltage configuration (default: 20V)
     * @return true if I2C probe and reset successful
     */
    bool begin(TwoWire& wire, int sda, int scl, uint32_t sample_rate = AUDIO_SAMPLE_RATE,
               TAS5825M_SupplyVoltage supply_voltage = TAS5825M_20V);

    /**
     * @brief Phase 2: Configure registers and transition to PLAY mode.
     *
     * MUST be called AFTER I2S BCLK/LRCLK are running on the GPIO pins.
     * Configures analog gain, clock registers, DSP coefficients, then
     * transitions Deep Sleep → HIZ (PLL locks to BCK) → PLAY.
     *
     * @return true if codec entered PLAY state successfully
     */
    bool activate();

    // Codec interface implementation
    bool begin(uint32_t sample_rate = AUDIO_SAMPLE_RATE);
    void reset();
    void setVolume(float volume);
    void setMute(bool mute);
    bool isInitialized() const { return initialized; }
    const char* getModelName() const { return "TAS5825M"; }

    /**
     * @brief Set digital volume in dB
     * @param db Volume in dB (-100.0 to +24.0)
     */
    void setVolumeDB(float db);

    /**
     * @brief Change supply voltage configuration at runtime.
     *
     * Reconfigures the analog gain register to match the new supply voltage.
     * Safe to call while playing — briefly enters Hi-Z, writes gain, returns to play.
     *
     * @param voltage New supply voltage configuration
     * @return true if successful
     */
    bool setSupplyVoltage(TAS5825M_SupplyVoltage voltage);

    /**
     * @brief Clear fault status
     * @return true if successful
     */
    bool clearFaults();

    /**
     * @brief Dump all important registers via serial
     */
    void dumpRegisters();

    // --- Status Queries (for CODEC_STATUS protocol) ---
    uint8_t getCodecType() const { return 1; }  // 1 = TAS5825M
    bool    getMuted() const { return muted; }
    uint8_t getVolumeRegister() const { return currentVolume; }
    TAS5825M_SupplyVoltage getSupplyVoltage() const { return supplyVoltage; }
    int     getSdaPin() const { return sdaPin; }
    int     getSclPin() const { return sclPin; }
    uint8_t getDeviceControlRegister();
    uint8_t getFaultRegister();
    bool    testI2CConnection();

    /**
     * @brief Parse a supply voltage string to the enum value.
     * @param str  "12v", "15v", "20v", or "24v" (case-insensitive)
     * @param out  Receives the parsed enum value
     * @return true if valid, false if unrecognized
     */
    static bool parseSupplyVoltage(const char* str, TAS5825M_SupplyVoltage& out) {
        if (!str) return false;
        if (strcmp(str, "12v") == 0 || strcmp(str, "12V") == 0) { out = TAS5825M_12V; return true; }
        if (strcmp(str, "15v") == 0 || strcmp(str, "15V") == 0) { out = TAS5825M_15V; return true; }
        if (strcmp(str, "20v") == 0 || strcmp(str, "20V") == 0) { out = TAS5825M_20V; return true; }
        if (strcmp(str, "24v") == 0 || strcmp(str, "24V") == 0) { out = TAS5825M_24V; return true; }
        return false;
    }

    /**
     * @brief Convert supply voltage enum to a display string.
     */
    static const char* supplyVoltageStr(TAS5825M_SupplyVoltage v) {
        switch (v) {
            case TAS5825M_12V: return "12v";
            case TAS5825M_15V: return "15v";
            case TAS5825M_20V: return "20v";
            case TAS5825M_24V: return "24v";
            default:           return "??v";
        }
    }
    
#if AUDIO_DEBUG
    // Debug methods
    bool testCommunication();
    uint16_t readRegisterCache(uint8_t reg) const;
    bool writeRegisterDebug(uint8_t reg, uint16_t value);
    void printStatus();
    void reinitialize(uint32_t sample_rate = 0);
    void* getCommunicationInterface() { return i2c; }
#endif // AUDIO_DEBUG

private:
    TAS5825Codec();  // Private constructor (singleton)

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
