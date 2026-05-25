/*
 * audio_codec.cpp — TAS5825P bring-up (test firmware).
 *
 * Register sequence is the production HubFX flow with everything
 * non-essential stripped out (Hybrid-Pro, fault decoding, EQ).
 * Targeted at 12 V PVDD supply (HubFX rev) — flip `kAnalogGain12V` if
 * running on a different supply rail.
 */

#include "audio_codec.h"

namespace {
// ── I²C address ───────────────────────────────────────────────────────
constexpr uint8_t kI2cAddr            = 0x4C;

// ── Page/Book selectors ───────────────────────────────────────────────
constexpr uint8_t kRegPage            = 0x00;
constexpr uint8_t kRegBook            = 0x7F;
constexpr uint8_t kBook00             = 0x00;
constexpr uint8_t kPage00             = 0x00;

// ── Core control registers (Book 0, Page 0) ──────────────────────────
constexpr uint8_t kRegReset           = 0x01;  // [4]=DSP, [0]=registers
constexpr uint8_t kRegModeCtrl        = 0x02;
constexpr uint8_t kRegDeviceCtrl      = 0x03;
constexpr uint8_t kRegSdoutSel        = 0x30;
constexpr uint8_t kRegClkSrc          = 0x33;
constexpr uint8_t kRegFsRate          = 0x34;
constexpr uint8_t kRegFsMon           = 0x37;
constexpr uint8_t kRegAnalogCtrl      = 0x46;
constexpr uint8_t kRegDigitalVol      = 0x4C;
constexpr uint8_t kRegAgainL          = 0x53;
constexpr uint8_t kRegAgainR          = 0x54;
constexpr uint8_t kRegSapCtrl1        = 0x60;
constexpr uint8_t kRegDspMisc         = 0x62;
constexpr uint8_t kRegPowerState      = 0x68;
constexpr uint8_t kRegFaultClear      = 0x78;

// ── DEVICE_CTRL mode values ──────────────────────────────────────────
constexpr uint8_t kCtrlDeepSleep      = 0x00;
constexpr uint8_t kCtrlHiZ            = 0x02;
constexpr uint8_t kCtrlPlay           = 0x03;
constexpr uint8_t kCtrlMuteBit        = 0x08;

// ── HubFX 12 V supply → analog gain register value ───────────────────
// From production tas5825_regs.h: AGAIN_12V = 0x10 → -8.0 dB, 11.74 Vpeak
constexpr uint8_t kAnalogGain12V      = 0x10;

// ── SAP_CTRL1 — 16-bit I²S word ──────────────────────────────────────
// bits[1:0] = 00 → 16-bit; bits[3:2] = 00 → standard I²S
constexpr uint8_t kSapCtrl1_16BitI2S  = 0x00;

// ── Volume scale ──────────────────────────────────────────────────────
// 0x30 (48) = 0 dB, 0x00 = +24 dB, 0xFF = mute (-103 dB)
constexpr uint8_t kVolume0dB          = 0x30;

}  // namespace


// ── I²C helpers ───────────────────────────────────────────────────────

bool AudioCodec::writeReg(uint8_t reg, uint8_t val) {
    if (!_i2c) return false;
    _i2c->beginTransmission(kI2cAddr);
    _i2c->write(reg);
    _i2c->write(val);
    return _i2c->endTransmission() == 0;
}

bool AudioCodec::readReg(uint8_t reg, uint8_t& val) {
    if (!_i2c) return false;
    _i2c->beginTransmission(kI2cAddr);
    _i2c->write(reg);
    if (_i2c->endTransmission(false) != 0) return false;
    if (_i2c->requestFrom((int)kI2cAddr, 1) != 1) return false;
    val = _i2c->read();
    return true;
}

// Helper: select Book + Page (every register block read/write should
// first select page 0 to be safe — only Book 0 / Page 0 used here).
static bool selectBookPage(TwoWire& wire, uint8_t book, uint8_t page) {
    // Per TAS5825 datasheet: write PAGE first, then BOOK.
    wire.beginTransmission(0x4C); wire.write(kRegPage); wire.write(page);
    if (wire.endTransmission() != 0) return false;
    wire.beginTransmission(0x4C); wire.write(kRegBook); wire.write(book);
    if (wire.endTransmission() != 0) return false;
    return true;
}


// ── Phase 1: probe + reset → DEEP_SLEEP ──────────────────────────────

bool AudioCodec::begin(TwoWire& wire) {
    _i2c = &wire;
    _i2c->setClock(100000);   // 100 kHz — TAS5825 max is 400 kHz, 100 is safe

    // Probe — empty write to address.  Retry a few times in case the
    // chip is still POR-resetting after a hard reset.
    Serial.printf("[Codec] probing TAS5825P @ 0x%02X ...\n", kI2cAddr);
    uint8_t err = 1;
    for (int attempt = 0; attempt < 3; ++attempt) {
        _i2c->beginTransmission(kI2cAddr);
        err = _i2c->endTransmission();
        if (err == 0) break;
        delay(500);
    }
    if (err != 0) {
        Serial.printf("[Codec] probe FAILED (i2c err=%u)\n", err);
        _initialized = false;
        return false;
    }
    Serial.printf("[Codec] probe OK\n");

    // Reset.  Bit [4]=DSP reset, bit [0]=registers reset → 0x11.
    selectBookPage(*_i2c, kBook00, kPage00);
    writeReg(kRegReset, 0x11);
    delay(5);

    // Enter DEEP_SLEEP — PLL off; safe before I²S clocks are running.
    writeReg(kRegDeviceCtrl, kCtrlDeepSleep);
    delay(5);

    _initialized = true;
    Serial.printf("[Codec] phase 1 done — in DEEP_SLEEP\n");
    return true;
}


// ── Phase 2: configure + HIZ → PLAY ──────────────────────────────────

bool AudioCodec::activate(uint32_t sampleRate) {
    if (!_initialized || !_i2c) {
        Serial.printf("[Codec] activate() before begin() — refusing\n");
        return false;
    }
    Serial.printf("[Codec] phase 2 — configuring for %u Hz\n", (unsigned)sampleRate);

    // Analog gain for 12 V supply (HubFX rev).
    selectBookPage(*_i2c, kBook00, kPage00);
    writeReg(kRegAnalogCtrl, 0x11);
    writeReg(kRegModeCtrl,   0x00);
    writeReg(kRegAgainL,     0x01);
    writeReg(kRegAgainR,     kAnalogGain12V);

    // Clock-source: auto-detect from BCLK / LRCK
    writeReg(kRegClkSrc,   0x00);
    writeReg(kRegFsRate,   0x00);
    writeReg(kRegSdoutSel, 0x00);

    // SAP — 16-bit standard I²S
    writeReg(kRegSapCtrl1, kSapCtrl1_16BitI2S);
    writeReg(kRegDspMisc,  0x09);   // default DSP mode

    // Digital volume (production starts at 0 dB).
    writeReg(kRegDigitalVol, _volume);

    // HIZ — PLL begins lock attempt on the running I²S clocks.
    writeReg(kRegDeviceCtrl, kCtrlHiZ);
    delay(5);

    uint8_t fsMon = 0;
    readReg(kRegFsMon, fsMon);
    Serial.printf("[Codec] HIZ — FS_MON=0x%02X\n", fsMon);

    // PLAY.
    writeReg(kRegDeviceCtrl, kCtrlPlay);
    delay(5);

    uint8_t pwr = 0;
    readReg(kRegPowerState, pwr);
    readReg(kRegFsMon, fsMon);
    Serial.printf("[Codec] PLAY — POWER_STATE=0x%02X FS_MON=0x%02X\n", pwr, fsMon);

    // Fallback: if FS_MON didn't lock, bounce through DEEP_SLEEP.
    if (fsMon == 0) {
        Serial.printf("[Codec] PLL not locked — bouncing through DEEP_SLEEP\n");
        writeReg(kRegDeviceCtrl, kCtrlDeepSleep);
        delay(5);
        writeReg(kRegDeviceCtrl, kCtrlPlay);
        delay(5);
        readReg(kRegPowerState, pwr);
    }

    writeReg(kRegFaultClear, 0x80);   // clear any latched faults
    _playing = (pwr & 0x0F) == kCtrlPlay;
    Serial.printf("[Codec] %s (POWER_STATE=0x%02X)\n",
                  _playing ? "PLAY ✓" : "PLAY FAILED", pwr);
    return _playing;
}


// ── Volume / mute ────────────────────────────────────────────────────

void AudioCodec::setVolumeDb(float volume_db) {
    if (!_initialized) return;
    if (volume_db < -100.0f) volume_db = -100.0f;
    if (volume_db >   24.0f) volume_db =   24.0f;
    // 0.5 dB per step; 0 dB = 48
    int v = (int)((volume_db / 0.5f) + 48.0f);
    if (v < 0) v = 0;
    if (v > 0xFE) v = 0xFE;
    _volume = (uint8_t)v;
    selectBookPage(*_i2c, kBook00, kPage00);
    writeReg(kRegDigitalVol, _volume);
}

void AudioCodec::setMute(bool mute) {
    if (!_initialized) return;
    selectBookPage(*_i2c, kBook00, kPage00);
    writeReg(kRegDeviceCtrl,
             mute ? (kCtrlPlay | kCtrlMuteBit) : kCtrlPlay);
}
