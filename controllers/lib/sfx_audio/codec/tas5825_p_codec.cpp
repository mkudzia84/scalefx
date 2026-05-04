/**
 * @file tas5825_p_codec.cpp
 * @brief Implementation of the TAS5825P (Class-H + Hybrid-Pro) driver.
 *
 * Init flow mirrors tests/hw/sfx_test_p/src/sfx_test_p.ino — the
 * permissive sequence proven on the original HubFX silicon. The
 * Hybrid-Pro / boost-converter methods are stubs against the wire
 * format; the HubFX board does not have an external DC-DC on the BOM
 * so these paths are exercised only on bespoke designs that wire
 * GPIO1 → HPFB.
 */

#if defined(SFX_HAS_AUDIO)

#include "tas5825_p_codec.h"
#include "tas5825_regs.h"
#include "../audio/audio_log.h"
#include "../audio/audio_config.h"
#include "platform/sfx_platform.h"

namespace {

using namespace sfx_audio::tas5825;

constexpr uint8_t SAP_CTRL1_FOR_BIT_DEPTH =
    (AUDIO_BIT_DEPTH == 32) ? SAP_WORD_32BIT :
    (AUDIO_BIT_DEPTH == 24) ? SAP_WORD_24BIT :
                              SAP_WORD_16BIT;

// GPIO1_SEL bit pattern that selects HPFB on the P silicon.
constexpr uint8_t GPIO1_SEL_HPFB  = 0x01;
constexpr uint8_t GPIO1_SEL_FAULT = 0x0B;

// Hybrid-Pro configuration register (book 0, P-only).
// The actual register address varies between silicon revisions; the
// commonly-cited value for TAS5825P RHB silicon is 0x4D for the
// Hybrid-Pro target voltage.
constexpr uint8_t REG_HYBRID_PRO_TARGET = 0x4D;
constexpr uint8_t REG_HYBRID_PRO_CTRL   = 0x4E;
constexpr uint8_t REG_BOOST_MIN         = 0x52;  // P-specific
constexpr uint8_t REG_BOOST_MAX         = 0x55;  // P-specific
constexpr uint8_t REG_BOOST_FEEDBACK    = 0x6A;  // P-specific PVDD ADC

}  // namespace

// ─── Construction ─────────────────────────────────────────────────────────

TAS5825PCodec::TAS5825PCodec() = default;

// ─── Phase 1: pre-clock init (P-permissive) ──────────────────────────────

bool TAS5825PCodec::begin(TwoWire& wire, int sda, int scl,
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

    TAS5825_LOG("TAS5825P: probing @ 0x%02X (SDA=%d SCL=%d)...",
                I2C_ADDR, sdaPin_, sclPin_);

    constexpr int PROBE_RETRIES = 3;
    constexpr int PROBE_DELAY_MS = 500;
    uint8_t probeResult = 0;
    for (int attempt = 0; attempt < PROBE_RETRIES; attempt++) {
        i2c_->beginTransmission(I2C_ADDR);
        probeResult = i2c_->endTransmission();
        if (probeResult == 0) break;
        if (attempt < PROBE_RETRIES - 1) SFX_DELAY_MS(PROBE_DELAY_MS);
    }
    if (probeResult != 0) {
        TAS5825_LOG("TAS5825P: probe FAILED (err %d)", probeResult);
        initialized_ = false;
        return false;
    }
    TAS5825_LOG("TAS5825P: probe OK");

    // Phase 1: simple reset → DEEP_SLEEP. P silicon doesn't need the
    // explicit fault-clear that the M does (no CDET latch on this
    // variant).
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_RESET, 0x11);
    SFX_DELAY_MS(5);
    writeRegister(REG_DEVICE_CTRL, CTRL_DEEP_SLEEP);
    SFX_DELAY_MS(5);

    initialized_ = true;
    hybridProOn_ = false;
    TAS5825_LOG("TAS5825P: phase 1 done — DEEP_SLEEP (SR=%lu, supply=%s)",
                sampleRate_, supplyStr(supply_));
    return true;
}

bool TAS5825PCodec::begin(uint32_t /*sample_rate*/) {
    TAS5825_LOG("TAS5825P: must call begin(Wire, sda, scl, ...)");
    return false;
}

// ─── Phase 2: post-clock activation (P-permissive) ───────────────────────

bool TAS5825PCodec::activate() {
    if (!initialized_ || !i2c_) {
        TAS5825_LOG("TAS5825P: activate() before begin()");
        return false;
    }
    TAS5825_LOG("TAS5825P: phase 2 — configuring");

    if (!configureAnalogGain()) return false;

    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_CLK_SRC,    0x00);
    writeRegister(REG_FS_RATE,    0x00);
    writeRegister(REG_SDOUT_SEL,  0x00);
    writeRegister(REG_SAP_CTRL1,  SAP_CTRL1_FOR_BIT_DEPTH);
    writeRegister(REG_DSP_MISC,   0x09);

    if (!initDSPCoefficients()) return false;

    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DIGITAL_VOL, currentVolume_);

    // P does not need an FS_MON gate before HIZ — the chip
    // auto-detects sample rate during the transition. Permissive flow.
    writeRegister(REG_DEVICE_CTRL, CTRL_HIZ);
    SFX_DELAY_MS(5);

    uint8_t fsMon = 0;
    readRegister(REG_FS_MON, &fsMon);
    TAS5825_LOG("TAS5825P: HIZ — FS_MON=0x%02X (%s)", fsMon, fsMonStr(fsMon));

    writeRegister(REG_DEVICE_CTRL, CTRL_PLAY);
    SFX_DELAY_MS(5);

    uint8_t pwr = 0;
    readRegister(REG_POWER_STATE, &pwr);
    readRegister(REG_FS_MON, &fsMon);
    TAS5825_LOG("TAS5825P: PLAY — POWER_STATE=0x%02X FS_MON=0x%02X",
                pwr, fsMon);

    if (fsMon == 0) {
        // Fallback that the original P-flow used: bounce through
        // DEEP_SLEEP → PLAY directly to re-trigger auto-detect.
        TAS5825_LOG("TAS5825P: PLL not locked via HIZ — bouncing through DEEP_SLEEP");
        writeRegister(REG_DEVICE_CTRL, CTRL_DEEP_SLEEP);
        SFX_DELAY_MS(5);
        writeRegister(REG_DEVICE_CTRL, CTRL_PLAY);
        SFX_DELAY_MS(5);
        readRegister(REG_POWER_STATE, &pwr);
    }

    writeRegister(REG_FAULT_CLEAR, 0x80);
    bool inPlay = (pwr & 0x0F) == CTRL_PLAY;
    TAS5825_LOG("TAS5825P: %s (POWER_STATE=0x%02X)",
                inPlay ? "PLAY ✓" : "PLAY FAILED", pwr);
    return inPlay;
}

// ─── Volume / mute / reset ────────────────────────────────────────────────

void TAS5825PCodec::setVolume(float volume) {
    if (!initialized_) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    currentVolume_ = static_cast<uint8_t>(volume * VOL_0DB);
    if (currentVolume_ > VOL_0DB) currentVolume_ = VOL_0DB;
    if (!muted_) {
        selectBookPage(BOOK_00, PAGE_00);
        writeRegister(REG_DIGITAL_VOL, currentVolume_);
    }
}

void TAS5825PCodec::setVolumeDB(float db) {
    if (!initialized_) return;
    if (db < -100.0f) db = -100.0f;
    if (db > 24.0f)   db = 24.0f;
    currentVolume_ = static_cast<uint8_t>((db / 0.5f) + 48.0f);
    if (currentVolume_ > VOL_MAX) currentVolume_ = VOL_MAX;
    if (!muted_) {
        selectBookPage(BOOK_00, PAGE_00);
        writeRegister(REG_DIGITAL_VOL, currentVolume_);
    }
}

void TAS5825PCodec::setMute(bool mute) {
    if (!initialized_) return;
    muted_ = mute;
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DIGITAL_VOL, mute ? VOL_MUTE : currentVolume_);
}

void TAS5825PCodec::reset() {
    if (!initialized_) return;
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL, CTRL_DEEP_SLEEP);
    writeRegister(REG_RESET, 0x11);
    SFX_DELAY_MS(50);
    initialized_ = false;
    hybridProOn_ = false;
    if (begin(*i2c_, sdaPin_, sclPin_, sampleRate_, supply_)) {
        activate();
    }
}

bool TAS5825PCodec::setSupplyVoltage(Supply voltage) {
    if (!initialized_) return false;
    if (voltage == supply_) return true;
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL, CTRL_DEEP_SLEEP);
    SFX_DELAY_MS(2);
    supply_ = voltage;
    if (!configureAnalogGain()) {
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

bool TAS5825PCodec::clearFaults() {
    if (!initialized_) return false;
    selectBookPage(BOOK_00, PAGE_00);
    return writeRegister(REG_FAULT_CLEAR, 0x80);
}

uint8_t TAS5825PCodec::readFaultRegister() {
    if (!initialized_) return 0xFF;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    readRegister(REG_GLOBAL1, &v);
    return v;
}

// ─── Hybrid-Pro / boost API (P-only) ─────────────────────────────────────

bool TAS5825PCodec::hybridProEnable(bool on) {
    if (!initialized_) return false;
    selectBookPage(BOOK_00, PAGE_00);

    // Toggle GPIO1 between FAULT and HPFB.
    writeRegister(REG_GPIO1_SEL, on ? GPIO1_SEL_HPFB : GPIO1_SEL_FAULT);

    // Toggle the algorithm enable bit in HYBRID_PRO_CTRL.
    uint8_t ctrl = 0;
    readRegister(REG_HYBRID_PRO_CTRL, &ctrl);
    if (on) {
        ctrl |= 0x01;  // ENGINE_ENABLE
    } else {
        ctrl &= ~0x01;
    }
    writeRegister(REG_HYBRID_PRO_CTRL, ctrl);

    hybridProOn_ = on;
    TAS5825_LOG("TAS5825P: Hybrid-Pro %s (CTRL=0x%02X)",
                on ? "ENABLED" : "disabled", ctrl);
    return true;
}

bool TAS5825PCodec::hybridProSetTarget(uint16_t targetMv) {
    if (!initialized_ || !hybridProOn_) return false;
    selectBookPage(BOOK_00, PAGE_00);
    // Target register is u8 in 100-mV units (clamp to 25.5 V).
    uint16_t val = targetMv / 100;
    if (val > 0xFF) val = 0xFF;
    writeRegister(REG_HYBRID_PRO_TARGET, static_cast<uint8_t>(val));
    TAS5825_LOG("TAS5825P: Hybrid-Pro target = %u mV", targetMv);
    return true;
}

bool TAS5825PCodec::boostConfigure(uint16_t minMv, uint16_t maxMv) {
    if (!initialized_) return false;
    boostMinMv_ = minMv;
    boostMaxMv_ = maxMv;
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_BOOST_MIN, static_cast<uint8_t>(minMv / 100));
    writeRegister(REG_BOOST_MAX, static_cast<uint8_t>(maxMv / 100));
    TAS5825_LOG("TAS5825P: boost envelope %u-%u mV", minMv, maxMv);
    return true;
}

uint16_t TAS5825PCodec::readBoostVoltage_mV() {
    if (!initialized_ || !hybridProOn_) return 0;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    if (!readRegister(REG_BOOST_FEEDBACK, &v)) return 0;
    return static_cast<uint16_t>(v) * 100;  // 100-mV LSB
}

// ─── Diagnostics ──────────────────────────────────────────────────────────

void TAS5825PCodec::dumpRegisters() {
    if (!initialized_) return;
    TAS5825_LOG("─── TAS5825P register dump ───");
    selectBookPage(BOOK_00, PAGE_00);
    struct { uint8_t reg; const char* name; } regs[] = {
        {REG_DEVICE_CTRL, "DEVICE_CTRL"},
        {REG_POWER_STATE, "POWER_STATE"},
        {REG_FS_MON,      "FS_MON"},
        {REG_DIGITAL_VOL, "DIGITAL_VOL"},
        {REG_AGAIN_R,     "AGAIN_R"},
        {REG_GPIO1_SEL,   "GPIO1_SEL"},
        {REG_SAP_CTRL1,   "SAP_CTRL1"},
        {REG_GLOBAL1,     "GLOBAL1"},
        {REG_HYBRID_PRO_CTRL,   "HPRO_CTRL"},
        {REG_HYBRID_PRO_TARGET, "HPRO_TARGET"},
    };
    for (const auto& r : regs) {
        uint8_t v = 0;
        if (readRegister(r.reg, &v)) {
            TAS5825_LOG("  0x%02X %-12s : 0x%02X", r.reg, r.name, v);
        }
    }
}

uint8_t TAS5825PCodec::getDeviceControlRegister() {
    if (!initialized_) return 0xFF;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    readRegister(REG_DEVICE_CTRL, &v);
    return v;
}

uint8_t TAS5825PCodec::getFaultRegister() {
    if (!initialized_) return 0xFF;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    readRegister(REG_FAULT_CLEAR, &v);
    return v;
}

bool TAS5825PCodec::testI2CConnection() {
    if (!i2c_) return false;
    i2c_->beginTransmission(I2C_ADDR);
    return i2c_->endTransmission() == 0;
}

// ─── Private helpers ──────────────────────────────────────────────────────

bool TAS5825PCodec::writeRegister(uint8_t reg, uint8_t value) {
    if (!i2c_) return false;
    i2c_->beginTransmission(I2C_ADDR);
    i2c_->write(reg);
    i2c_->write(value);
    return i2c_->endTransmission() == 0;
}

bool TAS5825PCodec::readRegister(uint8_t reg, uint8_t* value) {
    if (!i2c_ || !value) return false;
    i2c_->beginTransmission(I2C_ADDR);
    i2c_->write(reg);
    if (i2c_->endTransmission(false) != 0) return false;
    if (i2c_->requestFrom((uint8_t)I2C_ADDR, (uint8_t)1) != 1) return false;
    *value = i2c_->read();
    return true;
}

bool TAS5825PCodec::selectBookPage(uint8_t book, uint8_t page) {
    if (!writeRegister(REG_PAGE, page)) return false;
    if (!writeRegister(REG_BOOK, book)) return false;
    return true;
}

bool TAS5825PCodec::initDSPCoefficients() {
    if (!selectBookPage(0x8C, 0x0B)) return false;
    constexpr uint8_t identity[] = {
        0x00, 0x80, 0x00, 0x00,
        0x00, 0x80, 0x00, 0x00,
    };
    for (size_t i = 0; i < sizeof(identity); i++) {
        if (!writeRegister(0x28 + i, identity[i])) return false;
    }
    return true;
}

bool TAS5825PCodec::configureAnalogGain() {
    selectBookPage(BOOK_00, PAGE_00);
    bool ok = true;
    ok &= writeRegister(REG_ANALOG_CTRL, 0x11);
    ok &= writeRegister(REG_MODE_CTRL, 0x00);
    ok &= writeRegister(REG_AGAIN_L, 0x01);
    ok &= writeRegister(REG_AGAIN_R, analogGainFor(supply_));
    return ok;
}

#endif // SFX_HAS_AUDIO
