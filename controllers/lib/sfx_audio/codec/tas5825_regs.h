/**
 * @file tas5825_regs.h
 * @brief Shared register definitions for the TI TAS5825 audio amplifier
 *        family — used by both the M-variant and P-variant drivers.
 *
 * GOLD STANDARD: the TI TAS5825M datasheet **SLASEH7H** (Oct 2019, rev
 * Jan 2023), section 9.6.1 "CONTROL PORT Registers", Table 9-6. Every
 * address and bit constant below was audited against that table on
 * 2026-07-30. The previous revision of this header carried a folk map
 * that diverged on six registers (SAP_CTRL1, GPIO1_SEL, ANA_CTRL/AGAIN,
 * CLK/FS regs, DSP regs), an inverted DIG_VOL scale (0x00 is **+24 dB
 * full gain**, NOT mute — 0xFF is mute), and a fabricated FS_MON code
 * table (48 kHz reports **0x09**, not 0x04). Do not "fix" values here
 * from memory — cite the datasheet section in the commit.
 *
 * Variant-specific registers / bits are defined in each variant's
 * header: tas5825_p_codec.h (Hybrid-Pro/Class-H) and tas5825_m_codec.h
 * (smart-amp/IV-sense, restored 2026-07-30 — bench boards turned out
 * to carry M silicon despite the BOM's P).
 *
 * Reference: TI TAS5825M datasheet SLASEH7H; SLAA846 (advanced
 *            features); TI process-flow app notes for book/page use.
 */

#ifndef TAS5825_REGS_H
#define TAS5825_REGS_H

#include <stdint.h>

namespace sfx_audio {
namespace tas5825 {

// ─── I2C address — same on both M and P silicon ────────────────────────────
constexpr uint8_t I2C_ADDR = 0x4C;

// ─── Page / Book select (TI DSP convention; page 0x00, book 0x7F) ─────────
constexpr uint8_t REG_PAGE = 0x00;
constexpr uint8_t REG_BOOK = 0x7F;
constexpr uint8_t BOOK_00  = 0x00;
constexpr uint8_t PAGE_00  = 0x00;

// ─── Book 0 / Page 0 register map (SLASEH7H Table 9-6) ────────────────────
constexpr uint8_t REG_RESET_CTRL    = 0x01;  // [4]=RST_DIG_CORE, [0]=RST_REG → 0x11 = full reset
constexpr uint8_t REG_DEVICE_CTRL1  = 0x02;  // FSW_SEL / DAMP_PBTL / DAMP_MOD (amp topology)
constexpr uint8_t REG_DEVICE_CTRL2  = 0x03;  // [4]=DIS_DSP, [3]=MUTE, [1:0]=CTRL_STATE
constexpr uint8_t REG_SIG_CH_CTRL   = 0x28;  // [7:4] SCLK ratio (RO-ish), [3:0] FSMODE (0 = auto)
constexpr uint8_t REG_CLOCK_DET_CTRL= 0x29;  // clock-detection disable bits (default 0 = all on)
constexpr uint8_t REG_SDOUT_SEL     = 0x30;  // [0] 0 = SDOUT post-DSP, 1 = pre-DSP
constexpr uint8_t REG_I2S_CTRL      = 0x31;  // [5] SCLK_INV
constexpr uint8_t REG_SAP_CTRL1     = 0x33;  // [5:4] DATA_FORMAT (00=I2S), [1:0] WORD_LENGTH — reset 0x02 (24-bit)
constexpr uint8_t REG_SAP_CTRL2     = 0x34;  // I2S_SHIFT LSB (data offset in frame)
constexpr uint8_t REG_SAP_CTRL3     = 0x35;  // DAC data-path L/R routing — reset 0x11 (normal)
constexpr uint8_t REG_FS_MON        = 0x37;  // RO: [5:4] SCLK ratio msbs, [3:0] detected FS code
constexpr uint8_t REG_BCK_MON       = 0x38;  // RO: detected SCLK-per-frame ratio (low 8 bits)
constexpr uint8_t REG_CLKDET_STATUS = 0x39;  // RO: [0]FS bad [1]SCLK bad [2]SCLK missing [3]PLL unlocked [4]PLL overrate [5]SCLK over/under
constexpr uint8_t REG_DSP_PGM_MODE  = 0x40;  // DSP program select — reset 0x01 (ROM mode 1)
constexpr uint8_t REG_DSP_CTRL      = 0x46;  // [4:3] processing rate (00=input), [0] USE_DEFAULT_COEFFS — reset 0x01
constexpr uint8_t REG_DIG_VOL       = 0x4C;  // digital volume, both channels (see VOL_* below)
constexpr uint8_t REG_DIG_VOL_CTRL1 = 0x4E;  // normal ramp up/down speed+step — reset 0x33
constexpr uint8_t REG_DIG_VOL_CTRL2 = 0x4F;  // emergency ramp down speed+step — reset 0x30
constexpr uint8_t REG_AUTO_MUTE_CTRL= 0x50;  // per-channel auto-mute enables — reset 0x07
constexpr uint8_t REG_AUTO_MUTE_TIME= 0x51;  // auto-mute zero-sample time
constexpr uint8_t REG_ANA_CTRL      = 0x53;  // [6:5] Class-D bandwidth, [0] L/R PWM phase
constexpr uint8_t REG_AGAIN         = 0x54;  // [4:0] analog gain, 0 = 0 dB (29.5 Vpeak), −0.5 dB/step
constexpr uint8_t REG_PVDD_ADC      = 0x5E;  // RO: PVDD voltage = value / 8.428 V
constexpr uint8_t REG_GPIO_CTRL     = 0x60;  // [2:0] GPIO2..0 output enables
constexpr uint8_t REG_GPIO0_SEL     = 0x61;  // [3:0] GPIO0 function (see GPIO_SEL_*)
constexpr uint8_t REG_GPIO1_SEL     = 0x62;  // [3:0] GPIO1 function
constexpr uint8_t REG_GPIO2_SEL     = 0x63;  // [3:0] GPIO2 function
constexpr uint8_t REG_GPIO_INPUT_SEL= 0x64;  // GPIO input routing (RESETZ/MUTEZ/...)
constexpr uint8_t REG_GPIO_OUT      = 0x65;  // user-programmed GPIO output values
constexpr uint8_t REG_DIE_ID        = 0x67;  // RO: 0x95 on TAS5825M
constexpr uint8_t REG_POWER_STATE   = 0x68;  // RO: 0=DeepSleep 1=Sleep 2=HiZ 3=Play
constexpr uint8_t REG_AUTOMUTE_STATE= 0x69;  // RO: [1]=right auto-muted, [0]=left auto-muted
constexpr uint8_t REG_CHAN_FAULT    = 0x70;  // RO latch: per-channel DC / OC faults
constexpr uint8_t REG_GLOBAL_FAULT1 = 0x71;  // RO latch: OTP-CRC / BQ / EEPROM / CLK / PVDD OV / PVDD UV
constexpr uint8_t REG_GLOBAL_FAULT2 = 0x72;  // RO latch: CBC OC / over-temp shutdown
constexpr uint8_t REG_WARNING       = 0x73;  // RO latch: CBC warnings + OT warning levels
constexpr uint8_t REG_PIN_CONTROL1  = 0x74;  // FAULTZ-pin fault report masks
constexpr uint8_t REG_PIN_CONTROL2  = 0x75;  // fault/warning latch enables — reset 0xF8
constexpr uint8_t REG_MISC_CONTROL  = 0x76;  // [4] OTSD auto-recovery enable
constexpr uint8_t REG_CBC_CONTROL   = 0x77;  // cycle-by-cycle current-limit enables
constexpr uint8_t REG_FAULT_CLEAR   = 0x78;  // write 0x80 ([7] ANALOG_FAULT_CLEAR) to clear latches

// ─── DEVICE_CTRL2 (0x03) fields — reset value is 0x10 (DIS_DSP set) ───────
constexpr uint8_t CTRL_DEEP_SLEEP  = 0x00;
constexpr uint8_t CTRL_SLEEP       = 0x01;
constexpr uint8_t CTRL_HIZ         = 0x02;
constexpr uint8_t CTRL_PLAY        = 0x03;
constexpr uint8_t CTRL_MUTE_BIT    = 0x08;  // soft ramp mute, both channels
constexpr uint8_t CTRL_DIS_DSP_BIT = 0x10;  // hold DSP in reset; clear ONLY after I2S clocks are stable

// ─── SAP_CTRL1 (0x33) fields ──────────────────────────────────────────────
constexpr uint8_t SAP_WORD_16BIT  = 0x00;   // [1:0] word length; [5:4]=00 keeps I2S format
constexpr uint8_t SAP_WORD_20BIT  = 0x01;
constexpr uint8_t SAP_WORD_24BIT  = 0x02;   // silicon reset default
constexpr uint8_t SAP_WORD_32BIT  = 0x03;

// ─── DIG_VOL (0x4C) scale — SLASEH7H Table 9-24 ───────────────────────────
//
// 0x00 = +24.0 dB (FULL GAIN — the loudest setting, never a "mute"),
// each +1 count = −0.5 dB, 0x30 = 0 dB, 0xFE = −103 dB, 0xFF = mute.
constexpr uint8_t VOL_MAX_GAIN = 0x00;  // +24 dB
constexpr uint8_t VOL_0DB      = 0x30;  // 0 dB reference
constexpr uint8_t VOL_MIN      = 0xFE;  // −103 dB
constexpr uint8_t VOL_MUTE     = 0xFF;  // mute

/// dB → DIG_VOL register value (clamped to [+24 dB … −103 dB]).
inline uint8_t volRegForDb(float db) {
    if (db > 24.0f)   db = 24.0f;
    if (db < -103.0f) return VOL_MUTE;
    int v = static_cast<int>(48.0f - db * 2.0f + 0.5f);
    if (v < 0x00) v = 0x00;
    if (v > VOL_MIN) v = VOL_MIN;
    return static_cast<uint8_t>(v);
}

// ─── DIE_ID (0x67) ────────────────────────────────────────────────────────
constexpr uint8_t DIE_ID_TAS5825M = 0x95;   // datasheet reset value

// ─── Analog gain (AGAIN 0x54) — PVDD auto-detected ────────────────────────
//
// [4:0], 0 = 0 dB (29.5 V peak), −0.5 dB per step, down to −15.5 dB.
// The gain is chosen so full-scale output matches the supply rail. Since
// 2026-08-01 the drivers AUTO-DETECT the rail via PVDD_ADC at activate()
// (the manual `codec_supply` config was retired); AGAIN_FALLBACK is used
// when the ADC reads below the chip's 4.5 V operating floor.
constexpr uint8_t AGAIN_FALLBACK = 0x10;   // −8.0 dB, 11.74 Vpeak — safe on any valid rail

/// Full-scale peak output in mV for each AGAIN step (29.5 V × 10^(−step/40)).
constexpr uint16_t AGAIN_FULLSCALE_MV[32] = {
    29500, 27850, 26292, 24821, 23432, 22121, 20884, 19715,
    18612, 17571, 16588, 15660, 14784, 13957, 13176, 12439,
    11743, 11086, 10466,  9880,  9327,  8805,  8313,  7848,
     7409,  6994,  6603,  6234,  5885,  5556,  5245,  4951,
};

/// Smallest attenuation step whose full-scale output fits under the
/// measured rail (no clipping). ≥29.5 V → step 0; below the table → 31.
inline uint8_t againStepForPvdd_mV(uint32_t pvdd_mV) {
    for (uint8_t step = 0; step < 32; step++) {
        if (AGAIN_FULLSCALE_MV[step] <= pvdd_mV) return step;
    }
    return 31;
}

// PVDD_ADC (0x5E) conversion + the chip's operating floor (datasheet
// operating range 4.5–26.4 V; a reading below the floor means the ADC
// isn't valid yet → use AGAIN_FALLBACK).
constexpr uint32_t PVDD_MIN_VALID_MV = 4500;
inline uint32_t pvddMvFromAdc(uint8_t adc) { return (adc * 1000000UL) / 8428; }

// ─── FS_MON (0x37) low-nibble sample-rate codes — Table 9-19 ──────────────
//
// 0x00 = FS error / no lock; all codes not listed are reserved. On the
// M variant this must be polled non-zero before HIZ → PLAY.
constexpr uint8_t FS_ERROR  = 0x00;
constexpr uint8_t FS_16KHZ  = 0x04;
constexpr uint8_t FS_32KHZ  = 0x06;
constexpr uint8_t FS_48KHZ  = 0x09;
constexpr uint8_t FS_96KHZ  = 0x0B;
constexpr uint8_t FS_192KHZ = 0x0D;

// ─── CHAN_FAULT (0x70) bits — Table 9-56 ──────────────────────────────────
constexpr uint8_t CHF_CH1_DC = 0x08;  // left DC fault
constexpr uint8_t CHF_CH2_DC = 0x04;  // right DC fault
constexpr uint8_t CHF_CH1_OC = 0x02;  // left over-current
constexpr uint8_t CHF_CH2_OC = 0x01;  // right over-current

// ─── GLOBAL_FAULT1 (0x71) bits — Table 9-57 ───────────────────────────────
constexpr uint8_t FAULT1_OTP_CRC = 0x80;  // OTP CRC check error
constexpr uint8_t FAULT1_BQ_WR   = 0x40;  // biquad write failed
constexpr uint8_t FAULT1_EEPROM  = 0x20;  // EEPROM boot load failed
constexpr uint8_t FAULT1_CLK     = 0x04;  // clock fault (auto-recovers when clocks return)
constexpr uint8_t FAULT1_PVDD_OV = 0x02;  // PVDD over-voltage
constexpr uint8_t FAULT1_PVDD_UV = 0x01;  // PVDD under-voltage

// ─── GLOBAL_FAULT2 (0x72) bits — Table 9-58 ───────────────────────────────
constexpr uint8_t FAULT2_CBC_CH2 = 0x04;  // right cycle-by-cycle over-current
constexpr uint8_t FAULT2_CBC_CH1 = 0x02;  // left cycle-by-cycle over-current
constexpr uint8_t FAULT2_OTSD    = 0x01;  // over-temperature shutdown

// ─── WARNING (0x73) bits — Table 9-59 ─────────────────────────────────────
constexpr uint8_t WARN_CBCW_CH1 = 0x20;
constexpr uint8_t WARN_CBCW_CH2 = 0x10;
constexpr uint8_t WARN_OTW_146C = 0x08;
constexpr uint8_t WARN_OTW_134C = 0x04;
constexpr uint8_t WARN_OTW_122C = 0x02;
constexpr uint8_t WARN_OTW_112C = 0x01;

// ─── GPIOx_SEL (0x61–0x63) function codes — Table 9-41/42/43 ──────────────
constexpr uint8_t GPIO_SEL_OFF    = 0x00;
constexpr uint8_t GPIO_SEL_WARNZ  = 0x08;
constexpr uint8_t GPIO_SEL_SDOUT  = 0x09;
constexpr uint8_t GPIO_SEL_FAULTZ = 0x0B;

// GPIO_CTRL (0x60) output-enable bits — required for any GPIOx_SEL output.
constexpr uint8_t GPIO0_OE = 0x01;
constexpr uint8_t GPIO1_OE = 0x02;
constexpr uint8_t GPIO2_OE = 0x04;

// ─── FAULT_CLEAR (0x78) ───────────────────────────────────────────────────
constexpr uint8_t FAULT_CLEAR_CMD = 0x80;   // [7] ANALOG_FAULT_CLEAR, write-clear

// ─── Supply-mode wire code ────────────────────────────────────────────────
//
// CODEC_STATUS_RESP byte 5 historically carried the manual `codec_supply`
// selector (0=12v … 3=24v). The config was retired 2026-08-01 — the
// drivers auto-detect via PVDD_ADC — so the byte now always reports 4
// ("auto"); the measured rail rides in the appended pvdd_mV field.
constexpr uint8_t SUPPLY_CODE_AUTO = 4;

inline const char* fsMonStr(uint8_t fs) {
    switch (fs & 0x0F) {
        case FS_ERROR:  return "NONE";
        case FS_16KHZ:  return "16kHz";
        case FS_32KHZ:  return "32kHz";
        case FS_48KHZ:  return "48kHz";
        case FS_96KHZ:  return "96kHz";
        case FS_192KHZ: return "192kHz";
    }
    return "reserved";
}

inline const char* powerStateStr(uint8_t state) {
    switch (state) {
        case CTRL_DEEP_SLEEP: return "DEEP_SLEEP";
        case CTRL_SLEEP:      return "SLEEP";
        case CTRL_HIZ:        return "HIZ";
        case CTRL_PLAY:       return "PLAY";
    }
    return "???";
}

/// PVDD_ADC (0x5E) reading → volts (SLASEH7H Table 9-39).
inline float pvddVolts(uint8_t adc) { return adc / 8.428f; }

}  // namespace tas5825
}  // namespace sfx_audio

#endif // TAS5825_REGS_H
