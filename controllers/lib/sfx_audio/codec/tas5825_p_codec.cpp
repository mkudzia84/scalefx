/**
 * @file tas5825_p_codec.cpp
 * @brief Implementation of the TAS5825P (Class-H + Hybrid-Pro) driver.
 *
 * Shared-map register addresses follow the TI TAS5825M datasheet
 * SLASEH7H (gold standard — see tas5825_regs.h; the family shares the
 * book-0/page-0 map). The startup flow is the P's permissive one: no
 * FS_MON gate before HIZ → PLAY, with a DEEP_SLEEP bounce fallback.
 *
 * The Hybrid-Pro / boost-converter methods are stubs: their P-only
 * register addresses are folk-cited, NOT verified against an official
 * TAS5825P datasheet, and the HubFX BOM has no external DC-DC — they
 * are never invoked in production. Verify against TI documentation
 * before wiring them to real hardware.
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

// GPIO1_SEL function code that selects HPFB on P silicon (0x01 is
// Reserved on the M — Table 9-42).
constexpr uint8_t GPIO_SEL_HPFB = 0x01;

// Hybrid-Pro configuration registers (P-only, UNVERIFIED — no official
// TAS5825P datasheet audited; folk-cited values for RHB silicon).
constexpr uint8_t REG_HYBRID_PRO_TARGET = 0x4D;
constexpr uint8_t REG_HYBRID_PRO_CTRL   = 0x4E;
constexpr uint8_t REG_BOOST_MIN         = 0x52;
constexpr uint8_t REG_BOOST_MAX         = 0x55;
constexpr uint8_t REG_BOOST_FEEDBACK    = 0x6A;

// Parking value while clocks are absent: DSP held in reset + deep sleep
// (matches the silicon reset value of DEVICE_CTRL2).
constexpr uint8_t CTRL_PARKED = CTRL_DIS_DSP_BIT | CTRL_DEEP_SLEEP;

}  // namespace

// ─── Construction ─────────────────────────────────────────────────────────

TAS5825PCodec::TAS5825PCodec() = default;

// ─── Phase 1: pre-clock init (P-permissive) ──────────────────────────────

bool TAS5825PCodec::begin(sfx_peripherals::SfxI2cBus& wire, int sda, int scl,
                          uint32_t sample_rate) {
    bool needWireInit = (i2c_ != &wire || sdaPin_ != sda || sclPin_ != scl);

    i2c_       = &wire;
    sdaPin_    = sda;
    sclPin_    = scl;
    sampleRate_ = sample_rate;

    if (needWireInit) {
        i2c_->begin(sdaPin_, sclPin_, 100000);   // native bus bring-up (100 kHz)
    }

    TAS5825_LOG("TAS5825P: probing @ 0x%02X (SDA=%d SCL=%d)...",
                I2C_ADDR, sdaPin_, sclPin_);

    constexpr int PROBE_RETRIES = 3;
    constexpr int PROBE_DELAY_MS = 500;
    bool probeOk = false;
    for (int attempt = 0; attempt < PROBE_RETRIES; attempt++) {
        probeOk = i2c_->probe(I2C_ADDR);
        if (probeOk) break;
        if (attempt < PROBE_RETRIES - 1) SFX_DELAY_MS(PROBE_DELAY_MS);
    }
    if (!probeOk) {
        TAS5825_LOG("TAS5825P: probe FAILED");
        initialized_ = false;
        return false;
    }

    selectBookPage(BOOK_00, PAGE_00);
    dieId_ = 0;
    readRegister(REG_DIE_ID, &dieId_);
    TAS5825_LOG("TAS5825P: probe OK, DIE_ID=0x%02X%s", dieId_,
                dieId_ == DIE_ID_TAS5825M ? " (M silicon!)" : "");

    // Phase 1: full reset, park with the DSP held (reset state).
    writeRegister(REG_RESET_CTRL, 0x11);
    SFX_DELAY_MS(5);
    writeRegister(REG_DEVICE_CTRL2, CTRL_PARKED);
    SFX_DELAY_MS(5);

    initialized_ = true;
    hybridProOn_ = false;
    TAS5825_LOG("TAS5825P: phase 1 done — parked (SR=%lu)", sampleRate_);
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

    // Serial audio port: FS/SCLK auto-detect, SAP word length matched
    // to the I2S TX width.
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_SIG_CH_CTRL, 0x00);
    writeRegister(REG_SDOUT_SEL,   0x00);
    writeRegister(REG_SAP_CTRL1,   SAP_CTRL1_FOR_BIT_DEPTH);

    // GPIO1 → FAULTZ output (Hybrid-Pro rewires it to HPFB on demand).
    writeRegister(REG_GPIO_CTRL, GPIO1_OE);
    writeRegister(REG_GPIO1_SEL, GPIO_SEL_FAULTZ);

    writeRegister(REG_DIG_VOL, currentVolume_);

    // P does not need an FS_MON gate before HIZ — the chip auto-detects
    // sample rate during the transition. Permissive flow. HIZ also
    // releases the DSP (DIS_DSP → 0).
    writeRegister(REG_DEVICE_CTRL2, CTRL_HIZ);
    SFX_DELAY_MS(5);

    // Analog gain auto-detected from the measured PVDD rail — read in
    // HiZ (analog powered) rather than in deep sleep where the ADC may
    // not sample.
    if (!configureAnalogGain()) return false;

    uint8_t fsMon = 0;
    readRegister(REG_FS_MON, &fsMon);
    TAS5825_LOG("TAS5825P: HIZ — FS_MON=0x%02X (%s)", fsMon, fsMonStr(fsMon));

    writeRegister(REG_DEVICE_CTRL2, CTRL_PLAY);
    SFX_DELAY_MS(5);

    uint8_t pwr = 0;
    readRegister(REG_POWER_STATE, &pwr);
    readRegister(REG_FS_MON, &fsMon);
    TAS5825_LOG("TAS5825P: PLAY — POWER_STATE=0x%02X FS_MON=0x%02X",
                pwr, fsMon);

    if ((fsMon & 0x0F) == FS_ERROR) {
        // Fallback the original P-flow used: bounce through DEEP_SLEEP
        // → PLAY to re-trigger auto-detect.
        TAS5825_LOG("TAS5825P: no FS lock via HIZ — bouncing through DEEP_SLEEP");
        writeRegister(REG_DEVICE_CTRL2, CTRL_DEEP_SLEEP);
        SFX_DELAY_MS(5);
        writeRegister(REG_DEVICE_CTRL2, CTRL_PLAY);
        SFX_DELAY_MS(5);
        readRegister(REG_POWER_STATE, &pwr);
    }

    writeRegister(REG_FAULT_CLEAR, FAULT_CLEAR_CMD);
    bool inPlay = (pwr == CTRL_PLAY);
    TAS5825_LOG("TAS5825P: %s (POWER_STATE=0x%02X)",
                inPlay ? "PLAY OK" : "PLAY FAILED", pwr);
    active_ = inPlay;
    return inPlay;
}

// ─── Runtime rail governor (contract: tas5825_m_codec.h) ─────────────────

namespace {
constexpr int      RETUNE_MIN_STEPS = 2;    // 1 dB hysteresis vs sag jitter
constexpr uint32_t MUTE_RAMP_MS     = 15;   // soft-ramp settle (no ramp-done reg)
}

void TAS5825PCodec::governRail(bool quiet) {
    if (!initialized_ || !i2c_) return;

    // Verify the OBSERVABLE before trusting the cached flag — a live battery
    // pull faults the stage out of PLAY and a new pack does not self-recover
    // (see the M twin for the bench trail).  Demote and let the retry path
    // re-activate once the rail reads plausible.
    if (active_) {
        uint8_t powerState = 0;
        selectBookPage(BOOK_00, PAGE_00);
        readRegister(REG_POWER_STATE, &powerState);
        if (powerState != CTRL_PLAY) {
            TAS5825_LOG("TAS5825P: dropped out of PLAY (POWER_STATE=0x%02X, "
                        "GLOBAL_FAULT1=0x%02X) — re-activating once the rail returns",
                        powerState, readFaultRegister());
            active_ = false;
            retuneCandidate_ = 0xFF;
            return;
        }
    }

    if (!active_) {
        // Boot-time rail was absent (pack plugged after boot) — retry the
        // full activate() once the ADC sees a plausible rail; it re-picks
        // AGAIN for whatever pack arrived.
        const uint32_t mv = readPvdd_mV();
        if (mv >= PVDD_MIN_VALID_MV) {
            TAS5825_LOG("TAS5825P: rail present (%lu.%02lu V) — retrying activation",
                        (unsigned long)(mv / 1000), (unsigned long)((mv % 1000) / 10));
            activate();
        }
        return;
    }

    const uint32_t mv = readPvdd_mV();
    if (mv < PVDD_MIN_VALID_MV) return;   // transient/unplug — UV fault path owns real loss
    const uint8_t step = againStepForPvdd_mV(mv);
    const int delta = (int)step - (int)againReg_;
    if (delta > -RETUNE_MIN_STEPS && delta < RETUNE_MIN_STEPS) {
        retuneCandidate_ = 0xFF;
        return;
    }
    if (retuneCandidate_ != step) {       // first sighting — confirm next pass
        retuneCandidate_ = step;
        return;
    }
    // Rail DROP (step > current) = clipping/UV risk → apply now; rail RISE
    // only raises loudness → wait for silence.
    if (step < againReg_ && !quiet) return;
    retuneCandidate_ = 0xFF;

    const uint16_t db10 = step * 5;
    TAS5825_LOG("TAS5825P: rail moved to %lu.%02lu V — AGAIN -%u.%u dB (was -%u.%u dB)%s",
                (unsigned long)(mv / 1000), (unsigned long)((mv % 1000) / 10),
                db10 / 10, db10 % 10,
                (againReg_ * 5) / 10, (againReg_ * 5) % 10,
                quiet ? "" : " [mute-wrapped]");
    selectBookPage(BOOK_00, PAGE_00);
    if (!quiet) {
        writeRegister(REG_DEVICE_CTRL2, CTRL_MUTE_BIT | CTRL_PLAY);
        SFX_DELAY_MS(MUTE_RAMP_MS);
    }
    againReg_ = step;
    writeRegister(REG_AGAIN, againReg_);
    if (!quiet) {
        writeRegister(REG_DEVICE_CTRL2, CTRL_PLAY);
    }
}

// ─── Volume / mute / reset ────────────────────────────────────────────────

void TAS5825PCodec::setVolume(float volume) {
    if (!initialized_) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    // [0..1] → [mute .. 0 dB], linear in dB (−0.5 dB per count above
    // the 0x30 reference — Table 9-24; gain above 0 dB not exposed).
    if (volume <= 0.0f) {
        currentVolume_ = VOL_MUTE;
    } else {
        int v = VOL_0DB + static_cast<int>((1.0f - volume) * 206.0f + 0.5f);
        currentVolume_ = (v > VOL_MIN) ? VOL_MIN : static_cast<uint8_t>(v);
    }
    if (!muted_) {
        selectBookPage(BOOK_00, PAGE_00);
        writeRegister(REG_DIG_VOL, currentVolume_);
    }
}

void TAS5825PCodec::setVolumeDB(float db) {
    if (!initialized_) return;
    currentVolume_ = volRegForDb(db);
    if (!muted_) {
        selectBookPage(BOOK_00, PAGE_00);
        writeRegister(REG_DIG_VOL, currentVolume_);
    }
}

void TAS5825PCodec::setMute(bool mute) {
    if (!initialized_) return;
    muted_ = mute;
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DIG_VOL, mute ? VOL_MUTE : currentVolume_);
}

void TAS5825PCodec::reset() {
    if (!initialized_) return;
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL2, CTRL_PARKED);
    writeRegister(REG_RESET_CTRL, 0x11);
    SFX_DELAY_MS(50);
    initialized_ = false;
    hybridProOn_ = false;
    if (begin(*i2c_, sdaPin_, sclPin_, sampleRate_)) {
        activate();
    }
}

// ─── Fault management ─────────────────────────────────────────────────────

bool TAS5825PCodec::clearFaults() {
    if (!initialized_) return false;
    selectBookPage(BOOK_00, PAGE_00);
    return writeRegister(REG_FAULT_CLEAR, FAULT_CLEAR_CMD);
}

uint8_t TAS5825PCodec::readFaultRegister() {
    if (!initialized_) return 0xFF;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    readRegister(REG_GLOBAL_FAULT1, &v);
    return v;
}

// ─── Hybrid-Pro / boost API (P-only, register addresses UNVERIFIED) ──────

bool TAS5825PCodec::hybridProEnable(bool on) {
    if (!initialized_) return false;
    selectBookPage(BOOK_00, PAGE_00);

    // Toggle GPIO1 between FAULTZ and HPFB (output stays enabled).
    writeRegister(REG_GPIO_CTRL, GPIO1_OE);
    writeRegister(REG_GPIO1_SEL, on ? GPIO_SEL_HPFB : GPIO_SEL_FAULTZ);

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
    uint16_t val = targetMv / 100;   // u8 in 100-mV units
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
        {REG_DEVICE_CTRL2,  "DEVICE_CTRL2"},
        {REG_POWER_STATE,   "POWER_STATE"},
        {REG_FS_MON,        "FS_MON"},
        {REG_CLKDET_STATUS, "CLKDET_STAT"},
        {REG_SAP_CTRL1,     "SAP_CTRL1"},
        {REG_DIG_VOL,       "DIG_VOL"},
        {REG_AGAIN,         "AGAIN"},
        {REG_PVDD_ADC,      "PVDD_ADC"},
        {REG_GPIO_CTRL,     "GPIO_CTRL"},
        {REG_GPIO1_SEL,     "GPIO1_SEL"},
        {REG_DIE_ID,        "DIE_ID"},
        {REG_CHAN_FAULT,    "CHAN_FAULT"},
        {REG_GLOBAL_FAULT1, "GLOBAL_FAULT1"},
        {REG_GLOBAL_FAULT2, "GLOBAL_FAULT2"},
        {REG_WARNING,       "WARNING"},
    };
    for (const auto& r : regs) {
        uint8_t v = 0;
        if (readRegister(r.reg, &v)) {
            TAS5825_LOG("  0x%02X %-13s : 0x%02X", r.reg, r.name, v);
        }
    }
}

uint8_t TAS5825PCodec::getDeviceControlRegister() {
    if (!initialized_) return 0xFF;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    readRegister(REG_DEVICE_CTRL2, &v);
    return v;
}

uint8_t TAS5825PCodec::getFaultRegister() {
    return readFaultRegister();
}

bool TAS5825PCodec::testI2CConnection() {
    if (!i2c_) return false;
    return i2c_->probe(I2C_ADDR);
}

// ─── Private helpers ──────────────────────────────────────────────────────

bool TAS5825PCodec::writeRegister(uint8_t reg, uint8_t value) {
    if (!i2c_) return false;
    return i2c_->writeReg(I2C_ADDR, reg, &value, 1);
}

bool TAS5825PCodec::readRegister(uint8_t reg, uint8_t* value) {
    if (!i2c_ || !value) return false;
    return i2c_->readReg(I2C_ADDR, reg, value, 1);
}

bool TAS5825PCodec::selectBookPage(uint8_t book, uint8_t page) {
    if (!writeRegister(REG_PAGE, page)) return false;
    if (!writeRegister(REG_BOOK, book)) return false;
    return true;
}

uint32_t TAS5825PCodec::readPvdd_mV() {
    if (!i2c_) return 0;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t adc = 0;
    if (!readRegister(REG_PVDD_ADC, &adc)) return 0;
    return pvddMvFromAdc(adc);
}

bool TAS5825PCodec::configureAnalogGain() {
    // Auto-detect: measure the actual PVDD rail and pick the largest
    // analog gain whose full-scale output still fits under it. A
    // reading below the chip's 4.5 V operating floor means the ADC
    // isn't valid (or the rail is absent) — fall back to the safe
    // −8 dB setting.
    uint32_t mv = readPvdd_mV();
    if (mv < PVDD_MIN_VALID_MV) {
        againReg_ = AGAIN_FALLBACK;
        TAS5825_LOG("TAS5825P: PVDD ADC implausible (%lu mV) — AGAIN "
                    "fallback -8.0 dB", (unsigned long)mv);
    } else {
        againReg_ = againStepForPvdd_mV(mv);
        uint16_t db10 = againReg_ * 5;   // step = 0.5 dB
        TAS5825_LOG("TAS5825P: PVDD %lu.%02lu V -> AGAIN -%u.%u dB "
                    "(full-scale %u.%02u Vpeak)",
                    (unsigned long)(mv / 1000), (unsigned long)((mv % 1000) / 10),
                    db10 / 10, db10 % 10,
                    AGAIN_FULLSCALE_MV[againReg_] / 1000,
                    (AGAIN_FULLSCALE_MV[againReg_] % 1000) / 10);
    }
    selectBookPage(BOOK_00, PAGE_00);
    return writeRegister(REG_AGAIN, againReg_);
}

#endif // SFX_HAS_AUDIO
