/**
 * @file tas5825_regs.h
 * @brief Shared register definitions for the TI TAS5825 audio amplifier
 *        family — used by both the M-variant and P-variant drivers.
 *
 * Register addresses, bit patterns, and mode constants live here.
 * Variant-specific registers / bits would be defined in each variant's
 * header; only the P variant (tas5825_p_codec.h) is in use today — the
 * M-variant driver was removed as dead code (arduino-removal branch).
 *
 * The two variants are pin-compatible (QFN-32, RHB package) and share
 * ~95% of the page-0/book-0 register map. The P silicon adds Class-H
 * Hybrid-Pro feedback registers; the M silicon adds smart-amp /
 * IV-sense registers. The init sequence the chips enforce around
 * those shared registers also differs — see tas5825_m_codec.cpp and
 * tas5825_p_codec.cpp for the variant-specific flow.
 *
 * Reference: TI TAS5825M datasheet (SLAA846 advanced features),
 *            TI TAS5825P datasheet (Hybrid-Pro user guidance).
 */

#ifndef TAS5825_REGS_H
#define TAS5825_REGS_H

#include <stdint.h>

namespace sfx_audio {
namespace tas5825 {

// ─── I2C address — same on both M and P silicon ────────────────────────────
constexpr uint8_t I2C_ADDR = 0x4C;

// ─── Page / Book select (book + page navigation, same on both variants) ───
constexpr uint8_t REG_PAGE = 0x00;
constexpr uint8_t REG_BOOK = 0x7F;
constexpr uint8_t BOOK_00  = 0x00;
constexpr uint8_t PAGE_00  = 0x00;

// ─── Core control registers (book 0, page 0) — identical on M and P ───────
constexpr uint8_t REG_RESET        = 0x01;  // [4]=DSP reset, [0]=registers reset
constexpr uint8_t REG_MODE_CTRL    = 0x02;
constexpr uint8_t REG_DEVICE_CTRL  = 0x03;  // DEVICE_CTRL2 — main mode select
constexpr uint8_t REG_SIG_CH_CTRL  = 0x28;
constexpr uint8_t REG_SDOUT_SEL    = 0x30;
constexpr uint8_t REG_CLK_SRC      = 0x33;
constexpr uint8_t REG_FS_RATE      = 0x34;
constexpr uint8_t REG_FS_MON       = 0x37;  // Read-only sample-rate monitor
constexpr uint8_t REG_ANALOG_CTRL  = 0x46;
constexpr uint8_t REG_DIGITAL_VOL  = 0x4C;
constexpr uint8_t REG_GPIO1_SEL    = 0x4F;  // GPIO1 routing — semantics differ M vs P (see variant headers)
constexpr uint8_t REG_AGAIN_L      = 0x53;
constexpr uint8_t REG_AGAIN_R      = 0x54;

// SAP_CTRL_1 (Serial Audio Port Control 1) — TAS5825M datasheet §7.6.18,
// matching field on TAS5825P. Misleadingly named CLK_CFG in older code.
//   bits 5:4 = I2S_LRCLK_PULSE (00=auto, 11=50%-duty)
//   bits 3:2 = SAP_DATA_FORMAT (00=I2S, 01=TDM, 10=RTJ, 11=LTJ)
//   bits 1:0 = WORD_LENGTH      (00=16, 01=20, 10=24, 11=32)
// Reset default 0x32 (50%-duty LRCLK, I2S, 24-bit). Word length here MUST
// match the I2S TX bit width or the codec rejects the bus and never locks.
constexpr uint8_t REG_SAP_CTRL1    = 0x60;
constexpr uint8_t REG_DSP_MISC     = 0x62;
constexpr uint8_t REG_POWER_STATE  = 0x68;  // Read-only — current state
constexpr uint8_t REG_AUTOMUTE     = 0x69;
constexpr uint8_t REG_CHAN_FAULT   = 0x70;
constexpr uint8_t REG_GLOBAL1      = 0x71;  // GLOBAL_FAULT1 — bit-decoded by variant diagnostics
constexpr uint8_t REG_GLOBAL2      = 0x72;
constexpr uint8_t REG_OT_WARNING   = 0x73;
constexpr uint8_t REG_FAULT_CLEAR  = 0x78;  // Write 0x80 to clear latched faults

// ─── DEVICE_CTRL2 (0x03) mode field values ────────────────────────────────
constexpr uint8_t CTRL_DEEP_SLEEP = 0x00;  // PLL off, no clocks needed
constexpr uint8_t CTRL_SLEEP      = 0x01;  // PLL on, output muted
constexpr uint8_t CTRL_HIZ        = 0x02;  // High-Z, PLL active, awaits clocks
constexpr uint8_t CTRL_PLAY       = 0x03;  // Output active
constexpr uint8_t CTRL_MUTE_BIT   = 0x08;  // OR with mode → muted

// ─── SAP_CTRL_1 word-length values (lower 2 bits) ─────────────────────────
constexpr uint8_t SAP_WORD_16BIT  = 0x00;
constexpr uint8_t SAP_WORD_20BIT  = 0x01;
constexpr uint8_t SAP_WORD_24BIT  = 0x02;
constexpr uint8_t SAP_WORD_32BIT  = 0x03;

// ─── DIGITAL_VOL (0x4C) calibration ───────────────────────────────────────
constexpr uint8_t VOL_MUTE = 0x00;  // –100 dB / mute
constexpr uint8_t VOL_0DB  = 0x30;  // 0 dB reference
constexpr uint8_t VOL_MAX  = 0xCF;  // +24 dB

// ─── Analog gain register values per supply voltage ───────────────────────
//
// Mapped from datasheet §7.4. The chip's analog stage scales the
// modulator output to match the speaker excursion at a given PVDD; the
// gain register selects which scaling factor (lower numeric = higher
// gain, suitable for higher PVDD).
constexpr uint8_t AGAIN_12V = 0x10;  // -8.0 dB,  11.74 Vpeak  (3S LiPo nominal)
constexpr uint8_t AGAIN_15V = 0x0C;  // -5.05 dB, 14.73 Vpeak  (4S LiPo nominal)
constexpr uint8_t AGAIN_20V = 0x07;  // -3.05 dB, 19.73 Vpeak
constexpr uint8_t AGAIN_24V = 0x05;  // -2.05 dB, 23.72 Vpeak

// ─── FS_MON (0x37) sample-rate codes (lower nibble) ───────────────────────
//
// Returned by the auto-detect logic when valid clocks are present. 0x00
// means "no lock"; on the M variant this is the value to poll for
// non-zero before transitioning HIZ → PLAY.
constexpr uint8_t FS_NONE     = 0x00;
constexpr uint8_t FS_8KHZ     = 0x01;
constexpr uint8_t FS_16KHZ    = 0x02;
constexpr uint8_t FS_32KHZ    = 0x03;
constexpr uint8_t FS_48KHZ    = 0x04;
constexpr uint8_t FS_96KHZ    = 0x05;
constexpr uint8_t FS_44KHZ    = 0x06;
constexpr uint8_t FS_88KHZ    = 0x07;
constexpr uint8_t FS_176KHZ   = 0x08;
constexpr uint8_t FS_192KHZ   = 0x09;
constexpr uint8_t FS_INVALID  = 0x0F;

// ─── GLOBAL_FAULT1 (0x71) bit decode — used by variant diagnostics ────────
constexpr uint8_t FAULT_CLK   = 0x80;  // Clock invalid
constexpr uint8_t FAULT_OVP   = 0x40;  // Output / supply over-voltage
constexpr uint8_t FAULT_UVP   = 0x20;  // Supply under-voltage
constexpr uint8_t FAULT_DC    = 0x10;  // Output DC offset
constexpr uint8_t FAULT_OC    = 0x08;  // Output overcurrent (short)
constexpr uint8_t FAULT_OTP   = 0x04;  // Overtemperature shutdown

// ─── Common supply-voltage selector ────────────────────────────────────────
//
// Drives `configureAnalogGain()` in both variants. Same values, same
// semantics; the only difference is which AGAIN_* constant gets written.
enum class Supply : uint8_t {
    V12 = 0,
    V15 = 1,
    V20 = 2,
    V24 = 3,
};

inline uint8_t analogGainFor(Supply s) {
    switch (s) {
        case Supply::V12: return AGAIN_12V;
        case Supply::V15: return AGAIN_15V;
        case Supply::V20: return AGAIN_20V;
        case Supply::V24: return AGAIN_24V;
    }
    return AGAIN_20V;
}

inline const char* supplyStr(Supply s) {
    switch (s) {
        case Supply::V12: return "12v";
        case Supply::V15: return "15v";
        case Supply::V20: return "20v";
        case Supply::V24: return "24v";
    }
    return "??v";
}

inline bool parseSupply(const char* str, Supply& out) {
    if (!str) return false;
    if (strcmp(str, "12v") == 0 || strcmp(str, "12V") == 0) { out = Supply::V12; return true; }
    if (strcmp(str, "15v") == 0 || strcmp(str, "15V") == 0) { out = Supply::V15; return true; }
    if (strcmp(str, "20v") == 0 || strcmp(str, "20V") == 0) { out = Supply::V20; return true; }
    if (strcmp(str, "24v") == 0 || strcmp(str, "24V") == 0) { out = Supply::V24; return true; }
    return false;
}

inline const char* fsMonStr(uint8_t fs) {
    switch (fs & 0x0F) {
        case FS_NONE:    return "NONE";
        case FS_8KHZ:    return "8kHz";
        case FS_16KHZ:   return "16kHz";
        case FS_32KHZ:   return "32kHz";
        case FS_48KHZ:   return "48kHz";
        case FS_96KHZ:   return "96kHz";
        case FS_44KHZ:   return "44.1kHz";
        case FS_88KHZ:   return "88.2kHz";
        case FS_176KHZ:  return "176.4kHz";
        case FS_192KHZ:  return "192kHz";
        case FS_INVALID: return "INVALID";
    }
    return "???";
}

inline const char* powerStateStr(uint8_t state) {
    switch (state & 0x0F) {
        case CTRL_DEEP_SLEEP: return "DEEP_SLEEP";
        case CTRL_SLEEP:      return "SLEEP";
        case CTRL_HIZ:        return "HIZ";
        case CTRL_PLAY:       return "PLAY";
    }
    return "???";
}

}  // namespace tas5825
}  // namespace sfx_audio

#endif // TAS5825_REGS_H
