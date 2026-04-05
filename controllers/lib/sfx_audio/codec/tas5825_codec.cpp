/**
 * @file tas5825_codec.cpp
 * @brief TI TAS5825M Digital Audio Amplifier Driver Implementation
 * 
 * Based on initialization sequences from the bassowl-hat project:
 * https://github.com/Darmur/bassowl-hat
 */

#if defined(SFX_HAS_AUDIO)

#include "tas5825_codec.h"
#include <Arduino.h>
#include "../audio/audio_log.h"
#include "platform/sfx_platform.h"

TAS5825Codec::TAS5825Codec()
    : i2c(nullptr)
    , sdaPin(-1)
    , sclPin(-1)
    , sampleRate(AUDIO_SAMPLE_RATE)
    , supplyVoltage(TAS5825M_20V)
    , initialized(false)
    , muted(false)
    , currentVolume(TAS5825M_VOL_0DB)
{
}

bool TAS5825Codec::begin(TwoWire& wire, int sda, int scl, uint32_t sample_rate,
                          TAS5825M_SupplyVoltage supply_voltage)
{
    // Only initialize the I2C bus on first call or if bus params changed.
    // Re-calling Wire.begin() on ESP32 tears down and reinstalls the I2C
    // driver (i2c_del_master_bus + i2c_new_master_bus), which can leave
    // the bus in a transient state and cause subsequent probes to timeout.
    bool needWireInit = (i2c != &wire || sdaPin != sda || sclPin != scl);

    i2c = &wire;
    sdaPin = sda;
    sclPin = scl;
    sampleRate = sample_rate;
    supplyVoltage = supply_voltage;

    if (needWireInit) {
        // Initialize I2C bus (first call, or bus params changed)
#if SFX_PLATFORM_PICO
        // Arduino-Pico: must set SDA/SCL pins before begin()
        i2c->setSDA(sdaPin);
        i2c->setSCL(sclPin);
        i2c->begin();
#elif SFX_PLATFORM_ESP32
        // Arduino-ESP32: pass SDA/SCL to begin()
        i2c->begin(sdaPin, sclPin);
#endif
    }
    i2c->setClock(100000);  // 100kHz I2C

    TAS5825_LOG("Initializing codec (SDA=%d, SCL=%d, addr=0x%02X)...", sdaPin, sclPin, TAS5825M_I2C_ADDR);

    // Probe I2C bus with retry — the bus may be transiently unavailable
    // after boot (e.g., slave holding SDA low from interrupted transaction,
    // or I2C peripheral not fully stabilized).
    static constexpr int PROBE_RETRIES = 3;
    static constexpr int PROBE_DELAY_MS = 500;
    uint8_t probeResult = 0;
    for (int attempt = 0; attempt < PROBE_RETRIES; attempt++) {
        i2c->beginTransmission(TAS5825M_I2C_ADDR);
        probeResult = i2c->endTransmission();
        if (probeResult == 0) break;
        if (attempt < PROBE_RETRIES - 1) {
            TAS5825_LOG("I2C probe attempt %d/%d failed (error %d), retrying in %dms...",
                        attempt + 1, PROBE_RETRIES, probeResult, PROBE_DELAY_MS);
            SFX_DELAY_MS(PROBE_DELAY_MS);
        }
    }
    if (probeResult != 0) {
        TAS5825_LOG("I2C probe FAILED after %d attempts (error %d) — device not found at 0x%02X. "
                    "Check wiring: SDA=GPIO%d, SCL=GPIO%d, pull-ups, PVDD power.",
                    PROBE_RETRIES, probeResult, TAS5825M_I2C_ADDR, sdaPin, sclPin);
        initialized = false;
        return false;
    }
    TAS5825_LOG("I2C probe OK — device ACK at 0x%02X", TAS5825M_I2C_ADDR);

    // Phase 1: Reset → Deep Sleep (PLL off, no I2S clocks needed)
    // The codec's I2C interface is always accessible when PDN is HIGH,
    // regardless of power state. Deep Sleep disables the PLL so the
    // codec doesn't fault from missing clocks during early init.
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(0x01, 0x11);  // Software reset
    SFX_DELAY_MS(5);
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_DEEP_SLEEP);
    SFX_DELAY_MS(5);

    initialized = true;  // I2C path works — codec is reachable and in Deep Sleep
    TAS5825_LOG("Phase 1 done: codec in Deep Sleep (%.1fkHz, %dV supply). "
                "Call activate() after I2S clocks are running.",
                  sampleRate / 1000.0f,
                  supplyVoltage == TAS5825M_12V ? 12 :
                  supplyVoltage == TAS5825M_15V ? 15 :
                  supplyVoltage == TAS5825M_20V ? 20 : 24);

    return true;
}

bool TAS5825Codec::activate()
{
    if (!initialized || !i2c) {
        TAS5825_LOG("activate() failed — begin() not called or I2C not available");
        return false;
    }

    TAS5825_LOG("Phase 2: configuring codec (I2S clocks should be running)...");

    // Configure analog gain based on supply voltage
    if (!configureAnalogGain()) {
        TAS5825_LOG("Failed to configure analog gain");
        return false;
    }

    // Set clock source registers (auto-detect)
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(TAS5825M_REG_CLK_SRC, 0x00);   // Auto clock source
    writeRegister(TAS5825M_REG_FS_RATE, 0x00);    // Auto FS rate detect

    // Initialize DSP coefficients (still in Deep Sleep — register writes work)
    if (!initDSPCoefficients()) {
        TAS5825_LOG("Failed to initialize DSP coefficients");
        return false;
    }

    // Register tuning (from bassowl-hat)
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(TAS5825M_REG_SDOUT_SEL, 0x00);  // SDOUT is the DSP output
    writeRegister(TAS5825M_REG_CLK_CFG, 0x02);    // Clock configuration
    writeRegister(TAS5825M_REG_DSP_MISC, 0x09);   // DSP miscellaneous
    writeRegister(TAS5825M_REG_DIGITAL_VOL, currentVolume);

    // Transition: Deep Sleep → HIZ (PLL locks to BCLK)
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_HIZ);
    SFX_DELAY_MS(5);

    // Check if PLL locked (FS_MON should be non-zero)
    uint8_t fsMon = 0;
    readRegister(TAS5825M_REG_FS_MON, &fsMon);
    TAS5825_LOG("After HIZ: FS_MON=0x%02X", fsMon);

    // Transition: HIZ → PLAY
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_PLAY);
    SFX_DELAY_MS(5);

    // Verify PLAY state
    uint8_t powerState = 0;
    readRegister(0x68, &powerState);  // POWER_STATE register
    readRegister(TAS5825M_REG_FS_MON, &fsMon);
    TAS5825_LOG("After PLAY: POWER_STATE=0x%02X, FS_MON=0x%02X", powerState, fsMon);

    if (fsMon == 0x00) {
        // Fallback: try Deep Sleep → PLAY directly
        TAS5825_LOG("PLL not locked via HIZ path — trying Deep Sleep → PLAY...");
        writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_DEEP_SLEEP);
        SFX_DELAY_MS(5);
        writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_PLAY);
        SFX_DELAY_MS(5);
        readRegister(0x68, &powerState);
        readRegister(TAS5825M_REG_FS_MON, &fsMon);
        TAS5825_LOG("Fallback: POWER_STATE=0x%02X, FS_MON=0x%02X", powerState, fsMon);
    }

    // Clear any faults from state transitions
    writeRegister(TAS5825M_REG_FAULT_CLEAR, 0x80);

    bool inPlay = (powerState == 0x03);
    if (inPlay) {
        TAS5825_LOG("Codec activated — PLAY state, FS_MON=0x%02X", fsMon);
    } else {
        TAS5825_LOG("WARNING: Codec did not reach PLAY (POWER_STATE=0x%02X, FS_MON=0x%02X). "
                    "Check I2S clocks on BCLK/LRCLK pins.", powerState, fsMon);
    }

    return inPlay;
}

bool TAS5825Codec::setSupplyVoltage(TAS5825M_SupplyVoltage voltage)
{
    if (!initialized) {
        TAS5825_LOG("setSupplyVoltage: not initialized");
        return false;
    }
    if (voltage == supplyVoltage) return true;  // No change needed

    TAS5825_LOG("Changing supply voltage: %dV → %dV",
                supplyVoltage == TAS5825M_12V ? 12 :
                supplyVoltage == TAS5825M_15V ? 15 :
                supplyVoltage == TAS5825M_20V ? 20 : 24,
                voltage == TAS5825M_12V ? 12 :
                voltage == TAS5825M_15V ? 15 :
                voltage == TAS5825M_20V ? 20 : 24);

    // Briefly enter Deep Sleep to safely change analog gain
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_DEEP_SLEEP);
    SFX_DELAY_MS(2);

    supplyVoltage = voltage;
    if (!configureAnalogGain()) {
        TAS5825_LOG("Failed to reconfigure analog gain");
        // Try to recover to play mode
        writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_HIZ);
        SFX_DELAY_MS(5);
        writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_PLAY);
        return false;
    }

    // Return to play mode: Deep Sleep → HIZ (PLL lock) → PLAY
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_HIZ);
    SFX_DELAY_MS(5);
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_PLAY);
    writeRegister(TAS5825M_REG_FAULT_CLEAR, 0x80);  // Clear any faults from transition

    TAS5825_LOG("Supply voltage reconfigured OK");
    return true;
}

bool TAS5825Codec::begin(uint32_t sample_rate)
{
    TAS5825_LOG("Error: Must call begin(Wire, sda, scl) with I2C parameters");
    return false;
}

void TAS5825Codec::reset()
{
    if (!initialized) return;

    TAS5825_LOG("Resetting codec...");
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    writeRegister(TAS5825M_REG_DEVICE_CTRL, TAS5825M_CTRL_DEEP_SLEEP);
    writeRegister(0x01, 0x11);  // Reset
    SFX_DELAY_MS(50);

    // Re-initialize Phase 1 (Deep Sleep) then Phase 2 (activate with clocks)
    initialized = false;
    if (begin(*i2c, sdaPin, sclPin, sampleRate, supplyVoltage)) {
        activate();  // I2S should still be running from before reset
    }
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
        TAS5825_LOG("Volume set to %.1f%% (0x%02X)",
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
        TAS5825_LOG("Volume set to %.1f dB (0x%02X)", db, currentVolume);
    }
}

void TAS5825Codec::setMute(bool mute)
{
    if (!initialized) return;

    muted = mute;
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);

    if (mute) {
        writeRegister(TAS5825M_REG_DIGITAL_VOL, TAS5825M_VOL_MUTE);
        TAS5825_LOG("Muted");
    } else {
        writeRegister(TAS5825M_REG_DIGITAL_VOL, currentVolume);
        TAS5825_LOG("Unmuted");
    }
}

bool TAS5825Codec::clearFaults()
{
    if (!initialized) return false;

    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    bool success = writeRegister(TAS5825M_REG_FAULT_CLEAR, 0x80);
    
    if (success) {
        TAS5825_LOG("Faults cleared");
    }
    
    return success;
}

void TAS5825Codec::dumpRegisters()
{
    if (!initialized) {
        TAS5825_LOG("Not initialized");
        return;
    }

    TAS5825_LOG("Register Dump:");
    TAS5825_LOG("---------------------------");

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
            TAS5825_LOG("  0x%02X %-15s: 0x%02X", reg.reg, reg.name, value);
        }
    }

    TAS5825_LOG("---------------------------");
}

// ============================================================================
// Status Query Methods (for CODEC_STATUS protocol)
// ============================================================================

uint8_t TAS5825Codec::getDeviceControlRegister()
{
    if (!initialized || !i2c) return 0xFF;
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    uint8_t value = 0;
    if (!readRegister(TAS5825M_REG_DEVICE_CTRL, &value)) return 0xFF;
    return value;
}

uint8_t TAS5825Codec::getFaultRegister()
{
    if (!initialized || !i2c) return 0xFF;
    selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    uint8_t value = 0;
    if (!readRegister(TAS5825M_REG_FAULT_CLEAR, &value)) return 0xFF;
    return value;
}

bool TAS5825Codec::testI2CConnection()
{
    if (!i2c) return false;
    i2c->beginTransmission(TAS5825M_I2C_ADDR);
    return (i2c->endTransmission() == 0);
}

bool TAS5825Codec::writeRegister(uint8_t reg, uint8_t value)
{
    if (!i2c) return false;

    i2c->beginTransmission(TAS5825M_I2C_ADDR);
    i2c->write(reg);
    i2c->write(value);
    uint8_t error = i2c->endTransmission();

    if (error != 0) {
        TAS5825_LOG("I2C write error %d (reg 0x%02X)", error, reg);
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
        TAS5825_LOG("I2C write error %d during read", error);
        return false;
    }

    if (i2c->requestFrom(TAS5825M_I2C_ADDR, (uint8_t)1) != 1) {
        TAS5825_LOG("I2C read failed");
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
    bool ok = true;

    ok &= selectBookPage(TAS5825M_BOOK_00, TAS5825M_PAGE_00);
    ok &= writeRegister(0x46, 0x11);  // Analog control register
    ok &= writeRegister(0x02, 0x00);  // Mode control
    ok &= writeRegister(TAS5825M_REG_AGAIN_L, 0x01);  // Analog gain left enable
    ok &= writeRegister(TAS5825M_REG_AGAIN_R, gainValue);  // Analog gain right

    if (!ok) {
        TAS5825_LOG("Analog gain configuration failed");
        return false;
    }
    TAS5825_LOG("Analog gain configured: 0x%02X", gainValue);
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
    
    TAS5825_LOG("Initializing DSP coefficients (basic)...");
    bool ok = true;

    // Basic DSP initialization sequence
    ok &= selectBookPage(0x8C, 0x0B);
    if (!ok) {
        TAS5825_LOG("DSP page select failed");
        return false;
    }
    
    // Write basic identity matrix coefficients (pass-through)
    // These allow audio to pass through without DSP processing
    uint8_t identity_coeffs[] = {
        0x00, 0x80, 0x00, 0x00,  // Channel 0: 1.0 coefficient
        0x00, 0x80, 0x00, 0x00   // Channel 1: 1.0 coefficient
    };

    for (size_t i = 0; i < sizeof(identity_coeffs); i++) {
        if (!writeRegister(0x28 + i, identity_coeffs[i])) {
            TAS5825_LOG("DSP coefficient write failed at offset %d", (int)i);
            return false;
        }
    }

    // Additional pages for DSP configuration
    // Note: Full configuration requires loading PPC3-generated coefficients
    // See bassowl-hat scripts for complete initialization
    
    return true;
}

#if AUDIO_DEBUG
// ============================================================================
//  DEBUG METHODS (Compiled when AUDIO_DEBUG=1)
// ============================================================================

bool TAS5825Codec::testCommunication() {
    if (!i2c) {
        TAS5825_LOG("ERROR: I2C not initialized");
        return false;
    }
    
    TAS5825_LOG("Testing I2C communication...");
    TAS5825_LOG("I2C Address: 0x%02X", TAS5825M_I2C_ADDR);
    
    // Try to read device control register
    i2c->beginTransmission(TAS5825M_I2C_ADDR);
    i2c->write(TAS5825M_REG_DEVICE_CTRL);
    uint8_t result = i2c->endTransmission();
    
    TAS5825_LOG("I2C transmission result: %d", result);
    
    switch(result) {
        case 0:
            TAS5825_LOG("SUCCESS: Device responded");
            return true;
        case 1:
            TAS5825_LOG("ERROR: Data too long");
            break;
        case 2:
            TAS5825_LOG("ERROR: NACK on address (device not found)");
            break;
        case 3:
            TAS5825_LOG("ERROR: NACK on data");
            break;
        case 4:
            TAS5825_LOG("ERROR: Other I2C error");
            break;
        case 5:
            TAS5825_LOG("ERROR: Timeout");
            break;
        default:
            TAS5825_LOG("ERROR: Unknown error code %d", result);
            break;
    }
    return false;
}

uint16_t TAS5825Codec::readRegisterCache(uint8_t reg) const {
    // TAS5825M doesn't cache registers, so read directly
    uint8_t value = 0;
    if (!i2c) return 0xFFFF;
    
    // This is a const method but we need to read - use const_cast
    TAS5825Codec* nonconst = const_cast<TAS5825Codec*>(this);
    if (!nonconst->readRegister(reg, &value)) {
        return 0xFFFF;
    }
    return value;
}

bool TAS5825Codec::writeRegisterDebug(uint8_t reg, uint16_t value) {
    TAS5825_LOG("Writing R%d (0x%02X) = 0x%02X", reg, reg, (uint8_t)value);
    bool result = writeRegister(reg, (uint8_t)value);
    if (result) {
        TAS5825_LOG("Write SUCCESS");
    } else {
        TAS5825_LOG("Write FAILED");
    }
    return result;
}

void TAS5825Codec::printStatus() {
    TAS5825_LOG("=== TAS5825M Codec Status ===");
    TAS5825_LOG("Initialized: %s", initialized ? "YES" : "NO");
    TAS5825_LOG("I2C Interface: %s", i2c ? "Connected" : "Not Connected");
    TAS5825_LOG("Sample Rate: %.1f kHz", sampleRate / 1000.0f);
    
    const char* supplyStr;
    switch(supplyVoltage) {
        case TAS5825M_12V: supplyStr = "12V"; break;
        case TAS5825M_15V: supplyStr = "15V"; break;
        case TAS5825M_20V: supplyStr = "20V"; break;
        case TAS5825M_24V: supplyStr = "24V"; break;
        default: supplyStr = "Unknown"; break;
    }
    TAS5825_LOG("Supply Voltage: %s", supplyStr);
    TAS5825_LOG("Muted: %s", muted ? "YES" : "NO");
    TAS5825_LOG("Digital Volume: 0x%02X", currentVolume);
    
    if (i2c) {
        TAS5825_LOG("I2C Test:");
        testCommunication();
    }
    
    TAS5825_LOG("Key Registers:");
    uint8_t deviceCtrl = 0, sigChCtrl = 0, digVol = 0;
    readRegister(TAS5825M_REG_DEVICE_CTRL, &deviceCtrl);
    readRegister(TAS5825M_REG_SIG_CH_CTRL, &sigChCtrl);
    readRegister(TAS5825M_REG_DIGITAL_VOL, &digVol);
    
    TAS5825_LOG("  Device Control (0x03): 0x%02X", deviceCtrl);
    TAS5825_LOG("  Signal Ch Ctrl (0x28): 0x%02X", sigChCtrl);
    TAS5825_LOG("  Digital Volume (0x4C): 0x%02X", digVol);
    TAS5825_LOG("==============================");
}

void TAS5825Codec::reinitialize(uint32_t sample_rate) {
    TAS5825_LOG("Reinitializing codec...");
    
    if (!i2c) {
        TAS5825_LOG("ERROR: I2C not configured. Use begin(Wire, sda, scl) first.");
        return;
    }
    
    initialized = false;
    
    // Use stored sample rate when caller passes 0 (default)
    if (sample_rate == 0) {
        sample_rate = sampleRate;
    }
    
    // Full reinitialization with stored I2C pins
    if (begin(*i2c, sdaPin, sclPin, sample_rate, supplyVoltage)) {
        activate();  // I2S should still be running
    }
    
    TAS5825_LOG("Reinitialization complete");
}
#endif // AUDIO_DEBUG

#endif // SFX_HAS_AUDIO
