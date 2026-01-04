/**
 * @file tas5825_codec.cpp
 * @brief TI TAS5825M Digital Audio Amplifier Driver Implementation
 * 
 * Based on initialization sequences from the bassowl-hat project:
 * https://github.com/Darmur/bassowl-hat
 */

#include "tas5825_codec.h"
#include <Arduino.h>

TAS5825Codec::TAS5825Codec()
    : i2c(nullptr)
    , sdaPin(-1)
    , sclPin(-1)
    , sampleRate(44100)
    , supplyVoltage(TAS5825M_20V)
    , initialized(false)
    , muted(false)
    , currentVolume(TAS5825M_VOL_0DB)
{
}

bool TAS5825Codec::begin(TwoWire& wire, int sda, int scl, uint32_t sample_rate,
                          TAS5825M_SupplyVoltage supply_voltage)
{
    i2c = &wire;
    sdaPin = sda;
    sclPin = scl;
    sampleRate = sample_rate;
    supplyVoltage = supply_voltage;

    // Initialize I2C
    i2c->setSDA(sdaPin);
    i2c->setSCL(sclPin);
    i2c->begin();
    i2c->setClock(100000);  // 100kHz I2C

    Serial.println("[TAS5825M] Initializing codec...");

    // Initial reset sequence
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_HIZ);  // Enter HIZ mode
    writeRegister(0x01, 0x11);  // Reset
    delay(5);

    // Configure analog gain based on supply voltage
    if (!configureAnalogGain()) {
        Serial.println("[TAS5825M] Failed to configure analog gain");
        return false;
    }

    // Start device
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_HIZ);
    delay(5);

    // Initialize DSP coefficients
    if (!initDSPCoefficients()) {
        Serial.println("[TAS5825M] Failed to initialize DSP coefficients");
        return false;
    }

    // Register tuning (from bassowl-hat)
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(TAS5825M_REG_SDOUT_SEL, 0x00);  // SDOUT is the DSP output
    writeRegister(TAS5825M_REG_CLK_CFG, 0x02);    // Clock configuration
    writeRegister(TAS5825M_REG_DSP_MISC, 0x09);   // DSP miscellaneous
    writeRegister(TAS5825M_REG_DIGITAL_VOL, TAS5825M_VOL_0DB);  // 0dB volume
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_PLAY);  // Play mode

    // Clear faults
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(TAS5825M_REG_FAULT_CLEAR, 0x80);

    initialized = true;
    Serial.printf("[TAS5825M] Initialized successfully (%.1fkHz, %dV supply)\n",
                  sampleRate / 1000.0f,
                  supplyVoltage == TAS5825M_12V ? 12 :
                  supplyVoltage == TAS5825M_15V ? 15 :
                  supplyVoltage == TAS5825M_20V ? 20 : 24);

    return true;
}

bool TAS5825Codec::begin(uint32_t sample_rate)
{
    Serial.println("[TAS5825M] Error: Must call begin(Wire, sda, scl) with I2C parameters");
    return false;
}

void TAS5825Codec::reset()
{
    if (!initialized) return;

    Serial.println("[TAS5825M] Resetting codec...");
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_HIZ);
    writeRegister(0x01, 0x11);  // Reset
    delay(50);

    // Re-initialize
    initialized = false;
    begin(*i2c, sdaPin, sclPin, sampleRate, supplyVoltage);
}

void TAS5825Codec::setVolume(float volume)
{
    if (!initialized) return;

    // Convert 0.0-1.0 to register value (0x00-0xCF)
    // 0x30 (48) = 0dB, 0xCF (207) = +24dB, 0x00 = mute
    // Linear mapping: 0.0 -> 0x00, 1.0 -> 0x30 (0dB)
    currentVolume = (uint8_t)(volume * TAS5825M_VOL_0DB);
    
    if (currentVolume > TAS5825M_VOL_0DB) {
        currentVolume = TAS5825M_VOL_0DB;  // Limit to 0dB
    }

    if (!muted) {
        selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
        writeRegister(TAS5825M_REG_DIGITAL_VOL, currentVolume);
        Serial.printf("[TAS5825M] Volume set to %.1f%% (0x%02X)\n",
                      volume * 100.0f, currentVolume);
    }
}

void TAS5825Codec::setVolumeDB(float db)
{
    if (!initialized) return;

    // Convert dB to register value
    // Register formula: Vol_dB = (Reg_Value - 48) * 0.5
    // Reg_Value = (Vol_dB / 0.5) + 48
    // Range: -100dB (0x00) to +24dB (0xCF)
    
    if (db < -100.0f) db = -100.0f;
    if (db > 24.0f) db = 24.0f;

    currentVolume = (uint8_t)((db / 0.5f) + 48.0f);
    
    if (currentVolume < TAS5825M_VOL_MIN) currentVolume = TAS5825M_VOL_MIN;
    if (currentVolume > TAS5825M_VOL_MAX) currentVolume = TAS5825M_VOL_MAX;

    if (!muted) {
        selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
        writeRegister(TAS5825M_REG_DIGITAL_VOL, currentVolume);
        Serial.printf("[TAS5825M] Volume set to %.1f dB (0x%02X)\n", db, currentVolume);
    }
}

void TAS5825Codec::setMute(bool mute)
{
    if (!initialized) return;

    muted = mute;
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);

    if (mute) {
        writeRegister(TAS5825M_REG_DIGITAL_VOL, TAS5825M_VOL_MUTE);
        Serial.println("[TAS5825M] Muted");
    } else {
        writeRegister(TAS5825M_REG_DIGITAL_VOL, currentVolume);
        Serial.println("[TAS5825M] Unmuted");
    }
}

bool TAS5825Codec::clearFaults()
{
    if (!initialized) return false;

    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    bool success = writeRegister(TAS5825M_REG_FAULT_CLEAR, 0x80);
    
    if (success) {
        Serial.println("[TAS5825M] Faults cleared");
    }
    
    return success;
}

void TAS5825Codec::dumpRegisters()
{
    if (!initialized) {
        Serial.println("[TAS5825M] Not initialized");
        return;
    }

    Serial.println("\n[TAS5825M] Register Dump:");
    Serial.println("---------------------------");

    uint8_t value;
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);

    // Read important registers
    const struct {
        uint8_t reg;
        const char* name;
    } registers[] = {
        {0x00, "PAGE"},
        {0x01, "RESET"},
        {0x03, "DEVICE_CTRL"},
        {0x30, "SDOUT_SEL"},
        {0x4C, "DIGITAL_VOL"},
        {0x53, "ANALOG_GAIN"},
        {0x54, "ANALOG_GAIN_R"},
        {0x60, "CLK_CFG"},
        {0x62, "DSP_MISC"},
        {0x78, "FAULT_CLEAR"},
        {0x7F, "BOOK"}
    };

    for (const auto& reg : registers) {
        if (readRegister(reg.reg, &value)) {
            Serial.printf("  0x%02X %-15s: 0x%02X\n", reg.reg, reg.name, value);
        }
    }

    Serial.println("---------------------------\n");
}

bool TAS5825Codec::writeRegister(uint8_t reg, uint8_t value)
{
    if (!i2c) return false;

    i2c->beginTransmission(TAS5825M_I2C_ADDR);
    i2c->write(reg);
    i2c->write(value);
    uint8_t error = i2c->endTransmission();

    if (error != 0) {
        Serial.printf("[TAS5825M] I2C write error %d (reg 0x%02X)\n", error, reg);
        return false;
    }

    return true;
}

bool TAS5825Codec::readRegister(uint8_t reg, uint8_t* value)
{
    if (!i2c || !value) return false;

    i2c->beginTransmission(TAS5825M_I2C_ADDR);
    i2c->write(reg);
    uint8_t error = i2c->endTransmission(false);  // Repeated start

    if (error != 0) {
        Serial.printf("[TAS5825M] I2C write error %d during read\n", error);
        return false;
    }

    if (i2c->requestFrom(TAS5825M_I2C_ADDR, (uint8_t)1) != 1) {
        Serial.println("[TAS5825M] I2C read failed");
        return false;
    }

    *value = i2c->read();
    return true;
}

bool TAS5825Codec::selectBookPage(uint8_t book, uint8_t page)
{
    if (!writeRegister(TAS5825M_REG_PAGE, page)) return false;
    if (!writeRegister(TAS5825M_REG_BOOK, book)) return false;
    return true;
}

bool TAS5825Codec::configureAnalogGain()
{
    uint8_t gainValue = getAnalogGainValue();

    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(0x46, 0x11);  // Analog control register
    writeRegister(0x02, 0x00);  // Mode control
    writeRegister(TAS5825M_REG_AGAIN_L, 0x01);  // Analog gain left enable
    writeRegister(TAS5825M_REG_AGAIN_R, gainValue);  // Analog gain right

    Serial.printf("[TAS5825M] Analog gain configured: 0x%02X\n", gainValue);
    return true;
}

uint8_t TAS5825Codec::getAnalogGainValue() const
{
    switch (supplyVoltage) {
        case TAS5825M_12V: return TAS5825M_AGAIN_12V;
        case TAS5825M_15V: return TAS5825M_AGAIN_15V;
        case TAS5825M_20V: return TAS5825M_AGAIN_20V;
        case TAS5825M_24V: return TAS5825M_AGAIN_24V;
        default: return TAS5825M_AGAIN_20V;
    }
}

bool TAS5825Codec::initDSPCoefficients()
{
    // This is a simplified initialization
    // For full DSP coefficient programming from PPC3, see bassowl-hat install scripts
    // The coefficients are specific to the audio tuning and can be thousands of writes
    
    Serial.println("[TAS5825M] Initializing DSP coefficients (basic)...");

    // Basic DSP initialization sequence
    selectBookPage(0x8C, 0x0B);
    
    // Write basic identity matrix coefficients (pass-through)
    // These allow audio to pass through without DSP processing
    uint8_t identity_coeffs[] = {
        0x00, 0x80, 0x00, 0x00,  // Channel 0: 1.0 coefficient
        0x00, 0x80, 0x00, 0x00   // Channel 1: 1.0 coefficient
    };

    for (int i = 0; i < sizeof(identity_coeffs); i++) {
        writeRegister(0x28 + i, identity_coeffs[i]);
    }

    // Additional pages for DSP configuration
    // Note: Full configuration requires loading PPC3-generated coefficients
    // See bassowl-hat scripts for complete initialization
    
    return true;
}
