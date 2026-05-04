/**
 * @file tas5825_m_codec.cpp
 * @brief Implementation of the TAS5825M (smart-amp variant) driver.
 *
 * Init flow mirrors tests/hw/sfx_test_m/src/sfx_test_m.ino — see that
 * file's header for the M-specific guard rationale (FAULT_CLEAR before
 * config, FS_MON gate, GPIO1_SEL override, post-PLAY UVP clear).
 */

#if defined(SFX_HAS_AUDIO)

#include "tas5825_m_codec.h"
#include "tas5825_regs.h"
#include "../audio/audio_log.h"
#include "../audio/audio_config.h"   // AUDIO_BIT_DEPTH
#include "platform/sfx_platform.h"

namespace {

using namespace sfx_audio::tas5825;

// SAP_CTRL_1 (0x60) value picked from AUDIO_BIT_DEPTH at compile time.
// Word length here MUST match the I2S TX bit width or the codec rejects
// the bus and never locks (this was the original "stuck in HIZ" bug).
constexpr uint8_t SAP_CTRL1_FOR_BIT_DEPTH =
    (AUDIO_BIT_DEPTH == 32) ? SAP_WORD_32BIT :
    (AUDIO_BIT_DEPTH == 24) ? SAP_WORD_24BIT :
                              SAP_WORD_16BIT;

// FS_MON polling cadence used during activate(). Matches the test
// firmware in tests/hw/sfx_test_m/.
constexpr uint32_t FS_MON_POLL_INTERVAL_MS = 5;
constexpr uint32_t FS_MON_POLL_TIMEOUT_MS  = 500;

// GPIO1_SEL: route GPIO1 to "FAULT asserted, active-high" so the
// FAULT pin we read on the expander reflects real codec faults.
// On the M, bit pattern 0x01 (used by the P for Hybrid-Pro) is
// reserved and leaves GPIO1 in an undefined state.
constexpr uint8_t GPIO1_SEL_FAULT = 0x0B;

// Smart-amp register addresses in book 0xAA / page 0x24.
// The M's IV-sense ADC enable bit lives in the DSP RAM, not in book 0.
// We tunnel through the book/page mechanism for these.
constexpr uint8_t SMARTAMP_BOOK = 0xAA;
constexpr uint8_t SMARTAMP_PAGE = 0x24;

}  // namespace

// ─── Construction ─────────────────────────────────────────────────────────

TAS5825MCodec::TAS5825MCodec() = default;

// ─── Phase 1: pre-clock init ──────────────────────────────────────────────

bool TAS5825MCodec::begin(TwoWire& wire, int sda, int scl,
                          uint32_t sample_rate, Supply supply) {
    bool needWireInit = (i2c_ != &wire || sdaPin_ != sda || sclPin_ != scl);

    i2c_       = &wire;
    sdaPin_    = sda;
    sclPin_    = scl;
    sampleRate_ = sample_rate;
    supply_    = supply;

    if (needWireInit) {
#if SFX_PLATFORM_PICO
        i2c_->setSDA(sdaPin_);
        i2c_->setSCL(sclPin_);
        i2c_->begin();
#elif SFX_PLATFORM_ESP32
        i2c_->begin(sdaPin_, sclPin_);
#endif
    }
    i2c_->setClock(100000);

    TAS5825_LOG("TAS5825M: probing @ 0x%02X (SDA=%d SCL=%d)...",
                I2C_ADDR, sdaPin_, sclPin_);

    // I2C probe with retry — bus may be transiently unavailable post-boot.
    constexpr int PROBE_RETRIES = 3;
    constexpr int PROBE_DELAY_MS = 500;
    uint8_t probeResult = 0;
    for (int attempt = 0; attempt < PROBE_RETRIES; attempt++) {
        i2c_->beginTransmission(I2C_ADDR);
        probeResult = i2c_->endTransmission();
        if (probeResult == 0) break;
        if (attempt < PROBE_RETRIES - 1) {
            TAS5825_LOG("  probe %d/%d failed (err %d), retry in %dms",
                        attempt + 1, PROBE_RETRIES, probeResult, PROBE_DELAY_MS);
            SFX_DELAY_MS(PROBE_DELAY_MS);
        }
    }
    if (probeResult != 0) {
        TAS5825_LOG("TAS5825M: probe FAILED (err %d) — check wiring/PVDD",
                    probeResult);
        initialized_ = false;
        return false;
    }
    TAS5825_LOG("TAS5825M: probe OK");

    // Phase 1: park muted, full reset, re-park muted, clear fault latch.
    selectBookPage(BOOK_00, PAGE_00);

    // Park in DEEP_SLEEP + MUTE so DAC stays silent during config.
    writeRegister(REG_DEVICE_CTRL, CTRL_DEEP_SLEEP | CTRL_MUTE_BIT);
    SFX_DELAY_MS(5);

    // Full reset (DSP + registers).
    writeRegister(REG_RESET, 0x11);
    SFX_DELAY_MS(50);

    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL, CTRL_DEEP_SLEEP | CTRL_MUTE_BIT);

    // M-SPECIFIC: clear the boot-time CDET latch (clocks have been
    // absent — the M asserts CDET and refuses HIZ until acknowledged).
    writeRegister(REG_FAULT_CLEAR, 0x80);
    SFX_DELAY_MS(5);

    initialized_ = true;
    smartAmpOn_  = false;

    TAS5825_LOG("TAS5825M: phase 1 done — DEEP_SLEEP+MUTE, fault latch cleared "
                "(SR=%lu, supply=%s)",
                sampleRate_, supplyStr(supply_));
    return true;
}

bool TAS5825MCodec::begin(uint32_t /*sample_rate*/) {
    TAS5825_LOG("TAS5825M: must call begin(Wire, sda, scl, ...)");
    return false;
}

// ─── Phase 2: post-clock activation (M-specific) ──────────────────────────

bool TAS5825MCodec::activate() {
    if (!initialized_ || !i2c_) {
        TAS5825_LOG("TAS5825M: activate() before begin()");
        return false;
    }

    TAS5825_LOG("TAS5825M: phase 2 — configuring (I2S clocks must be running)");

    // Step 1: analog gain for the configured supply voltage.
    if (!configureAnalogGain()) {
        TAS5825_LOG("TAS5825M: analog-gain config failed");
        return false;
    }

    // Step 2: clock auto-detect + I2S format.
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_CLK_SRC, 0x00);
    writeRegister(REG_FS_RATE, 0x00);
    writeRegister(REG_SDOUT_SEL, 0x00);
    writeRegister(REG_SAP_CTRL1, SAP_CTRL1_FOR_BIT_DEPTH);
    writeRegister(REG_DSP_MISC, 0x09);

    // Step 3: M-SPECIFIC GPIO1_SEL override (default value varies by
    // silicon revision; some lots inherit the P's Hybrid-Pro setting
    // which is RESERVED on the M).
    writeRegister(REG_GPIO1_SEL, GPIO1_SEL_FAULT);

    // Step 4: identity-DSP coefficients (pass-through). Smart-amp
    // pages are NOT touched here — `smartAmpEnable()` configures
    // them on demand.
    if (!initDSPCoefficients()) {
        TAS5825_LOG("TAS5825M: DSP coefficient init failed");
        return false;
    }

    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DIGITAL_VOL, currentVolume_);

    // Step 5: M-SPECIFIC mid-config fault clear (config writes can
    // tickle CDET as auto-detect re-evaluates).
    writeRegister(REG_FAULT_CLEAR, 0x80);
    SFX_DELAY_MS(5);

    // Step 6: M-SPECIFIC FS_MON gate — wait for valid sample-rate code
    // BEFORE the HIZ → PLAY transition. The P would auto-detect during
    // the transition; the M won't.
    uint8_t fsCode = 0;
    if (!waitForFsLock(FS_MON_POLL_TIMEOUT_MS, &fsCode)) {
        TAS5825_LOG("TAS5825M: FS_MON never locked — check BCLK/LRCLK wiring");
        return false;
    }
    TAS5825_LOG("TAS5825M: FS_MON locked at %s (code 0x%02X)",
                fsMonStr(fsCode), fsCode);

    // Step 7: DEEP_SLEEP → HIZ.
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL, CTRL_HIZ);
    SFX_DELAY_MS(20);

    // Step 8: HIZ → PLAY (un-muted now — drop the MUTE bit).
    writeRegister(REG_DEVICE_CTRL, CTRL_PLAY);
    SFX_DELAY_MS(20);

    // Step 9: M-SPECIFIC post-PLAY clear — modulator inrush can
    // briefly assert PVDD_UVP.
    writeRegister(REG_FAULT_CLEAR, 0x80);
    SFX_DELAY_MS(50);

    uint8_t powerState = 0;
    readRegister(REG_POWER_STATE, &powerState);
    bool inPlay = (powerState & 0x0F) == CTRL_PLAY;

    if (inPlay) {
        TAS5825_LOG("TAS5825M: PLAY ✓ (FS=%s)", fsMonStr(fsCode));
    } else {
        uint8_t fault = 0;
        readRegister(REG_GLOBAL1, &fault);
        TAS5825_LOG("TAS5825M: PLAY FAILED — POWER_STATE=0x%02X GLOBAL1=0x%02X",
                    powerState, fault);
    }
    return inPlay;
}

// ─── Volume / mute / reset ────────────────────────────────────────────────

void TAS5825MCodec::setVolume(float volume) {
    if (!initialized_) return;

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    currentVolume_ = static_cast<uint8_t>(volume * VOL_0DB);
    if (currentVolume_ > VOL_0DB) currentVolume_ = VOL_0DB;

    if (!muted_) {
        selectBookPage(BOOK_00, PAGE_00);
        writeRegister(REG_DIGITAL_VOL, currentVolume_);
        TAS5825_LOG("TAS5825M: vol = %.0f%% (0x%02X)",
                    volume * 100.0f, currentVolume_);
    }
}

void TAS5825MCodec::setVolumeDB(float db) {
    if (!initialized_) return;
    if (db < -100.0f) db = -100.0f;
    if (db > 24.0f)   db = 24.0f;
    currentVolume_ = static_cast<uint8_t>((db / 0.5f) + 48.0f);
    if (currentVolume_ > VOL_MAX) currentVolume_ = VOL_MAX;
    if (!muted_) {
        selectBookPage(BOOK_00, PAGE_00);
        writeRegister(REG_DIGITAL_VOL, currentVolume_);
        TAS5825_LOG("TAS5825M: vol = %.1fdB (0x%02X)", db, currentVolume_);
    }
}

void TAS5825MCodec::setMute(bool mute) {
    if (!initialized_) return;
    muted_ = mute;
    selectBookPage(BOOK_00, PAGE_00);
    if (mute) {
        writeRegister(REG_DIGITAL_VOL, VOL_MUTE);
        TAS5825_LOG("TAS5825M: muted");
    } else {
        writeRegister(REG_DIGITAL_VOL, currentVolume_);
        TAS5825_LOG("TAS5825M: unmuted");
    }
}

void TAS5825MCodec::reset() {
    if (!initialized_) return;
    TAS5825_LOG("TAS5825M: reset");
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL, CTRL_DEEP_SLEEP | CTRL_MUTE_BIT);
    writeRegister(REG_RESET, 0x11);
    SFX_DELAY_MS(50);

    initialized_ = false;
    smartAmpOn_  = false;
    if (begin(*i2c_, sdaPin_, sclPin_, sampleRate_, supply_)) {
        activate();
    }
}

bool TAS5825MCodec::setSupplyVoltage(Supply voltage) {
    if (!initialized_) return false;
    if (voltage == supply_) return true;
    TAS5825_LOG("TAS5825M: supply %s → %s", supplyStr(supply_), supplyStr(voltage));

    // Briefly DEEP_SLEEP to safely change analog gain.
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL, CTRL_DEEP_SLEEP | CTRL_MUTE_BIT);
    SFX_DELAY_MS(2);

    supply_ = voltage;
    if (!configureAnalogGain()) {
        TAS5825_LOG("TAS5825M: analog gain reconfigure failed");
        // Recover to PLAY anyway.
        writeRegister(REG_DEVICE_CTRL, CTRL_HIZ);
        SFX_DELAY_MS(5);
        writeRegister(REG_DEVICE_CTRL, CTRL_PLAY);
        return false;
    }
    writeRegister(REG_DEVICE_CTRL, CTRL_HIZ);
    SFX_DELAY_MS(5);
    writeRegister(REG_DEVICE_CTRL, CTRL_PLAY);
    writeRegister(REG_FAULT_CLEAR, 0x80);
    return true;
}

// ─── Fault management ─────────────────────────────────────────────────────

bool TAS5825MCodec::clearFaults() {
    if (!initialized_) return false;
    selectBookPage(BOOK_00, PAGE_00);
    return writeRegister(REG_FAULT_CLEAR, 0x80);
}

uint8_t TAS5825MCodec::readFaultRegister() {
    if (!initialized_) return 0xFF;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    readRegister(REG_GLOBAL1, &v);
    return v;
}

// ─── Smart-amp / IV-sense API (M-only) ────────────────────────────────────

bool TAS5825MCodec::smartAmpEnable(bool on) {
    if (!initialized_) {
        TAS5825_LOG("TAS5825M: smartAmpEnable(%d) — not initialized", on);
        return false;
    }

    // The smart-amp DSP runs in book 0xAA / page 0x24. To activate we
    // need:
    //   1. Write the speaker model coefficients (Re/Le/Qts/excursion
    //      limit / thermal time-constants) to that page.
    //   2. Set the smart-amp enable bit in DSP_MISC bit 4.
    //   3. Enable IV-sense ADCs.
    //
    // Until we have a per-speaker calibration pipeline, this is a
    // stub that toggles the enable bit only — leaving the chip's
    // factory-default protection thresholds in effect (conservative
    // but functional).
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t miscVal = 0;
    readRegister(REG_DSP_MISC, &miscVal);
    if (on) {
        miscVal |= 0x10;
    } else {
        miscVal &= ~0x10;
    }
    writeRegister(REG_DSP_MISC, miscVal);

    setIvSenseEnabled(on);

    smartAmpOn_ = on;
    TAS5825_LOG("TAS5825M: smart-amp %s (DSP_MISC=0x%02X)",
                on ? "ENABLED" : "disabled", miscVal);
    return true;
}

bool TAS5825MCodec::calibrateSpeaker() {
    if (!initialized_) {
        TAS5825_LOG("TAS5825M: calibrateSpeaker — not initialized");
        return false;
    }
    // Stub — full PPC3 calibration sequence not yet implemented. The
    // chip's factory protection thresholds are still in effect and
    // are conservative enough to be safe for typical ≤4Ω drivers.
    TAS5825_LOG("TAS5825M: calibrateSpeaker (stub — using factory defaults)");
    return true;
}

int16_t TAS5825MCodec::readSpeakerTemp_C() {
    if (!initialized_ || !smartAmpOn_) return 0;
    // The smart-amp temperature estimate lives in the DSP RAM at a
    // documented offset within book 0xAA / page 0x24. Reading DSP RAM
    // requires the page-banked indirect-read sequence; until the
    // calibration pipeline lands we return 0.
    return 0;
}

uint16_t TAS5825MCodec::readSpeakerExcursion_mm100() {
    if (!initialized_ || !smartAmpOn_) return 0;
    return 0;  // see readSpeakerTemp_C()
}

bool TAS5825MCodec::setIvSenseEnabled(bool on) {
    if (!initialized_) return false;
    // IV-sense enable bit lives in book 0xAA — for now we just route
    // it through SDOUT_SEL so the I2S TDM frame can carry the IV data
    // upstream if smart-amp is on.
    selectBookPage(BOOK_00, PAGE_00);
    if (on) {
        // SDOUT carries DSP output; bit 5 enables IV-sense channel
        // overlay (M-only). Conservative — leave SDOUT alone for now.
        TAS5825_LOG("TAS5825M: IV-sense enable (stub)");
    } else {
        TAS5825_LOG("TAS5825M: IV-sense disable");
    }
    return true;
}

// ─── Diagnostics ──────────────────────────────────────────────────────────

void TAS5825MCodec::dumpRegisters() {
    if (!initialized_) {
        TAS5825_LOG("TAS5825M: dumpRegisters — not initialized");
        return;
    }
    TAS5825_LOG("─── TAS5825M register dump ───");
    selectBookPage(BOOK_00, PAGE_00);
    struct { uint8_t reg; const char* name; } regs[] = {
        {REG_DEVICE_CTRL,  "DEVICE_CTRL"},
        {REG_POWER_STATE,  "POWER_STATE"},
        {REG_FS_MON,       "FS_MON"},
        {REG_SDOUT_SEL,    "SDOUT_SEL"},
        {REG_DIGITAL_VOL,  "DIGITAL_VOL"},
        {REG_AGAIN_L,      "AGAIN_L"},
        {REG_AGAIN_R,      "AGAIN_R"},
        {REG_GPIO1_SEL,    "GPIO1_SEL"},
        {REG_SAP_CTRL1,    "SAP_CTRL1"},
        {REG_DSP_MISC,     "DSP_MISC"},
        {REG_GLOBAL1,      "GLOBAL1"},
        {REG_GLOBAL2,      "GLOBAL2"},
    };
    for (const auto& r : regs) {
        uint8_t v = 0;
        if (readRegister(r.reg, &v)) {
            TAS5825_LOG("  0x%02X %-12s : 0x%02X", r.reg, r.name, v);
        }
    }
}

uint8_t TAS5825MCodec::getDeviceControlRegister() {
    if (!initialized_) return 0xFF;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    readRegister(REG_DEVICE_CTRL, &v);
    return v;
}

uint8_t TAS5825MCodec::getFaultRegister() {
    if (!initialized_) return 0xFF;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    readRegister(REG_FAULT_CLEAR, &v);
    return v;
}

bool TAS5825MCodec::testI2CConnection() {
    if (!i2c_) return false;
    i2c_->beginTransmission(I2C_ADDR);
    return i2c_->endTransmission() == 0;
}

// ─── Private helpers ──────────────────────────────────────────────────────

bool TAS5825MCodec::writeRegister(uint8_t reg, uint8_t value) {
    if (!i2c_) return false;
    i2c_->beginTransmission(I2C_ADDR);
    i2c_->write(reg);
    i2c_->write(value);
    return i2c_->endTransmission() == 0;
}

bool TAS5825MCodec::readRegister(uint8_t reg, uint8_t* value) {
    if (!i2c_ || !value) return false;
    i2c_->beginTransmission(I2C_ADDR);
    i2c_->write(reg);
    if (i2c_->endTransmission(false) != 0) return false;
    if (i2c_->requestFrom((uint8_t)I2C_ADDR, (uint8_t)1) != 1) return false;
    *value = i2c_->read();
    return true;
}

bool TAS5825MCodec::selectBookPage(uint8_t book, uint8_t page) {
    if (!writeRegister(REG_PAGE, page)) return false;
    if (!writeRegister(REG_BOOK, book)) return false;
    return true;
}

bool TAS5825MCodec::waitForFsLock(uint32_t timeoutMs, uint8_t* outFsCode) {
    uint32_t start = millis();
    while (true) {
        uint8_t fs = 0;
        readRegister(REG_FS_MON, &fs);
        fs &= 0x0F;
        if (fs != FS_NONE && fs != FS_INVALID) {
            if (outFsCode) *outFsCode = fs;
            return true;
        }
        if (millis() - start > timeoutMs) {
            if (outFsCode) *outFsCode = fs;
            return false;
        }
        SFX_DELAY_MS(FS_MON_POLL_INTERVAL_MS);
    }
}

bool TAS5825MCodec::initDSPCoefficients() {
    // Identity pass-through coefficients in book 0x8C / page 0x0B.
    // Same as the prior driver — full DSP coefficient set requires
    // PPC3 export per audio configuration.
    if (!selectBookPage(0x8C, 0x0B)) return false;
    constexpr uint8_t identity[] = {
        0x00, 0x80, 0x00, 0x00,  // Channel 0: 1.0
        0x00, 0x80, 0x00, 0x00,  // Channel 1: 1.0
    };
    for (size_t i = 0; i < sizeof(identity); i++) {
        if (!writeRegister(0x28 + i, identity[i])) return false;
    }
    return true;
}

bool TAS5825MCodec::configureAnalogGain() {
    selectBookPage(BOOK_00, PAGE_00);
    bool ok = true;
    ok &= writeRegister(REG_ANALOG_CTRL, 0x11);
    ok &= writeRegister(REG_MODE_CTRL, 0x00);
    ok &= writeRegister(REG_AGAIN_L, 0x01);
    ok &= writeRegister(REG_AGAIN_R, analogGainFor(supply_));
    if (ok) {
        TAS5825_LOG("TAS5825M: analog gain set for %s (AGAIN_R=0x%02X)",
                    supplyStr(supply_), analogGainFor(supply_));
    }
    return ok;
}

#endif // SFX_HAS_AUDIO
