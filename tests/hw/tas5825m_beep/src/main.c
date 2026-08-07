/**
 * tas5825m_beep — TAS5825M codec bring-up probe with a 1 kHz beep.
 *
 * Self-contained pure ESP-IDF (no ScaleFX libs) so it can bisect a board
 * problem independent of the production firmware.  Everything prints to
 * UART0 at 115200:
 *
 *   1. I2C bus scan (expect the codec at 0x4C; the PCA9685 at 0x40/0x70
 *      and the INA226 at 0x41 also show on a HubFX).
 *   2. DIE_ID identity check — TAS5825M silicon reads 0x95.
 *   3. M-strict init sequence, one line per register access, every write
 *      READ BACK and verified (self-clearing regs are flagged instead).
 *   4. FS_MON polled until the codec locks onto the I2S clocks — 48 kHz
 *      reports code 0x09 (SLASEH7H Table 9-19).
 *   5. A 1 kHz sine beeps 200 ms on / 800 ms off; ZEROS are streamed in
 *      the gaps so BCLK/LRCLK never stop (clock loss = fault + HiZ).
 *   6. A 2 s heartbeat dumps POWER_STATE / FS_MON / PVDD voltage /
 *      CLKDET_STATUS / all three fault registers + warnings with bit
 *      decodes, and auto-recovers (FAULT_CLEAR + HIZ->PLAY) if the
 *      codec fell out of PLAY.
 *
 * Interpretation:
 *   - scan shows no 0x4C                 -> I2C wiring / codec power (DVDD)
 *   - DIE_ID != 0x95                     -> not TAS5825M silicon (or bus issue)
 *   - writes OK but FS_MON never locks   -> BCLK/LRCLK path (GPIO17/18);
 *                                           read the CLKDET_STATUS decode
 *   - FS locks but PLAY fails            -> GLOBAL_FAULT1 decode
 *                                           (PVDD_UV = amp supply rail!)
 *   - PLAY ok but no sound               -> speaker wiring / volume /
 *                                           scope OUT_A/OUT_B; check the
 *                                           heartbeat's PVDD voltage
 *
 * GOLD STANDARD: every register address / bit below is from the TI
 * TAS5825M datasheet SLASEH7H (Oct 2019, rev Jan 2023), Table 9-6 and
 * the per-register tables in section 9.6.1.  The library mirror is
 * controllers/lib/sfx_audio/codec/tas5825_regs.h.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_timer.h"

// ─── Board wiring (HubFX, same on every rev) ──────────────────────────────
#define PIN_I2C_SDA        8
#define PIN_I2C_SCL        9
#define PIN_I2S_BCLK       17
#define PIN_I2S_LRCLK      18
#define PIN_I2S_DOUT       16

#define I2C_PORT           I2C_NUM_0
#define I2C_FREQ_HZ        100000
#define I2C_TIMEOUT_MS     50

// ─── Audio format ─────────────────────────────────────────────────────────
#define SAMPLE_RATE_HZ     48000
#define BEEP_FREQ_HZ       1000        // 48 samples per cycle at 48 kHz
#define BEEP_AMPLITUDE     8000        // ~ -12 dBFS
#define BEEP_ON_MS         200
#define BEEP_OFF_MS        800
#define BEEP_ATTEN_DB      20          // codec digital volume: -20 dB

// ─── TAS5825M registers — SLASEH7H Table 9-6 (book 0, page 0) ─────────────
#define TAS_ADDR           0x4C
#define REG_PAGE           0x00
#define REG_RESET_CTRL     0x01        // 0x11 = RST_DIG_CORE + RST_REG
#define REG_DEVICE_CTRL1   0x02        // amp topology (BTL/BD defaults)
#define REG_DEVICE_CTRL2   0x03        // [4]=DIS_DSP [3]=MUTE [1:0]=state
#define REG_SIG_CH_CTRL    0x28        // [3:0] FSMODE, 0 = auto detect
#define REG_SDOUT_SEL      0x30
#define REG_SAP_CTRL1      0x33        // [5:4]=format(00=I2S) [1:0]=word len — reset 0x02 (24-bit)
#define REG_FS_MON         0x37        // RO [3:0] detected FS code
#define REG_BCK_MON        0x38        // RO detected SCLK-per-frame ratio
#define REG_CLKDET_STATUS  0x39        // RO clock-detector status bits
#define REG_DIG_VOL        0x4C        // 0x00=+24dB 0x30=0dB (-0.5dB/step) 0xFF=mute
#define REG_ANA_CTRL       0x53
#define REG_AGAIN          0x54        // [4:0] 0=0dB(29.5Vp), -0.5dB/step
#define REG_PVDD_ADC       0x5E        // RO: PVDD = value / 8.428 V
#define REG_GPIO_CTRL      0x60        // [2:0] GPIO output enables
#define REG_GPIO1_SEL      0x62        // [3:0] GPIO1 function
#define REG_DIE_ID         0x67        // RO: 0x95 on TAS5825M
#define REG_POWER_STATE    0x68        // RO: 0=DeepSleep 1=Sleep 2=HiZ 3=Play
#define REG_AUTOMUTE_STATE 0x69
#define REG_CHAN_FAULT     0x70        // [3]L-DC [2]R-DC [1]L-OC [0]R-OC
#define REG_GLOBAL_FAULT1  0x71        // [7]OTP-CRC [6]BQ [5]EEPROM [2]CLK [1]PVDD-OV [0]PVDD-UV
#define REG_GLOBAL_FAULT2  0x72        // [2]CBC-R [1]CBC-L [0]OTSD
#define REG_WARNING        0x73        // [5]CBCW-L [4]CBCW-R [3:0]OTW 146/134/122/112C
#define REG_FAULT_CLEAR    0x78        // write 0x80 to clear latches
#define REG_BOOK           0x7F

#define CTRL_DEEP_SLEEP    0x00
#define CTRL_HIZ           0x02
#define CTRL_PLAY          0x03
#define CTRL_MUTE_BIT      0x08
#define CTRL_DIS_DSP_BIT   0x10        // hold DSP in reset until clocks stable

#define SAP_I2S_16BIT      0x00        // I2S format + 16-bit word length
#define GPIO1_OE           0x02        // GPIO_CTRL bit for GPIO1
#define GPIO_SEL_FAULTZ    0x0B
#define DIE_ID_TAS5825M    0x95
#define FAULT_CLEAR_CMD    0x80
#define FS_ERROR           0x00
#define FS_48KHZ           0x09        // NOT 0x04 — Table 9-19

// AGAIN by PVDD supply — HubFX runs the amp rail at 12 V (-8 dB analog).
#define AGAIN_12V          0x10
#define VOL_0DB_REG        0x30

// ─── Shared counters (single writer each; probe-grade, not production) ────
static volatile uint32_t s_beep_cycles   = 0;
static volatile uint32_t s_i2s_errors    = 0;

static i2s_chan_handle_t s_tx_chan = NULL;

// ─── Logging helpers ──────────────────────────────────────────────────────

static int64_t up_ms(void) { return esp_timer_get_time() / 1000; }

#define LOGF(fmt, ...) printf("[%8lld] " fmt "\n", (long long)up_ms(), ##__VA_ARGS__)

// ─── I2C register access, instrumented ────────────────────────────────────

static esp_err_t i2c_wr_raw(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_PORT, TAS_ADDR, buf, 2,
                                      pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t i2c_rd_raw(uint8_t reg, uint8_t *val) {
    return i2c_master_write_read_device(I2C_PORT, TAS_ADDR, &reg, 1, val, 1,
                                        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

// Write + readback verify.  `self_clearing` skips the verify (RESET_CTRL,
// FAULT_CLEAR read back as 0 by design).  A readback MISMATCH is reported
// but not fatal (some ctrl regs mix status bits into the readback).
static bool wr(uint8_t reg, uint8_t val, const char *name, bool self_clearing) {
    esp_err_t e = i2c_wr_raw(reg, val);
    if (e != ESP_OK) {
        LOGF("[i2c] W 0x%02X %-13s <= 0x%02X  ** WRITE FAILED: %s **",
             reg, name, val, esp_err_to_name(e));
        return false;
    }
    if (self_clearing) {
        LOGF("[i2c] W 0x%02X %-13s <= 0x%02X  (self-clearing, no verify)",
             reg, name, val);
        return true;
    }
    uint8_t rb = 0;
    e = i2c_rd_raw(reg, &rb);
    if (e != ESP_OK) {
        LOGF("[i2c] W 0x%02X %-13s <= 0x%02X  ** READBACK FAILED: %s **",
             reg, name, val, esp_err_to_name(e));
        return false;
    }
    LOGF("[i2c] W 0x%02X %-13s <= 0x%02X  rb=0x%02X %s",
         reg, name, val, rb, (rb == val) ? "OK" : "** MISMATCH **");
    return true;
}

static uint8_t rd(uint8_t reg, const char *name) {
    uint8_t v = 0;
    esp_err_t e = i2c_rd_raw(reg, &v);
    if (e != ESP_OK) {
        LOGF("[i2c] R 0x%02X %-13s ** READ FAILED: %s **",
             reg, name, esp_err_to_name(e));
        return 0xFF;
    }
    LOGF("[i2c] R 0x%02X %-13s = 0x%02X", reg, name, v);
    return v;
}

// Silent read for the heartbeat (it prints its own decoded summary).
static uint8_t rdq(uint8_t reg) {
    uint8_t v = 0xFF;
    i2c_rd_raw(reg, &v);
    return v;
}

static void select_book_page(uint8_t book, uint8_t page) {
    wr(REG_PAGE, page, "PAGE", false);
    wr(REG_BOOK, book, "BOOK", false);
}

// ─── Decoders (SLASEH7H bit tables) ───────────────────────────────────────

static const char *power_state_str(uint8_t ps) {
    switch (ps) {
        case 0x00: return "DEEP_SLEEP";
        case 0x01: return "SLEEP";
        case 0x02: return "HIZ";
        case 0x03: return "PLAY";
    }
    return "reserved";
}

static const char *fs_mon_str(uint8_t fs) {
    switch (fs & 0x0F) {
        case 0x00: return "ERROR(no lock)";
        case 0x04: return "16kHz";
        case 0x06: return "32kHz";
        case 0x09: return "48kHz";
        case 0x0B: return "96kHz";
        case 0x0D: return "192kHz";
    }
    return "reserved";
}

// GLOBAL_FAULT1 (0x71): [7]OTP-CRC [6]BQ-write [5]EEPROM [2]CLK [1]OV [0]UV
static void decode_gf1(uint8_t f, char *out, size_t cap) {
    out[0] = '\0';
    if (f == 0) { snprintf(out, cap, "none"); return; }
    if (f & 0x80) strlcat(out, "OTP_CRC ", cap);
    if (f & 0x40) strlcat(out, "BQ_WR ", cap);
    if (f & 0x20) strlcat(out, "EEPROM ", cap);
    if (f & 0x04) strlcat(out, "CLK ", cap);
    if (f & 0x02) strlcat(out, "PVDD_OV ", cap);
    if (f & 0x01) strlcat(out, "PVDD_UV(supply!) ", cap);
}

// CHAN_FAULT (0x70): [3]L-DC [2]R-DC [1]L-OC [0]R-OC
static void decode_chf(uint8_t f, char *out, size_t cap) {
    out[0] = '\0';
    if (f == 0) { snprintf(out, cap, "none"); return; }
    if (f & 0x08) strlcat(out, "L_DC ", cap);
    if (f & 0x04) strlcat(out, "R_DC ", cap);
    if (f & 0x02) strlcat(out, "L_OC ", cap);
    if (f & 0x01) strlcat(out, "R_OC ", cap);
}

// GLOBAL_FAULT2 (0x72): [2]CBC-R [1]CBC-L [0]over-temp shutdown
static void decode_gf2(uint8_t f, char *out, size_t cap) {
    out[0] = '\0';
    if (f == 0) { snprintf(out, cap, "none"); return; }
    if (f & 0x04) strlcat(out, "CBC_R ", cap);
    if (f & 0x02) strlcat(out, "CBC_L ", cap);
    if (f & 0x01) strlcat(out, "OTSD(thermal!) ", cap);
}

// CLKDET_STATUS (0x39): [0]FS bad [1]SCLK bad [2]SCLK missing [3]PLL
// unlocked [4]PLL overrate [5]SCLK over/underrate
static void decode_clkdet(uint8_t s, char *out, size_t cap) {
    out[0] = '\0';
    if (s == 0) { snprintf(out, cap, "all clocks OK"); return; }
    if (s & 0x01) strlcat(out, "FS_bad ", cap);
    if (s & 0x02) strlcat(out, "SCLK_bad ", cap);
    if (s & 0x04) strlcat(out, "SCLK_missing ", cap);
    if (s & 0x08) strlcat(out, "PLL_unlocked ", cap);
    if (s & 0x10) strlcat(out, "PLL_overrate ", cap);
    if (s & 0x20) strlcat(out, "SCLK_rate_bad ", cap);
}

// ─── I2C bring-up + bus scan ──────────────────────────────────────────────

static void i2c_init_and_scan(void) {
    const i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));
    LOGF("[i2c] master up: SDA=%d SCL=%d @ %d kHz",
         PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ / 1000);

    LOGF("[i2c] bus scan 0x08..0x77:");
    int found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (a << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t e = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (e == ESP_OK) {
            found++;
            const char *hint = "";
            if (a == TAS_ADDR) hint = " <-- TAS5825 (target)";
            else if (a == 0x40) hint = " (PCA9685 hw addr / INA collision)";
            else if (a == 0x41) hint = " (INA226 battery)";
            else if (a == 0x70) hint = " (PCA9685 all-call alias)";
            LOGF("[i2c]   ACK @ 0x%02X%s", a, hint);
        }
    }
    LOGF("[i2c] scan done: %d device(s)", found);
}

// ─── I2S TX bring-up ──────────────────────────────────────────────────────

static void i2s_init_tx(void) {
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   // underrun outputs zeros, not stale data
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // TAS5825 PLLs from BCLK, no MCLK
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    LOGF("[i2s] TX up: BCLK=%d LRCLK=%d DOUT=%d — %d Hz, 16-bit stereo, "
         "Philips, no MCLK",
         PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT, SAMPLE_RATE_HZ);
}

// ─── Beep writer task ─────────────────────────────────────────────────────
//
// Streams 10 ms blocks forever: sine during the ON window, zeros during
// the OFF window.  The stream NEVER stops — losing BCLK/LRCLK trips the
// clock fault and drops the codec to HiZ.

#define BLOCK_FRAMES   (SAMPLE_RATE_HZ / 100)              // 10 ms
#define CYCLE_BLOCKS   ((BEEP_ON_MS + BEEP_OFF_MS) / 10)
#define ON_BLOCKS      (BEEP_ON_MS / 10)

static void beep_task(void *arg) {
    (void)arg;
    static int16_t sine[SAMPLE_RATE_HZ / BEEP_FREQ_HZ];    // one 1 kHz cycle
    static int16_t block[BLOCK_FRAMES * 2];                // stereo

    const int n = SAMPLE_RATE_HZ / BEEP_FREQ_HZ;
    for (int i = 0; i < n; i++) {
        sine[i] = (int16_t)(BEEP_AMPLITUDE * sinf(2.0f * (float)M_PI * i / n));
    }

    LOGF("[beep] writer running: %d Hz, %d ms on / %d ms off, amp=%d "
         "(~-12 dBFS), zeros in the gaps (clocks never stop)",
         BEEP_FREQ_HZ, BEEP_ON_MS, BEEP_OFF_MS, BEEP_AMPLITUDE);

    uint32_t blk = 0;
    int      phase = 0;
    for (;;) {
        bool on = (blk % CYCLE_BLOCKS) < ON_BLOCKS;
        if (on) {
            for (int i = 0; i < BLOCK_FRAMES; i++) {
                int16_t s = sine[phase];
                phase = (phase + 1) % n;
                block[2 * i]     = s;
                block[2 * i + 1] = s;
            }
        } else {
            memset(block, 0, sizeof(block));
            phase = 0;
        }

        size_t written = 0;
        esp_err_t e = i2s_channel_write(s_tx_chan, block, sizeof(block),
                                        &written, pdMS_TO_TICKS(1000));
        if (e != ESP_OK || written != sizeof(block)) {
            s_i2s_errors++;
        }

        blk++;
        if (blk % CYCLE_BLOCKS == 0) {
            s_beep_cycles++;
            if (s_beep_cycles <= 3 || s_beep_cycles % 30 == 0) {
                LOGF("[beep] cycle #%lu (i2s errors: %lu)",
                     (unsigned long)s_beep_cycles, (unsigned long)s_i2s_errors);
            }
        }
    }
}

// ─── TAS5825M init — official M-strict sequence, fully instrumented ───────

static bool tas_probe(void) {
    for (int attempt = 1; attempt <= 3; attempt++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (TAS_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t e = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (e == ESP_OK) {
            LOGF("[tas] probe @ 0x%02X: ACK (attempt %d)", TAS_ADDR, attempt);
            return true;
        }
        LOGF("[tas] probe @ 0x%02X: no ACK (attempt %d/3) — %s",
             TAS_ADDR, attempt, esp_err_to_name(e));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

// Phase 1 — safe BEFORE I2S clocks: identity check, full reset, park
// with the DSP HELD IN RESET (DEVICE_CTRL2.DIS_DSP stays 1 — the
// datasheet says clear it only after the input clocks are settled),
// clear the boot-time clock-fault latch.
static void tas_phase1(void) {
    LOGF("── PHASE 1: identity + reset + park (pre-clock) ──────────────");
    select_book_page(0x00, 0x00);

    uint8_t die = rd(REG_DIE_ID, "DIE_ID");
    if (die == DIE_ID_TAS5825M) {
        LOGF("[tas] DIE_ID=0x95 — TAS5825M silicon CONFIRMED");
    } else {
        LOGF("[tas] ** DIE_ID=0x%02X, expected 0x95 (TAS5825M) — different "
             "silicon or bus problem **", die);
    }

    wr(REG_DEVICE_CTRL2, CTRL_DIS_DSP_BIT | CTRL_MUTE_BIT | CTRL_DEEP_SLEEP,
       "DEVICE_CTRL2", false);
    vTaskDelay(pdMS_TO_TICKS(5));
    wr(REG_RESET_CTRL, 0x11, "RESET_CTRL", true);    // dig core + registers
    vTaskDelay(pdMS_TO_TICKS(50));
    select_book_page(0x00, 0x00);
    wr(REG_DEVICE_CTRL2, CTRL_DIS_DSP_BIT | CTRL_MUTE_BIT | CTRL_DEEP_SLEEP,
       "DEVICE_CTRL2", false);
    wr(REG_FAULT_CLEAR, FAULT_CLEAR_CMD, "FAULT_CLEAR", true);
    vTaskDelay(pdMS_TO_TICKS(5));
    LOGF("── phase 1 done: parked (DSP held), fault latch cleared ──────");
}

// Phase 2 — REQUIRES running I2S clocks.  Config, FS_MON gate, then
// release the DSP into HiZ and go to PLAY.
static bool tas_phase2(void) {
    LOGF("── PHASE 2: configure + FS gate + PLAY (clocks running) ──────");

    select_book_page(0x00, 0x00);

    // Analog gain for the 12 V PVDD rail (AGAIN 0x54: -0.5 dB/step).
    wr(REG_AGAIN, AGAIN_12V, "AGAIN", false);

    // Serial audio port: rate auto-detect (FSMODE=0), SDOUT post-DSP,
    // I2S format + 16-bit word length (reset default is 24-bit — MUST
    // match the I2S TX width).
    wr(REG_SIG_CH_CTRL, 0x00, "SIG_CH_CTRL", false);
    wr(REG_SDOUT_SEL,   0x00, "SDOUT_SEL",   false);
    wr(REG_SAP_CTRL1, SAP_I2S_16BIT, "SAP_CTRL1", false);

    // GPIO1 -> FAULTZ output: needs BOTH the function select and the
    // output enable.
    wr(REG_GPIO_CTRL, GPIO1_OE, "GPIO_CTRL", false);
    wr(REG_GPIO1_SEL, GPIO_SEL_FAULTZ, "GPIO1_SEL", false);

    // Volume: 0x30 = 0 dB, each +1 = -0.5 dB.
    uint8_t vol = VOL_0DB_REG + 2 * BEEP_ATTEN_DB;
    LOGF("[tas] digital volume: -%d dB (reg 0x%02X)", BEEP_ATTEN_DB, vol);
    wr(REG_DIG_VOL, vol, "DIG_VOL", false);

    // Clear anything the config writes latched.
    wr(REG_FAULT_CLEAR, FAULT_CLEAR_CMD, "FAULT_CLEAR", true);
    vTaskDelay(pdMS_TO_TICKS(5));

    // FS_MON gate — poll until the detector reports a valid code.
    // 48 kHz reports 0x09 (Table 9-19; 0x04 would be 16 kHz!).
    LOGF("[tas] polling FS_MON for clock lock (expect 0x09 = 48 kHz)...");
    uint8_t fs = 0;
    bool locked = false;
    for (int elapsed = 0; elapsed <= 500; elapsed += 5) {
        i2c_rd_raw(REG_FS_MON, &fs);
        fs &= 0x0F;
        if (fs != FS_ERROR) { locked = true; break; }
        if (elapsed % 50 == 0) {
            LOGF("[tas]   FS_MON=0x%02X (%s) @ %d ms", fs, fs_mon_str(fs), elapsed);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!locked) {
        uint8_t cs = rdq(REG_CLKDET_STATUS);
        uint8_t bck = rdq(REG_BCK_MON);
        char dec[80];
        decode_clkdet(cs, dec, sizeof(dec));
        LOGF("[tas] ** FS_MON NEVER LOCKED — CLKDET_STATUS=0x%02X [%s] "
             "BCK_MON=%u — check BCLK(GPIO%d)/LRCLK(GPIO%d) to the codec **",
             cs, dec, bck, PIN_I2S_BCLK, PIN_I2S_LRCLK);
        return false;
    }
    LOGF("[tas] FS_MON LOCKED: 0x%02X (%s), BCK_MON=%u SCLKs/frame",
         fs, fs_mon_str(fs), rdq(REG_BCK_MON));

    // Release the DSP (clocks are proven now) into HiZ, still muted...
    wr(REG_DEVICE_CTRL2, CTRL_MUTE_BIT | CTRL_HIZ, "DEVICE_CTRL2", false);
    vTaskDelay(pdMS_TO_TICKS(20));
    // ...then PLAY, unmuted.
    wr(REG_DEVICE_CTRL2, CTRL_PLAY, "DEVICE_CTRL2", false);
    vTaskDelay(pdMS_TO_TICKS(20));
    // Post-PLAY clear — modulator inrush can blip PVDD_UV.
    wr(REG_FAULT_CLEAR, FAULT_CLEAR_CMD, "FAULT_CLEAR", true);
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t ps = rd(REG_POWER_STATE, "POWER_STATE");
    bool play = (ps == CTRL_PLAY);
    if (play) {
        LOGF("[tas] *** PLAY — codec is live, listen for the beep ***");
    } else {
        uint8_t g1 = rdq(REG_GLOBAL_FAULT1), chf = rdq(REG_CHAN_FAULT);
        char d1[64], d2[48];
        decode_gf1(g1, d1, sizeof(d1));
        decode_chf(chf, d2, sizeof(d2));
        LOGF("[tas] ** PLAY FAILED — POWER_STATE=0x%02X (%s), "
             "GLOBAL_FAULT1=0x%02X [%s] CHAN_FAULT=0x%02X [%s] **",
             ps, power_state_str(ps), g1, d1, chf, d2);
    }
    return play;
}

static void tas_dump_all(void) {
    LOGF("── register dump (book 0 page 0) ─────────────────────────────");
    select_book_page(0x00, 0x00);
    rd(REG_DEVICE_CTRL2,  "DEVICE_CTRL2");
    rd(REG_POWER_STATE,   "POWER_STATE");
    rd(REG_FS_MON,        "FS_MON");
    rd(REG_BCK_MON,       "BCK_MON");
    rd(REG_CLKDET_STATUS, "CLKDET_STATUS");
    rd(REG_SAP_CTRL1,     "SAP_CTRL1");
    rd(REG_DIG_VOL,       "DIG_VOL");
    rd(REG_AGAIN,         "AGAIN");
    rd(REG_PVDD_ADC,      "PVDD_ADC");
    rd(REG_GPIO_CTRL,     "GPIO_CTRL");
    rd(REG_GPIO1_SEL,     "GPIO1_SEL");
    rd(REG_DIE_ID,        "DIE_ID");
    rd(REG_AUTOMUTE_STATE,"AUTOMUTE_STATE");
    rd(REG_CHAN_FAULT,    "CHAN_FAULT");
    rd(REG_GLOBAL_FAULT1, "GLOBAL_FAULT1");
    rd(REG_GLOBAL_FAULT2, "GLOBAL_FAULT2");
    rd(REG_WARNING,       "WARNING");
}

// ─── Heartbeat: decoded status every 2 s + PLAY auto-recovery ─────────────

static void heartbeat(void) {
    static uint32_t beat = 0, recoveries = 0;
    beat++;

    uint8_t ps  = rdq(REG_POWER_STATE);
    uint8_t fs  = rdq(REG_FS_MON);
    uint8_t g1  = rdq(REG_GLOBAL_FAULT1);
    uint8_t g2  = rdq(REG_GLOBAL_FAULT2);
    uint8_t cf  = rdq(REG_CHAN_FAULT);
    uint8_t wn  = rdq(REG_WARNING);
    uint8_t cks = rdq(REG_CLKDET_STATUS);
    uint8_t pvdd = rdq(REG_PVDD_ADC);

    char d1[64], d2[48], d3[48];
    decode_gf1(g1, d1, sizeof(d1));
    decode_chf(cf, d2, sizeof(d2));
    decode_gf2(g2, d3, sizeof(d3));
    LOGF("[hb #%lu] POWER=%s FS=%s PVDD=%.1fV CLKDET=0x%02X | "
         "GF1=0x%02X[%s] CHF=0x%02X[%s] GF2=0x%02X[%s] WARN=0x%02X | "
         "beeps=%lu i2s_err=%lu recov=%lu",
         (unsigned long)beat, power_state_str(ps), fs_mon_str(fs),
         pvdd / 8.428f, cks,
         g1, d1, cf, d2, g2, d3, wn,
         (unsigned long)s_beep_cycles, (unsigned long)s_i2s_errors,
         (unsigned long)recoveries);

    // Auto-recovery: a latched fault knocked the codec out of PLAY.
    if (ps != CTRL_PLAY && ps != 0xFF) {
        recoveries++;
        LOGF("[hb] ** codec left PLAY — recovery #%lu: FAULT_CLEAR + "
             "HIZ->PLAY **", (unsigned long)recoveries);
        wr(REG_FAULT_CLEAR, FAULT_CLEAR_CMD, "FAULT_CLEAR", true);
        vTaskDelay(pdMS_TO_TICKS(5));
        wr(REG_DEVICE_CTRL2, CTRL_MUTE_BIT | CTRL_HIZ, "DEVICE_CTRL2", false);
        vTaskDelay(pdMS_TO_TICKS(20));
        wr(REG_DEVICE_CTRL2, CTRL_PLAY, "DEVICE_CTRL2", false);
        vTaskDelay(pdMS_TO_TICKS(20));
        wr(REG_FAULT_CLEAR, FAULT_CLEAR_CMD, "FAULT_CLEAR", true);
        uint8_t ps2 = rdq(REG_POWER_STATE);
        LOGF("[hb] recovery result: POWER=%s(0x%02X)", power_state_str(ps2), ps2);
    }
}

// ─── Entry point ──────────────────────────────────────────────────────────

void app_main(void) {
    printf("\n");
    LOGF("==============================================================");
    LOGF(" tas5825m_beep — TAS5825M bring-up probe (%s %s)", __DATE__, __TIME__);
    LOGF(" register map per TI SLASEH7H (datasheet = gold standard)");
    LOGF(" I2C SDA=%d SCL=%d | I2S BCLK=%d LRCLK=%d DOUT=%d | %d Hz 16-bit",
         PIN_I2C_SDA, PIN_I2C_SCL, PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT,
         SAMPLE_RATE_HZ);
    LOGF("==============================================================");

    i2c_init_and_scan();

    if (!tas_probe()) {
        LOGF("** TAS5825 NOT FOUND @ 0x%02X — check DVDD/wiring; halting "
             "with a rescan every 5 s **", TAS_ADDR);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            if (tas_probe()) { LOGF("** codec appeared — reboot to init **"); }
        }
    }

    // Phase 1 before clocks, then start I2S + the beep stream, then the
    // clock-dependent phase 2 — mirrors the production two-phase split.
    tas_phase1();
    i2s_init_tx();
    xTaskCreatePinnedToCore(beep_task, "beep", 4096, NULL, 5, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(50));   // let the first blocks hit the wire
    bool play = tas_phase2();

    tas_dump_all();
    if (!play) {
        LOGF("** init did NOT reach PLAY — heartbeat will keep retrying **");
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        heartbeat();
    }
}
