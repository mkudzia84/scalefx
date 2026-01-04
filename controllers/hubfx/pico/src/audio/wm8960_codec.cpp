/*
 * WM8960 Audio Codec Driver - Implementation
 * 
 * Based on WM8960 datasheet and Waveshare WM8960 Audio HAT
 */

#include "wm8960_codec.h"

WM8960Codec::WM8960Codec() : wire(nullptr), initialized(false) {
    // Clear register cache
    memset(regCache, 0, sizeof(regCache));
}

bool WM8960Codec::begin(TwoWire& wire_interface, uint8_t sda_pin, uint8_t scl_pin, uint32_t sample_rate) {
    Serial.println("[WM8960] Initializing codec...");
    
    wire = &wire_interface;
    
    // Initialize I2C
    wire->setSDA(sda_pin);
    wire->setSCL(scl_pin);
    wire->begin();
    wire->setClock(100000);  // 100kHz I2C
    
    delay(10);
    
    // Reset codec
    reset();
    delay(100);
    
    // Initialize subsystems in order
    initPower();
    initClock(sample_rate);
    initInterface();
    initDAC();
    initOutputs();
    
    // Set default volume
    setHeadphoneVolume(100);  // ~-17dB
    setSpeakerVolume(100);
    
    initialized = true;
    Serial.println("[WM8960] Initialization complete");
    
    return true;
}

// AudioCodec interface: simplified begin (for generic codec usage)
bool WM8960Codec::begin(uint32_t sample_rate) {
    // Default to Wire with standard pins if not already configured
    if (!initialized && wire == nullptr) {
        Serial.println("[WM8960] Error: Must call begin(Wire, sda, scl) first");
        return false;
    }
    return initialized;
}

void WM8960Codec::reset() {
    Serial.println("[WM8960] Resetting codec...");
    writeRegister(WM8960_REG_RESET, 0x00);
    delay(10);
    
    // Clear cache
    memset(regCache, 0, sizeof(regCache));
}

bool WM8960Codec::writeRegister(uint8_t reg, uint16_t value) {
    if (!wire) return false;
    
    // WM8960 protocol: send 2 bytes
    // Byte 1: [7-bit reg addr (bits 8-2)] [data bit 8]
    // Byte 2: [data bits 7-0]
    uint8_t byte1 = (reg << 1) | ((value >> 8) & 0x01);
    uint8_t byte2 = value & 0xFF;
    
    wire->beginTransmission(WM8960_I2C_ADDR);
    wire->write(byte1);
    wire->write(byte2);
    uint8_t result = wire->endTransmission();
    
    if (result == 0) {
        // Update cache
        if (reg < 56) {
            regCache[reg] = value;
        }
        return true;
    } else {
        Serial.printf("[WM8960] I2C write failed: reg=0x%02X, error=%d\n", reg, result);
        return false;
    }
}

void WM8960Codec::updateBits(uint8_t reg, uint16_t mask, uint16_t value) {
    if (reg >= 56) return;
    
    uint16_t newVal = (regCache[reg] & ~mask) | (value & mask);
    writeRegister(reg, newVal);
}

void WM8960Codec::initPower() {
    Serial.println("[WM8960] Configuring power management...");
    
    // Power Management 1: Enable VMID, VREF, AINL, AINR
    // Bit 7: VMIDSEL[1] = 0 (50k divider)
    // Bit 6: VMIDSEL[0] = 1 (50k divider)  
    // Bit 5: VREF = 1
    // Bit 4: AINL = 1
    // Bit 3: AINR = 1
    writeRegister(WM8960_REG_POWER1, 0x00FE);  // All on except MICB
    
    // Power Management 2: Enable DAC, outputs
    // Bit 8: DACL = 1
    // Bit 7: DACR = 1
    // Bit 6: LOUT1 = 1 (headphone left)
    // Bit 5: ROUT1 = 1 (headphone right)
    // Bit 4: SPKL = 1 (speaker left)
    // Bit 3: SPKR = 1 (speaker right)
    writeRegister(WM8960_REG_POWER2, 0x01FF);  // All outputs on
    
    // Power Management 3: Enable L/R output mixers
    // Bit 3: LOMIX = 1
    // Bit 2: ROMIX = 1
    writeRegister(WM8960_REG_POWER3, 0x000C);
    
    delay(100);  // Wait for VMID charge pump
}

void WM8960Codec::initClock(uint32_t sample_rate) {
    Serial.printf("[WM8960] Configuring clocks for %lu Hz...\n", sample_rate);
    
    // Clock 1: Use MCLK as system clock source (assuming external MCLK)
    // For I2S slave mode, we rely on BCLK/LRCLK from master (Pico I2S)
    // Bit 0: CLKSEL = 0 (MCLK)
    writeRegister(WM8960_REG_CLOCK1, 0x0000);
    
    // Additional Control 1: Set GPIO1 as ADCLRC output (optional)
    writeRegister(WM8960_REG_ADDCTL1, 0x00C0);
    
    // For 44.1kHz with typical 256*Fs MCLK (11.2896 MHz):
    // No PLL needed if MCLK matches
    // If no MCLK available, we'd need to enable internal PLL
    
    // Additional Control 4: Enable DAC oversampling
    writeRegister(WM8960_REG_ADDCTL4, 0x0000);
}

void WM8960Codec::initInterface() {
    Serial.println("[WM8960] Configuring I2S interface...");
    
    // Audio Interface 1
    // Bits 7-6: Format = 10 (I2S)
    // Bits 3-2: Word Length = 00 (16-bit)
    // Bit 0: DLRSWAP = 0 (normal L/R)
    uint16_t iface1 = (WM8960_FMT_I2S << 6) | (WM8960_WL_16BIT << 2);
    writeRegister(WM8960_REG_IFACE1, iface1);
    
    // Audio Interface 2: Slave mode
    // Bit 6: ALRCGPIO = 0 (ADCLRC is input)
    writeRegister(WM8960_REG_IFACE2, 0x0000);
}

void WM8960Codec::initDAC() {
    Serial.println("[WM8960] Configuring DAC...");
    
    // DAC Control: Enable DAC, no mute
    // Bit 3: DACMU = 0 (no mute)
    // Bit 2: DEEMPH = 0 (no de-emphasis)
    writeRegister(WM8960_REG_DACCTL1, 0x0000);
    
    // Set DAC volume to 0dB (default)
    // Volume range: 0x00 (-127dB) to 0xFF (0dB)
    writeRegister(WM8960_REG_LDAC, 0x01FF);  // 0dB, update both
    writeRegister(WM8960_REG_RDAC, 0x01FF);  // 0dB
}

void WM8960Codec::initOutputs() {
    Serial.println("[WM8960] Configuring outputs...");
    
    // Left Output Mixer: DAC to left output
    // Bit 8: LD2LO = 1 (connect Left DAC to Left Output Mixer)
    writeRegister(WM8960_REG_LOUTMIX, 0x0100);
    
    // Right Output Mixer: DAC to right output  
    // Bit 8: RD2RO = 1 (connect Right DAC to Right Output Mixer)
    writeRegister(WM8960_REG_ROUTMIX, 0x0100);
    
    // Headphone outputs (LOUT1/ROUT1)
    // Volume: 0x00 = mute, 0x30-0x7F = -73dB to +6dB
    writeRegister(WM8960_REG_LOUT1, 0x0179);  // 0dB, zero-cross, update
    writeRegister(WM8960_REG_ROUT1, 0x0179);  // 0dB, zero-cross
    
    // Speaker outputs (LOUT2/ROUT2) - Class D
    writeRegister(WM8960_REG_LOUT2, 0x0179);  // 0dB
    writeRegister(WM8960_REG_ROUT2, 0x0179);  // 0dB
    
    // Class D Control 1: Enable speakers
    // Bit 7: SPK_OP_EN = 1 (enable speaker outputs)
    // Bits 6-5: 11 = 1.6x gain
    writeRegister(WM8960_REG_CLASSD1, 0x00F7);
    
    // Class D Control 3
    // Bit 3: DCGAIN = 0 (3.6V)
    // Bit 2: ACGAIN = 0 (4.4V)
    writeRegister(WM8960_REG_CLASSD3, 0x0000);
    
    // Anti-pop: Smooth power-up
    writeRegister(WM8960_REG_ANTICLICK, 0x0000);
    writeRegister(WM8960_REG_ANTIPOP2, 0x0000);
}

void WM8960Codec::enableSpeakers(bool enable) {
    if (enable) {
        // Enable speaker outputs in power management
        updateBits(WM8960_REG_POWER2, 0x0018, 0x0018);  // SPKL | SPKR
        // Enable Class D
        updateBits(WM8960_REG_CLASSD1, 0x0080, 0x0080);  // SPK_OP_EN
    } else {
        // Disable Class D
        updateBits(WM8960_REG_CLASSD1, 0x0080, 0x0000);
        // Disable speaker power
        updateBits(WM8960_REG_POWER2, 0x0018, 0x0000);
    }
}

void WM8960Codec::enableHeadphones(bool enable) {
    if (enable) {
        // Enable headphone outputs
        updateBits(WM8960_REG_POWER2, 0x0060, 0x0060);  // LOUT1 | ROUT1
    } else {
        // Disable headphone outputs
        updateBits(WM8960_REG_POWER2, 0x0060, 0x0000);
    }
}

void WM8960Codec::setVolume(float volume) {
    // Clamp to 0.0-1.0
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    
    // Map to 0-127 range
    uint8_t vol = (uint8_t)(volume * 127.0f);
    
    setHeadphoneVolume(vol);
    setSpeakerVolume(vol);
}

void WM8960Codec::setHeadphoneVolume(uint8_t volume) {
    // Volume range: 0x30 (mute) to 0x7F (+6dB)
    // Map input 0-127 to this range
    if (volume > 127) volume = 127;
    
    uint16_t vol = 0x30 + ((volume * 0x4F) / 127);  // Map to 0x30-0x7F
    
    // Set with zero-cross and update bits
    writeRegister(WM8960_REG_LOUT1, 0x0100 | vol);  // Bit 8 = update both
    writeRegister(WM8960_REG_ROUT1, 0x0100 | vol);
}

void WM8960Codec::setSpeakerVolume(uint8_t volume) {
    if (volume > 127) volume = 127;
    
    uint16_t vol = 0x30 + ((volume * 0x4F) / 127);
    
    writeRegister(WM8960_REG_LOUT2, 0x0100 | vol);
    writeRegister(WM8960_REG_ROUT2, 0x0100 | vol);
}

void WM8960Codec::setMute(bool mute) {
    if (mute) {
        // Set DAC soft mute
        updateBits(WM8960_REG_DACCTL1, 0x0008, 0x0008);
    } else {
        // Clear DAC soft mute
        updateBits(WM8960_REG_DACCTL1, 0x0008, 0x0000);
    }
}

void WM8960Codec::dumpRegisters() {
    Serial.println("=== WM8960 Register Dump ===");
    for (uint8_t reg = 0; reg < 56; reg++) {
        Serial.printf("R%02d (0x%02X): 0x%04X\n", reg, reg, regCache[reg]);
    }
    Serial.println("============================");
}
