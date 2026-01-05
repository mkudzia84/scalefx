/*
 * WM8960 Audio Codec Driver - Implementation
 * 
 * Based on WM8960 datasheet and Waveshare WM8960 Audio HAT
 * 
 * I2S TIMING CONFIGURATION:
 * ========================
 * Sample Rate: 44.1 kHz (AUDIO_SAMPLE_RATE)
 * Bit Depth:   32 bits per channel (16-bit data in 32-bit frame)
 * Channels:    2 (stereo)
 * 
 * Clock Frequencies:
 *   LRCLK (Frame Clock):  44,100 Hz         (= sample rate)
 *   BCLK (Bit Clock):     2,822,400 Hz      (= 44.1k × 32 × 2)
 *   SYSCLK (Internal):    11,289,600 Hz     (= 44.1k × 256, from PLL)
 * 
 * PLL Configuration:
 *   Input:  BCLK (2.8224 MHz) - Pico I2S output
 *   Output: SYSCLK (11.2896 MHz) - 4× multiplication
 *   Mode:   Fractional (SDM), K = 0x0C93E9
 * 
 * WIRING REQUIREMENTS:
 * ===================
 * Signal integrity is CRITICAL at 2.8 MHz BCLK frequency:
 * 
 *   • Wire Length:    Keep ALL I2S wires < 6 inches (< 150mm)
 *   • Wire Matching:  BCLK, LRCLK, DATA within ±1 inch of each other
 *   • Ground Return:  Run ground wire parallel to each signal (twisted pair)
 *   • Wire Gauge:     22-26 AWG solid core (lower capacitance)
 *   • Separation:     Keep away from power/servo wires (EMI)
 *   • Clock Skew:     At 2.8 MHz, each bit = 355ns - length matters!
 * 
 *   Long wires cause:
 *     - Increased capacitance (~30-100 pF/foot) → slow edges
 *     - Clock/data skew → bit errors → audio clicks/pops
 *     - Signal reflections → jitter
 * 
 * I2C CONFIGURATION:
 * ==================
 * Speed: 50 kHz (reduced from standard 100 kHz for stability)
 * Reason: HAT capacitance + wire length causes timeouts at 100 kHz
 */

#include "wm8960_codec.h"
#include "audio_config.h"

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
    wire->setClock(WM8960_I2C_SPEED);
    
    delay(10);
    
#if AUDIO_DEBUG_TIMING
    Serial.printf("[WM8960] I2C speed: %lu Hz\n", (unsigned long)WM8960_I2C_SPEED);
    Serial.printf("[WM8960] I2S BCLK: %lu Hz\n", (unsigned long)I2S_BCLK_FREQ);
    Serial.printf("[WM8960] I2S LRCLK: %lu Hz\n", (unsigned long)I2S_LRCLK_FREQ);
#endif
    
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
    
    // CRITICAL: No MCLK available - must use PLL from BCLK
    // The Pico I2S only generates BCLK and LRCLK, not MCLK
    
    // I2S Timing Chain (for 44.1 kHz):
    //   LRCLK = 44,100 Hz (sample rate)
    //   BCLK  = 44,100 × 32 bits × 2 channels = 2,822,400 Hz
    //   SYSCLK = 44,100 × 256 (from PLL) = 11,289,600 Hz
    //
    // PLL multiplies BCLK by 4× to generate SYSCLK for internal DAC/ADC
    
    // PLL Control 1: Enable PLL, use BCLK as input
    // Bit 5: SDM = 1 (fractional mode)
    // Bits 4-0: Prescale divider (from audio_config.h)
    writeRegister(WM8960_REG_PLL1, 0x0020 | WM8960_PLL_PRESCALE);  // SDM mode + prescale
    
    // PLL Control 2-4: K value (fractional multiplier from audio_config.h)
    // K value is calculated for the configured sample rate
    writeRegister(WM8960_REG_PLL2, WM8960_PLL_K_HIGH);  // K[23:16]
    writeRegister(WM8960_REG_PLL3, WM8960_PLL_K_MID);   // K[15:8]
    writeRegister(WM8960_REG_PLL4, WM8960_PLL_K_LOW);   // K[7:0]
    
#if AUDIO_DEBUG_TIMING
    Serial.printf("[WM8960] PLL K value: 0x%06lX\n", (unsigned long)WM8960_PLL_K_VALUE);
#endif
    
    // PLL Control 1: Enable PLL (set bit 6)
    writeRegister(WM8960_REG_PLL1, 0x0060 | WM8960_PLL_PRESCALE);  // Enable PLL + config
    
    // Clock 1: Use PLL output as system clock (SYSCLK)
    // Bit 0: CLKSEL = 1 (PLL output)
    writeRegister(WM8960_REG_CLOCK1, 0x0001);
    
    delay(10);  // Wait for PLL to lock
    
    // Additional Control 1: Set GPIO1 as ADCLRC output (optional)
    writeRegister(WM8960_REG_ADDCTL1, 0x00C0);
    
    // Additional Control 4: Enable DAC oversampling
    writeRegister(WM8960_REG_ADDCTL4, 0x0000);
    
    Serial.println("[WM8960] PLL configured from BCLK (no MCLK needed)");
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
        Serial.printf("R%02d (0x%02X): 0x%04X", reg, reg, regCache[reg]);
        
        // Add register names for key registers
        switch(reg) {
            case WM8960_REG_LINVOL: Serial.print(" - Left Input Volume"); break;
            case WM8960_REG_RINVOL: Serial.print(" - Right Input Volume"); break;
            case WM8960_REG_LOUT1: Serial.print(" - LOUT1 (Headphone L)"); break;
            case WM8960_REG_ROUT1: Serial.print(" - ROUT1 (Headphone R)"); break;
            case WM8960_REG_CLOCK1: Serial.print(" - Clock 1"); break;
            case WM8960_REG_DACCTL1: Serial.print(" - DAC Control 1"); break;
            case WM8960_REG_IFACE1: Serial.print(" - Audio Interface 1"); break;
            case WM8960_REG_POWER1: Serial.print(" - Power Mgmt 1"); break;
            case WM8960_REG_POWER2: Serial.print(" - Power Mgmt 2"); break;
            case WM8960_REG_POWER3: Serial.print(" - Power Mgmt 3"); break;
            case WM8960_REG_LOUT2: Serial.print(" - LOUT2 (Speaker L)"); break;
            case WM8960_REG_ROUT2: Serial.print(" - ROUT2 (Speaker R)"); break;
        }
        Serial.println();
    }
    Serial.println("============================");
}

// ============================================================================
//  DEBUG METHODS (AudioCodec Interface)
// ============================================================================

bool WM8960Codec::testCommunication() {
    if (!wire) {
        Serial.println("[WM8960] ERROR: I2C not initialized");
        return false;
    }
    
    Serial.println("[WM8960] Testing I2C communication...");
    Serial.printf("[WM8960] I2C Address: 0x%02X\n", WM8960_I2C_ADDR);
    
    // Try to write to a safe register (reset register)
    wire->beginTransmission(WM8960_I2C_ADDR);
    wire->write(0x0F << 1);  // Reset register
    wire->write(0x00);
    uint8_t result = wire->endTransmission();
    
    Serial.printf("[WM8960] I2C transmission result: %d\n", result);
    
    switch(result) {
        case 0:
            Serial.println("[WM8960] SUCCESS: Device responded");
            return true;
        case 1:
            Serial.println("[WM8960] ERROR: Data too long");
            break;
        case 2:
            Serial.println("[WM8960] ERROR: NACK on address (device not found)");
            break;
        case 3:
            Serial.println("[WM8960] ERROR: NACK on data");
            break;
        case 4:
            Serial.println("[WM8960] ERROR: Other I2C error (bus may be stuck)");
            Serial.println("[WM8960] Try: codec recover");
            break;
        case 5:
            Serial.println("[WM8960] ERROR: Timeout");
            break;
        default:
            Serial.printf("[WM8960] ERROR: Unknown error code %d\n", result);
            break;
    }
    return false;
}

uint16_t WM8960Codec::readRegisterCache(uint8_t reg) const {
    if (reg >= 56) return 0xFFFF;
    return regCache[reg];
}

bool WM8960Codec::writeRegisterDebug(uint8_t reg, uint16_t value) {
    Serial.printf("[WM8960] Writing R%d (0x%02X) = 0x%04X\n", reg, reg, value);
    bool result = writeRegister(reg, value);
    if (result) {
        Serial.println("[WM8960] Write SUCCESS");
    } else {
        Serial.println("[WM8960] Write FAILED");
    }
    return result;
}

void WM8960Codec::printStatus() {
    Serial.println("\n=== WM8960 Codec Status ===");
    Serial.printf("Initialized: %s\n", initialized ? "YES" : "NO");
    Serial.printf("I2C Interface: %s\n", wire ? "Connected" : "Not Connected");
    
    if (wire) {
        Serial.println("\nI2C Test:");
        testCommunication();
    }
    
    Serial.println("\nKey Registers:");
    Serial.printf("  POWER1 (0x19): 0x%04X\n", regCache[WM8960_REG_POWER1]);
    Serial.printf("  POWER2 (0x1A): 0x%04X\n", regCache[WM8960_REG_POWER2]);
    Serial.printf("  POWER3 (0x2F): 0x%04X\n", regCache[WM8960_REG_POWER3]);
    Serial.printf("  IFACE1 (0x07): 0x%04X\n", regCache[WM8960_REG_IFACE1]);
    Serial.printf("  CLOCK1 (0x04): 0x%04X\n", regCache[WM8960_REG_CLOCK1]);
    Serial.printf("  DACCTL1 (0x05): 0x%04X\n", regCache[WM8960_REG_DACCTL1]);
    Serial.println("===========================\n");
}

void WM8960Codec::reinitialize(uint32_t sample_rate) {
    Serial.println("[WM8960] Reinitializing codec...");
    
    if (!wire) {
        Serial.println("[WM8960] ERROR: I2C not configured. Use begin(Wire, sda, scl) first.");
        return;
    }
    
    initialized = false;
    
    // Reset codec
    reset();
    delay(100);
    
    // Re-initialize subsystems
    initPower();
    initClock(sample_rate);
    initInterface();
    initDAC();
    initOutputs();
    
    // Set default volume
    setHeadphoneVolume(100);
    setSpeakerVolume(100);
    
    initialized = true;
    Serial.println("[WM8960] Reinitialization complete");
}

bool WM8960Codec::recoverI2C() {
    if (!wire) {
        Serial.println("[WM8960] ERROR: I2C not configured");
        return false;
    }
    
    Serial.println("[WM8960] Attempting I2C bus recovery...");
    
    // Method 1: Reset the I2C peripheral
    Serial.println("[WM8960] Step 1: Resetting I2C peripheral");
    wire->end();
    delay(100);
    wire->begin();
    wire->setClock(WM8960_I2C_SPEED);
    delay(100);
    
    // Method 2: Generate clock pulses to clear stuck bus
    Serial.println("[WM8960] Step 2: Generating I2C clock pulses");
    // This is done automatically by the Wire library on restart
    
    // Method 3: Test communication
    Serial.println("[WM8960] Step 3: Testing communication");
    wire->beginTransmission(WM8960_I2C_ADDR);
    uint8_t result = wire->endTransmission();
    
    if (result == 0) {
        Serial.println("[WM8960] Recovery SUCCESS: Device responding");
        Serial.println("[WM8960] Hint: Run 'codec reinit' to restore codec state");
        return true;
    } else {
        Serial.printf("[WM8960] Recovery FAILED: Error code %d\n", result);
        Serial.println("[WM8960] Possible causes:");
        Serial.println("  - I2C wiring disconnected (check SDA/SCL)");
        Serial.println("  - Codec power issue (check 3.3V)");
        Serial.println("  - Hardware failure");
        return false;
    }
}
