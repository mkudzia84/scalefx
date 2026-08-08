/**
 * @file tas5825_m_codec.cpp
 * @brief Implementation of the TAS5825M (smart-amp variant) driver.
 *
 * Register addresses, bit fields, and state machine follow the TI
 * TAS5825M datasheet SLASEH7H (the gold standard — see tas5825_regs.h).
 * The M-specific guard rails are load-bearing:
 *   - DIS_DSP (DEVICE_CTRL2[4]) stays SET until I2S clocks are stable
 *     (datasheet: clearing it early desyncs the DSP DMA channels);
 *   - FAULT_CLEAR before config, after config, and after PLAY;
 *   - FS_MON polled for a valid rate code before HIZ → PLAY;
 *   - GPIO1 routed to FAULTZ (0x0B) WITH its output enable in GPIO_CTRL.
 * Bench mirror: tests/hw/tas5825m_beep.
 */

#if defined(SFX_HAS_AUDIO)

#include "tas5825_m_codec.h"
#include "tas5825_regs.h"
#include "../audio/audio_log.h"
#include "../audio/audio_config.h"   // AUDIO_BIT_DEPTH
#include "platform/sfx_platform.h"

namespace {

using namespace sfx_audio::tas5825;

// SAP_CTRL_1 (0x33) value picked from AUDIO_BIT_DEPTH at compile time.
// Bits [5:4] = 00 keep I2S (Philips) format; [1:0] = word length, which
// MUST match the I2S TX bit width (silicon reset default is 24-bit).
constexpr uint8_t SAP_CTRL1_FOR_BIT_DEPTH =
    (AUDIO_BIT_DEPTH == 32) ? SAP_WORD_32BIT :
    (AUDIO_BIT_DEPTH == 24) ? SAP_WORD_24BIT :
                              SAP_WORD_16BIT;

// FS_MON polling cadence used during activate().
constexpr uint32_t FS_MON_POLL_INTERVAL_MS = 5;
constexpr uint32_t FS_MON_POLL_TIMEOUT_MS  = 500;

// State-transition polling: every wait in activate() with an observable
// register behind it (FS lock, PVDD ADC sample, POWER_STATE) is a bounded
// poll of that observable, NOT a blind settle delay — converges as fast as
// the silicon allows and times out only on a real fault.  Blind delays are
// reserved for datasheet-mandated settle times with nothing to observe
// (the post-reset 50 ms).
constexpr uint32_t STATE_POLL_INTERVAL_MS   = 10;
constexpr uint32_t PVDD_ADC_POLL_TIMEOUT_MS = 150;
constexpr uint32_t PLAY_POLL_TIMEOUT_MS     = 200;

// Rail governor: ignore rail moves smaller than 2 AGAIN steps (1 dB) —
// normal 4S sag under load is well inside that; a pack swap is not.
// The soft volume ramp is a fixed-rate blind settle (no ramp-done
// register to observe) — the one legitimate delay in the retune path.
constexpr int      RETUNE_MIN_STEPS = 2;
constexpr uint32_t MUTE_RAMP_MS     = 15;

// Poll `pred` every interval_ms until it returns true or timeout_ms
// elapses.  First check is immediate — a condition that already holds
// costs no delay.
template <typename Pred>
static bool pollUntil(Pred&& pred, uint32_t timeout_ms, uint32_t interval_ms) {
    for (uint32_t elapsed = 0;; elapsed += interval_ms) {
        if (pred()) return true;
        if (elapsed >= timeout_ms) return false;
        SFX_DELAY_MS(interval_ms);
    }
}

// DEVICE_CTRL2 parking value while clocks are absent: DSP held in
// reset + soft mute + deep sleep.
constexpr uint8_t CTRL_PARKED = CTRL_DIS_DSP_BIT | CTRL_MUTE_BIT | CTRL_DEEP_SLEEP;

}  // namespace

// ─── Construction ─────────────────────────────────────────────────────────

TAS5825MCodec::TAS5825MCodec() = default;

// ─── Phase 1: pre-clock init ──────────────────────────────────────────────

bool TAS5825MCodec::begin(sfx_peripherals::SfxI2cBus& wire, int sda, int scl,
                          uint32_t sample_rate) {
    bool needWireInit = (i2c_ != &wire || sdaPin_ != sda || sclPin_ != scl);

    i2c_       = &wire;
    sdaPin_    = sda;
    sclPin_    = scl;
    sampleRate_ = sample_rate;

    if (needWireInit) {
        i2c_->begin(sdaPin_, sclPin_, 100000);   // native bus bring-up (100 kHz)
    }

    TAS5825_LOG("TAS5825M: probing @ 0x%02X (SDA=%d SCL=%d)...",
                I2C_ADDR, sdaPin_, sclPin_);

    // I2C probe with retry — bus may be transiently unavailable post-boot.
    constexpr int PROBE_RETRIES = 3;
    constexpr int PROBE_DELAY_MS = 500;
    bool probeOk = false;
    for (int attempt = 0; attempt < PROBE_RETRIES; attempt++) {
        probeOk = i2c_->probe(I2C_ADDR);
        if (probeOk) break;
        if (attempt < PROBE_RETRIES - 1) {
            TAS5825_LOG("  probe %d/%d failed, retry in %dms",
                        attempt + 1, PROBE_RETRIES, PROBE_DELAY_MS);
            SFX_DELAY_MS(PROBE_DELAY_MS);
        }
    }
    if (!probeOk) {
        TAS5825_LOG("TAS5825M: probe FAILED — check wiring/DVDD");
        initialized_ = false;
        return false;
    }

    // Identity: DIE_ID reads 0x95 on TAS5825M silicon.
    selectBookPage(BOOK_00, PAGE_00);
    dieId_ = 0;
    readRegister(REG_DIE_ID, &dieId_);
    TAS5825_LOG("TAS5825M: probe OK, DIE_ID=0x%02X (%s)",
                dieId_, dieId_ == DIE_ID_TAS5825M ? "TAS5825M" : "UNEXPECTED");

    // Phase 1: park with the DSP held in reset (DIS_DSP must stay set
    // until I2S clocks are stable), full reset, re-park, clear the
    // boot-time clock-fault latch (clocks were absent at power-up).
    writeRegister(REG_DEVICE_CTRL2, CTRL_PARKED);
    SFX_DELAY_MS(5);

    writeRegister(REG_RESET_CTRL, 0x11);   // RST_DIG_CORE + RST_REG
    SFX_DELAY_MS(50);

    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL2, CTRL_PARKED);
    writeRegister(REG_FAULT_CLEAR, FAULT_CLEAR_CMD);
    SFX_DELAY_MS(5);

    initialized_ = true;
    smartAmpOn_  = false;

    TAS5825_LOG("TAS5825M: phase 1 done — parked (DSP held), fault latch "
                "cleared (SR=%lu)", sampleRate_);
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

    // Step 1: serial audio port. FS/SCLK detection is automatic
    // (SIG_CH_CTRL FSMODE=0, CLOCK_DET_CTRL default); the one field
    // that must match the wire is the SAP word length.
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_SIG_CH_CTRL, 0x00);
    writeRegister(REG_SDOUT_SEL, 0x00);
    writeRegister(REG_SAP_CTRL1, SAP_CTRL1_FOR_BIT_DEPTH);

    // Step 3: route GPIO1 to FAULTZ so the fault line reflects real
    // codec faults. GPIO_SEL alone is not enough — the pin also needs
    // its output enable in GPIO_CTRL. (On the M, GPIO_SEL value 0x01
    // is reserved — Table 9-42.)
    writeRegister(REG_GPIO_CTRL, GPIO1_OE);
    writeRegister(REG_GPIO1_SEL, GPIO_SEL_FAULTZ);

    // Step 4: DSP stays on silicon defaults — ROM mode 1 with ZROM
    // coefficients (DSP_PGM_MODE/DSP_CTRL reset values), which is the
    // documented pass-through configuration. Custom coefficients are
    // a PPC3 export and belong to the smart-amp pipeline.

    writeRegister(REG_DIG_VOL, currentVolume_);

    // Step 5: mid-config fault clear (config writes can tickle the
    // clock-fault latch as auto-detect re-evaluates).
    writeRegister(REG_FAULT_CLEAR, FAULT_CLEAR_CMD);
    SFX_DELAY_MS(5);

    // Step 6: FS_MON gate — wait for a valid sample-rate code BEFORE
    // releasing the DSP and transitioning to PLAY. 48 kHz reports 0x09
    // (Table 9-19).
    //
    // The chip must be brought OUT of deep sleep first: FS_MON/CLKDET
    // do not sample in the parked (deep-sleep) state phase 1 left it in
    // — the gate then times out with CLKDET_STATUS=0x00 even though the
    // I2S clocks are clean (bench 9C6C, every boot).  HiZ with DIS_DSP
    // still held + muted wakes the serial-port clock detector while the
    // DSP stays safely in reset until the lock is proven.
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL2, CTRL_DIS_DSP_BIT | CTRL_MUTE_BIT | CTRL_HIZ);
    // No settle delay — waitForFsLock IS the wait; it polls the observable.

    uint8_t fsCode = 0;
    if (!waitForFsLock(FS_MON_POLL_TIMEOUT_MS, &fsCode)) {
        uint8_t clkStat = 0;
        readRegister(REG_CLKDET_STATUS, &clkStat);
        TAS5825_LOG("TAS5825M: FS_MON never locked (CLKDET_STATUS=0x%02X) — "
                    "check BCLK/LRCLK wiring", clkStat);
        return false;
    }
    TAS5825_LOG("TAS5825M: FS_MON locked at %s (code 0x%02X)",
                fsMonStr(fsCode), fsCode);

    // Step 7: HIZ with the DSP released (DIS_DSP → 0 now that clocks
    // are proven stable), still soft-muted.
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL2, CTRL_MUTE_BIT | CTRL_HIZ);

    // Step 7b: analog gain auto-detected from the measured PVDD rail —
    // read here (HiZ, analog powered) rather than in deep sleep where
    // the ADC may not sample.  configureAnalogGain polls the ADC until
    // it produces a plausible sample, so no settle delay here either.
    if (!configureAnalogGain()) {
        TAS5825_LOG("TAS5825M: analog-gain config failed");
        return false;
    }

    // Step 8: HIZ → PLAY, un-muted, gated on the observable POWER_STATE.
    // The modulator's inrush can briefly assert PVDD_UV and latch it, so
    // each poll pass re-clears the fault latch before reading the state —
    // the retry IS the recovery, no blind post-PLAY settle.
    writeRegister(REG_DEVICE_CTRL2, CTRL_PLAY);
    uint8_t powerState = 0;
    const bool inPlay = pollUntil([&] {
        writeRegister(REG_FAULT_CLEAR, FAULT_CLEAR_CMD);
        readRegister(REG_POWER_STATE, &powerState);
        return powerState == CTRL_PLAY;
    }, PLAY_POLL_TIMEOUT_MS, STATE_POLL_INTERVAL_MS);

    if (inPlay) {
        TAS5825_LOG("TAS5825M: PLAY OK (FS=%s)", fsMonStr(fsCode));
    } else {
        uint8_t g1 = 0, chf = 0;
        readRegister(REG_GLOBAL_FAULT1, &g1);
        readRegister(REG_CHAN_FAULT, &chf);
        TAS5825_LOG("TAS5825M: PLAY FAILED — POWER_STATE=0x%02X (%s) "
                    "GLOBAL_FAULT1=0x%02X CHAN_FAULT=0x%02X",
                    powerState, powerStateStr(powerState), g1, chf);
    }
    active_ = inPlay;
    return inPlay;
}

// ─── Volume / mute / reset ────────────────────────────────────────────────

void TAS5825MCodec::setVolume(float volume) {
    if (!initialized_) return;

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    // Map [0..1] linearly in dB onto [mute .. 0 dB]. 0x4C counts are
    // −0.5 dB each above the 0x30 = 0 dB reference (Table 9-24);
    // gain above 0 dB is deliberately not exposed here.
    if (volume <= 0.0f) {
        currentVolume_ = VOL_MUTE;
    } else {
        int v = VOL_0DB + static_cast<int>((1.0f - volume) * 206.0f + 0.5f);
        currentVolume_ = (v > VOL_MIN) ? VOL_MIN : static_cast<uint8_t>(v);
    }

    if (!muted_) {
        selectBookPage(BOOK_00, PAGE_00);
        writeRegister(REG_DIG_VOL, currentVolume_);
        TAS5825_LOG("TAS5825M: vol = %.0f%% (0x%02X)",
                    volume * 100.0f, currentVolume_);
    }
}

void TAS5825MCodec::setVolumeDB(float db) {
    if (!initialized_) return;
    currentVolume_ = volRegForDb(db);
    if (!muted_) {
        selectBookPage(BOOK_00, PAGE_00);
        writeRegister(REG_DIG_VOL, currentVolume_);
        TAS5825_LOG("TAS5825M: vol = %.1fdB (0x%02X)", db, currentVolume_);
    }
}

void TAS5825MCodec::setMute(bool mute) {
    if (!initialized_) return;
    muted_ = mute;
    selectBookPage(BOOK_00, PAGE_00);
    if (mute) {
        writeRegister(REG_DIG_VOL, VOL_MUTE);
        TAS5825_LOG("TAS5825M: muted");
    } else {
        writeRegister(REG_DIG_VOL, currentVolume_);
        TAS5825_LOG("TAS5825M: unmuted");
    }
}

void TAS5825MCodec::reset() {
    if (!initialized_) return;
    TAS5825_LOG("TAS5825M: reset");
    selectBookPage(BOOK_00, PAGE_00);
    writeRegister(REG_DEVICE_CTRL2, CTRL_PARKED);
    writeRegister(REG_RESET_CTRL, 0x11);
    SFX_DELAY_MS(50);

    initialized_ = false;
    smartAmpOn_  = false;
    if (begin(*i2c_, sdaPin_, sclPin_, sampleRate_)) {
        activate();
    }
}

// ─── Fault management ─────────────────────────────────────────────────────

bool TAS5825MCodec::clearFaults() {
    if (!initialized_) return false;
    selectBookPage(BOOK_00, PAGE_00);
    return writeRegister(REG_FAULT_CLEAR, FAULT_CLEAR_CMD);
}

uint8_t TAS5825MCodec::readFaultRegister() {
    if (!initialized_) return 0xFF;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    readRegister(REG_GLOBAL_FAULT1, &v);
    return v;
}

// ─── Smart-amp / IV-sense API (M-only) ────────────────────────────────────
//
// The smart-amp DSP lives in coefficient books that require a PPC3
// (TI PurePath Console) export per speaker model — there is no
// documented "enable bit" in the book-0 register map. Until that
// pipeline exists these methods only track intent; they deliberately
// write NO registers (the datasheet-unlisted writes the old driver
// did here landed on unrelated registers).

bool TAS5825MCodec::smartAmpEnable(bool on) {
    if (!initialized_) {
        TAS5825_LOG("TAS5825M: smartAmpEnable(%d) — not initialized", on);
        return false;
    }
    smartAmpOn_ = on;
    TAS5825_LOG("TAS5825M: smart-amp %s (stub — needs PPC3 coefficient "
                "pipeline; factory protection thresholds remain active)",
                on ? "requested" : "off");
    return true;
}

bool TAS5825MCodec::calibrateSpeaker() {
    if (!initialized_) {
        TAS5825_LOG("TAS5825M: calibrateSpeaker — not initialized");
        return false;
    }
    TAS5825_LOG("TAS5825M: calibrateSpeaker (stub — using factory defaults)");
    return true;
}

int16_t TAS5825MCodec::readSpeakerTemp_C() {
    if (!initialized_ || !smartAmpOn_) return 0;
    return 0;   // needs the PPC3 pipeline (indirect DSP-RAM reads)
}

uint16_t TAS5825MCodec::readSpeakerExcursion_mm100() {
    if (!initialized_ || !smartAmpOn_) return 0;
    return 0;   // see readSpeakerTemp_C()
}

bool TAS5825MCodec::setIvSenseEnabled(bool on) {
    if (!initialized_) return false;
    TAS5825_LOG("TAS5825M: IV-sense %s (stub)", on ? "enable" : "disable");
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
        {REG_DEVICE_CTRL2,  "DEVICE_CTRL2"},
        {REG_POWER_STATE,   "POWER_STATE"},
        {REG_FS_MON,        "FS_MON"},
        {REG_BCK_MON,       "BCK_MON"},
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

uint8_t TAS5825MCodec::getDeviceControlRegister() {
    if (!initialized_) return 0xFF;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t v = 0;
    readRegister(REG_DEVICE_CTRL2, &v);
    return v;
}

uint8_t TAS5825MCodec::getFaultRegister() {
    return readFaultRegister();
}

bool TAS5825MCodec::testI2CConnection() {
    if (!i2c_) return false;
    return i2c_->probe(I2C_ADDR);
}

// ─── Private helpers ──────────────────────────────────────────────────────

bool TAS5825MCodec::writeRegister(uint8_t reg, uint8_t value) {
    if (!i2c_) return false;
    return i2c_->writeReg(I2C_ADDR, reg, &value, 1);
}

bool TAS5825MCodec::readRegister(uint8_t reg, uint8_t* value) {
    if (!i2c_ || !value) return false;
    return i2c_->readReg(I2C_ADDR, reg, value, 1);
}

bool TAS5825MCodec::selectBookPage(uint8_t book, uint8_t page) {
    if (!writeRegister(REG_PAGE, page)) return false;
    if (!writeRegister(REG_BOOK, book)) return false;
    return true;
}

bool TAS5825MCodec::waitForFsLock(uint32_t timeoutMs, uint8_t* outFsCode) {
    for (uint32_t elapsed = 0;; elapsed += FS_MON_POLL_INTERVAL_MS) {
        uint8_t fs = 0;
        readRegister(REG_FS_MON, &fs);
        fs &= 0x0F;
        if (fs != FS_ERROR) {
            if (outFsCode) *outFsCode = fs;
            return true;
        }
        if (elapsed >= timeoutMs) {
            if (outFsCode) *outFsCode = fs;
            return false;
        }
        SFX_DELAY_MS(FS_MON_POLL_INTERVAL_MS);
    }
}

uint32_t TAS5825MCodec::readPvdd_mV() {
    if (!i2c_) return 0;
    selectBookPage(BOOK_00, PAGE_00);
    uint8_t adc = 0;
    if (!readRegister(REG_PVDD_ADC, &adc)) return 0;
    return pvddMvFromAdc(adc);
}

bool TAS5825MCodec::configureAnalogGain() {
    // Auto-detect: measure the actual PVDD rail and pick the largest
    // analog gain whose full-scale output still fits under it. A
    // reading below the chip's 4.5 V operating floor means the ADC
    // isn't valid (or the rail is absent) — fall back to the safe
    // −8 dB setting.
    // The PVDD ADC may not have produced a sample yet this early after
    // deep sleep (bench 9C6C: first read implausible on every boot, while
    // the same register reads 16 V moments later) — poll the observable
    // until a plausible sample lands before concluding the rail is absent.
    uint32_t mv = 0;
    const bool plausible = pollUntil([&] {
        mv = readPvdd_mV();
        return mv >= PVDD_MIN_VALID_MV;
    }, PVDD_ADC_POLL_TIMEOUT_MS, STATE_POLL_INTERVAL_MS);
    if (!plausible) {
        againReg_ = AGAIN_FALLBACK;
        TAS5825_LOG("TAS5825M: PVDD ADC implausible (%lu mV) — AGAIN "
                    "fallback -8.0 dB", (unsigned long)mv);
    } else {
        againReg_ = againStepForPvdd_mV(mv);
        uint16_t db10 = againReg_ * 5;   // step = 0.5 dB
        TAS5825_LOG("TAS5825M: PVDD %lu.%02lu V -> AGAIN -%u.%u dB "
                    "(full-scale %u.%02u Vpeak)",
                    (unsigned long)(mv / 1000), (unsigned long)((mv % 1000) / 10),
                    db10 / 10, db10 % 10,
                    AGAIN_FULLSCALE_MV[againReg_] / 1000,
                    (AGAIN_FULLSCALE_MV[againReg_] % 1000) / 10);
    }
    selectBookPage(BOOK_00, PAGE_00);
    return writeRegister(REG_AGAIN, againReg_);
}

void TAS5825MCodec::governRail(bool quiet) {
    if (!initialized_ || !i2c_) return;

    // Verify the OBSERVABLE before trusting the cached flag: a LIVE battery
    // pull faults the output stage (PVDD_UV → global fault) and the chip
    // drops out of PLAY on its own — a fresh pack does NOT bring it back.
    // Demote to inactive; the retry path below re-activates (full re-config,
    // gain re-picked for the NEW pack, fault latch cleared by the PLAY poll)
    // as soon as the rail reads plausible again.  Bench 9C6C 2026-08-07:
    // live pull → GLOBAL_FAULT, different pack replugged, no recovery until
    // this check existed.
    if (active_) {
        uint8_t powerState = 0;
        selectBookPage(BOOK_00, PAGE_00);
        readRegister(REG_POWER_STATE, &powerState);
        if (powerState != CTRL_PLAY) {
            TAS5825_LOG("TAS5825M: dropped out of PLAY (POWER_STATE=0x%02X %s, "
                        "GLOBAL_FAULT1=0x%02X) — re-activating once the rail returns",
                        powerState, powerStateStr(powerState), readFaultRegister());
            active_ = false;
            retuneCandidate_ = 0xFF;
            return;   // next pass retries activation against the new rail
        }
    }

    // (a) Activation never reached PLAY — the boot-time rail was absent
    // (bench USB power, pack plugged later).  Retry the full activate()
    // once the ADC sees a plausible rail; activate() re-picks AGAIN for
    // whatever pack arrived.
    if (!active_) {
        const uint32_t mv = readPvdd_mV();
        if (mv >= PVDD_MIN_VALID_MV) {
            TAS5825_LOG("TAS5825M: rail present (%lu.%02lu V) — retrying activation",
                        (unsigned long)(mv / 1000), (unsigned long)((mv % 1000) / 10));
            activate();
        }
        return;
    }

    // (b) In PLAY — track the live rail against the current AGAIN step.
    const uint32_t mv = readPvdd_mV();
    if (mv < PVDD_MIN_VALID_MV) return;   // transient/unplug — UV fault path owns real loss
    const uint8_t step = againStepForPvdd_mV(mv);
    const int delta = (int)step - (int)againReg_;
    if (delta > -RETUNE_MIN_STEPS && delta < RETUNE_MIN_STEPS) {
        retuneCandidate_ = 0xFF;          // inside hysteresis — drop any candidate
        return;
    }
    if (retuneCandidate_ != step) {       // first sighting — confirm next pass
        retuneCandidate_ = step;
        return;
    }
    // step > againReg_ = rail DROPPED (needs more attenuation): clipping/UV
    // risk, apply now.  step < againReg_ = rail ROSE: loudness-only change,
    // wait for silence so the level never steps mid-effect.
    if (step < againReg_ && !quiet) return;
    retuneCandidate_ = 0xFF;

    const uint16_t db10 = step * 5;
    TAS5825_LOG("TAS5825M: rail moved to %lu.%02lu V — AGAIN -%u.%u dB "
                "(was -%u.%u dB)%s",
                (unsigned long)(mv / 1000), (unsigned long)((mv % 1000) / 10),
                db10 / 10, db10 % 10,
                (againReg_ * 5) / 10, (againReg_ * 5) % 10,
                quiet ? "" : " [mute-wrapped]");
    selectBookPage(BOOK_00, PAGE_00);
    if (!quiet) {                          // audible: wrap in the soft ramp
        writeRegister(REG_DEVICE_CTRL2, CTRL_MUTE_BIT | CTRL_PLAY);
        SFX_DELAY_MS(MUTE_RAMP_MS);
    }
    againReg_ = step;
    writeRegister(REG_AGAIN, againReg_);
    if (!quiet) {
        writeRegister(REG_DEVICE_CTRL2, CTRL_PLAY);
    }
}

#endif // SFX_HAS_AUDIO
