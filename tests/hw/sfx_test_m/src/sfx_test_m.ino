/**
 * SFX Test (M-variant) — TAS5825M Audio Codec Test Firmware
 *
 * Test firmware for verifying TAS5825M codec operation on the HubFX
 * board populated with the inductor-less Class-D + smart-amp silicon
 * (package suffix RHB). Generates a 440 Hz sine wave via I2S,
 * initializes the TAS5825M via I2C using an init sequence that
 * respects the M-specific guards the P silicon ignored, and prints
 * extensive codec diagnostics to Serial.
 *
 * What's different vs ../sfx_test_p/:
 *
 *   1. **Smart-amp / IV-sense default state** — the M ships with
 *      speaker-protection latch armed out of reset. We explicitly
 *      clear faults *before* programming registers and again after
 *      PLAY, and we leave DSP coefficients at their reset defaults
 *      so smart-amp isn't activated. (P silicon has no smart-amp.)
 *
 *   2. **FS_MON gating** — DEVICE_CTRL2 transition to PLAY only
 *      happens once register 0x37 reports a valid sample-rate code.
 *      The P auto-detected during the HIZ → PLAY transition; the M
 *      requires the lock first or it stays latched at HIZ.
 *
 *   3. **GPIO1_SEL explicit FAULT mode** — register 0x4F bit pattern
 *      0x01 is Hybrid-Pro on the P but RESERVED on the M. We program
 *      0x0B (Fault asserted, active-high) so the M's FAULT pin
 *      actually drives the expander input we're reading. Leaving
 *      it at the silicon default risks an undefined GPIO1 state.
 *
 *   4. **PVDD UVP** — the M trips undervoltage protection more
 *      eagerly than the P. We poll for UVP/OVP fault bits in
 *      diagnostics so a marginal supply shows up clearly.
 *
 * Sources for the M-only requirements:
 *   - TAS5825M datasheet §7.5.2  (clock detection)
 *   - TAS5825M Advanced Features (SLAA846) — smart-amp init
 *   - TI E2E forum: "firmware between TAS5825M and TAS5825P is different
 *     so you couldn't use the same TAS5825M process flows on a TAS5825P
 *     as it may not function properly"
 *
 * NO external libraries — only Arduino core + ESP-IDF headers.
 *
 * Hardware: ESP32-S3R8 (HubFX board, TAS5825M silicon)
 *
 * I2S Pin Mapping (wired to TAS5825M codec):
 *   GPIO1  — I2S Data Out (DIN on codec)
 *   GPIO4  — I2S Bit Clock (BCLK)
 *   GPIO3  — I2S Word Select / LR Clock (LRCLK)
 *
 * I2C Pin Mapping (codec + IO expander control):
 *   GPIO8  — I2C SDA
 *   GPIO9  — I2C SCL
 *
 * TAS5825M Codec: I2C address 0x4C
 *   - Power/mute controlled via PCAL6416A IO expander (0x20)
 *   - Sequence: Expander PDN+MUTE → Reset → Configure → Wait FS lock → PLAY
 *
 * Serial: 115200 baud on UART0 (USB-UART bridge)
 *
 * Architecture:
 *   Core 0 (Arduino loopTask): Codec init, serial diagnostics every 2s
 *   Core 1 (FreeRTOS task):    Sine wave generation → I2S DMA
 */

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <cmath>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_psram.h>

// ============================================================================
//  CONFIGURATION
// ============================================================================

// I2S pins (match HubFX board wiring)
static constexpr gpio_num_t PIN_I2S_DOUT  = GPIO_NUM_1;
static constexpr gpio_num_t PIN_I2S_BCLK  = GPIO_NUM_4;
static constexpr gpio_num_t PIN_I2S_LRCLK = GPIO_NUM_3;

// I2C pins (codec + IO expander)
static constexpr int PIN_I2C_SDA = 8;
static constexpr int PIN_I2C_SCL = 9;

// I2C addresses
static constexpr uint8_t TAS5825M_ADDR  = 0x4C;
static constexpr uint8_t PCAL6416A_ADDR = 0x20;

// LED
static constexpr int PIN_LED = 48;

// Audio parameters
static constexpr uint32_t SAMPLE_RATE     = 48000;   // Hz
static constexpr uint32_t BIT_DEPTH       = 16;
static constexpr float    SINE_FREQ_HZ    = 440.0f;  // A4 note
static constexpr float    AMPLITUDE        = 0.8f;    // 0.0–1.0 (80% to avoid clipping)
static constexpr size_t   FRAMES_PER_BATCH = 512;     // Frames per I2S write

// Diagnostics interval
static constexpr uint32_t DIAG_INTERVAL_MS = 2000;

// FS_MON polling parameters (M-specific guard)
//   The M needs a valid sample-rate code in register 0x37 BEFORE the
//   HIZ → PLAY transition; the P auto-detected during the transition.
static constexpr uint32_t FS_MON_POLL_INTERVAL_MS = 5;
static constexpr uint32_t FS_MON_POLL_TIMEOUT_MS  = 500;

// ============================================================================
//  PCAL6416A IO EXPANDER REGISTERS
// ============================================================================

static constexpr uint8_t PCAL_OUTPUT_0 = 0x02;
static constexpr uint8_t PCAL_OUTPUT_1 = 0x03;
static constexpr uint8_t PCAL_CONFIG_0 = 0x06;  // Port 0 direction (1=input, 0=output)
static constexpr uint8_t PCAL_CONFIG_1 = 0x07;  // Port 1 direction

// Expander Port 1 bit assignments
static constexpr uint8_t EXP_FAULT_BIT = 0;  // P1_0 — TAS5825M nFAULT (input, active-low)
static constexpr uint8_t EXP_MUTE_BIT  = 1;  // P1_1 — TAS5825M MUTE (output, HIGH=unmuted)
static constexpr uint8_t EXP_PDN_BIT   = 2;  // P1_2 — TAS5825M PDN (output, HIGH=run)

// ============================================================================
//  TAS5825M CODEC REGISTERS
// ============================================================================

static constexpr uint8_t TAS_REG_PAGE        = 0x00;
static constexpr uint8_t TAS_REG_RESET       = 0x01;
static constexpr uint8_t TAS_REG_MODE_CTRL   = 0x02;
static constexpr uint8_t TAS_REG_DEVICE_CTRL = 0x03;
static constexpr uint8_t TAS_REG_SIG_CH_CTRL = 0x28;
static constexpr uint8_t TAS_REG_SDOUT_SEL   = 0x30;
static constexpr uint8_t TAS_REG_CLK_SRC     = 0x33;
static constexpr uint8_t TAS_REG_FS_RATE     = 0x34;
static constexpr uint8_t TAS_REG_FS_MON      = 0x37;
static constexpr uint8_t TAS_REG_ANALOG_CTRL = 0x46;
static constexpr uint8_t TAS_REG_DIGITAL_VOL = 0x4C;
// GPIO1_SEL (0x4F) — bit pattern selects what GPIO1 outputs:
//   0x00 = Off (Hi-Z)        0x07 = Volume Ramp Done
//   0x01 = Reserved (M only) 0x08 = Mute Status
//          [P uses for HPFB] 0x09 = Limiter Active
//   0x02 = AutoMute Flag     0x0A = Sub-channel data
//   0x03 = AutoMute (L)      0x0B = Fault asserted (active high)  ← we use
//   0x04 = AutoMute (R)
//   0x05 = Clock Invalid
//   0x06 = SDOUT
static constexpr uint8_t TAS_REG_GPIO1_SEL    = 0x4F;
static constexpr uint8_t TAS_GPIO1_FAULT      = 0x0B;
static constexpr uint8_t TAS_REG_AGAIN_L     = 0x53;
static constexpr uint8_t TAS_REG_AGAIN_R     = 0x54;
static constexpr uint8_t TAS_REG_CLK_CFG     = 0x60;  // SAP_CTRL_1
static constexpr uint8_t TAS_REG_DSP_MISC    = 0x62;
static constexpr uint8_t TAS_REG_POWER_STATE = 0x68;
static constexpr uint8_t TAS_REG_AUTOMUTE    = 0x69;
static constexpr uint8_t TAS_REG_CHAN_FAULT  = 0x70;
static constexpr uint8_t TAS_REG_GLOBAL1     = 0x71;
static constexpr uint8_t TAS_REG_GLOBAL2     = 0x72;
static constexpr uint8_t TAS_REG_OT_WARNING  = 0x73;
static constexpr uint8_t TAS_REG_FAULT_CLEAR = 0x78;
static constexpr uint8_t TAS_REG_BOOK        = 0x7F;

static constexpr uint8_t TAS_CTRL_DEEP_SLEEP = 0x00;
static constexpr uint8_t TAS_CTRL_SLEEP      = 0x01;
static constexpr uint8_t TAS_CTRL_HIZ        = 0x02;
static constexpr uint8_t TAS_CTRL_PLAY       = 0x03;
static constexpr uint8_t TAS_CTRL_MUTE_BIT   = 0x08;  // OR with mode → muted

// ============================================================================
//  TYPES
// ============================================================================

struct StereoSample {
    int16_t left;
    int16_t right;
};

// ============================================================================
//  GLOBALS
// ============================================================================

static i2s_chan_handle_t i2sHandle = nullptr;
static TaskHandle_t      audioTaskHandle = nullptr;

static std::atomic<uint32_t> totalFramesWritten{0};
static std::atomic<uint32_t> totalWrites{0};
static std::atomic<uint32_t> writeErrors{0};
static std::atomic<bool>     i2sRunning{false};
static std::atomic<uint32_t> audioTaskLoops{0};
static std::atomic<int16_t>  lastPeakSample{0};
static std::atomic<bool>     codecInPlay{false};
static std::atomic<uint32_t> fsLockTime_ms{0};

static float sinePhase = 0.0f;
static StereoSample batchBuffer[FRAMES_PER_BATCH];

// ============================================================================
//  I2C HELPERS
// ============================================================================

static bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static uint8_t i2cReadReg(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(addr, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

static bool i2cProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// ============================================================================
//  PCAL6416A IO EXPANDER INIT (identical to P-variant)
// ============================================================================

static bool initExpander() {
    Serial.printf("[EXP] Probing PCAL6416A @ 0x%02X... ", PCAL6416A_ADDR);
    if (!i2cProbe(PCAL6416A_ADDR)) {
        Serial.println("NOT FOUND");
        return false;
    }
    Serial.println("OK");

    uint8_t dir1 = 0xF9;  // 1111_1001 — P1_0 input, P1_1+P1_2 output
    i2cWriteReg(PCAL6416A_ADDR, PCAL_CONFIG_1, dir1);

    uint8_t out1 = (1 << EXP_PDN_BIT) | (1 << EXP_MUTE_BIT);  // 0x06 — PDN+MUTE high
    i2cWriteReg(PCAL6416A_ADDR, PCAL_OUTPUT_1, out1);

    uint8_t readDir = i2cReadReg(PCAL6416A_ADDR, PCAL_CONFIG_1);
    uint8_t readOut = i2cReadReg(PCAL6416A_ADDR, PCAL_OUTPUT_1);
    Serial.printf("[EXP] Port 1: dir=0x%02X (expect 0x%02X), out=0x%02X (expect 0x%02X)\n",
                  readDir, dir1, readOut, out1);

    uint8_t pins = i2cReadReg(PCAL6416A_ADDR, 0x01);
    Serial.printf("[EXP] Port 1 input: 0x%02X\n", pins);
    Serial.printf("[EXP]   PDN  (P1_2): %s\n", (pins & (1 << EXP_PDN_BIT)) ? "HIGH (run)" : "LOW (!!)");
    Serial.printf("[EXP]   MUTE (P1_1): %s\n", (pins & (1 << EXP_MUTE_BIT)) ? "HIGH (unmuted)" : "LOW (!!)");
    Serial.printf("[EXP]   FAULT(P1_0): %s\n", (pins & (1 << EXP_FAULT_BIT)) ? "HIGH (ok)" : "LOW (FAULT!)");
    return true;
}

// ============================================================================
//  TAS5825M CODEC INIT — M-SPECIFIC SEQUENCE
// ============================================================================

static void tasSelectBookPage(uint8_t book, uint8_t page) {
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_PAGE, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_BOOK, book);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_PAGE, page);
}

/**
 * Phase 1 (before I2S clocks): Reset the codec into Deep Sleep + clear
 * any latched smart-amp fault state. Identical surface to the P
 * variant but we ALSO clear faults before configuring — a freshly
 * powered M will have OT/UV/CDET latched even though nothing has gone
 * wrong, and the CDET latch will block a later HIZ entry.
 */
static bool initCodecPreClock() {
    Serial.printf("[TAS] Probing TAS5825M @ 0x%02X... ", TAS5825M_ADDR);
    if (!i2cProbe(TAS5825M_ADDR)) {
        Serial.println("NOT FOUND");
        return false;
    }
    Serial.println("OK");

    tasSelectBookPage(0x00, 0x00);

    // M-specific: park in Deep Sleep with the MUTE bit set so the DAC
    // stays silent during register init. The combined value 0x08 means
    // mode=DEEP_SLEEP + MUTE bit asserted.
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL,
                TAS_CTRL_DEEP_SLEEP | TAS_CTRL_MUTE_BIT);
    delay(5);

    // Full reset (registers + DSP) — re-arms factory defaults.
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_RESET, 0x11);
    delay(50);

    // Re-park in Deep Sleep + muted after reset (reset itself moves the
    // chip to a default mode that varies between silicon revisions).
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL,
                TAS_CTRL_DEEP_SLEEP | TAS_CTRL_MUTE_BIT);

    // M-specific: clear any latched fault bits BEFORE we touch any
    // configuration registers. On the M these can include CDET (clock
    // detect) which fires whenever clocks have been absent — i.e. it
    // is asserted RIGHT NOW and would block our HIZ transition later.
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_FAULT_CLEAR, 0x80);
    delay(5);

    uint8_t pwr = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    Serial.printf("[TAS] After reset+clear: POWER_STATE=0x%02X (expect 0x00=Deep Sleep)\n", pwr);
    return true;
}

/**
 * Wait for FS_MON to report a valid sample-rate code. Returns true once
 * a non-zero code appears within FS_MON_POLL_TIMEOUT_MS, false on
 * timeout. This is the M-specific guard the P didn't need — without
 * it, writing PLAY while FS_MON is still 0 leaves the chip latched at
 * HIZ in the M.
 */
static bool waitForFsLock(uint8_t* outCode) {
    Serial.printf("[TAS] Polling FS_MON (timeout %lums, interval %lums)...",
                  FS_MON_POLL_TIMEOUT_MS, FS_MON_POLL_INTERVAL_MS);
    uint32_t start = millis();
    while (true) {
        uint8_t fs = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON) & 0x0F;
        if (fs != 0x00 && fs != 0x0F) {
            uint32_t elapsed = millis() - start;
            Serial.printf(" LOCK after %lums (FS_MON=0x%02X)\n", elapsed, fs);
            fsLockTime_ms.store(elapsed, std::memory_order_relaxed);
            if (outCode) *outCode = fs;
            return true;
        }
        if (millis() - start > FS_MON_POLL_TIMEOUT_MS) {
            Serial.printf(" TIMEOUT — FS_MON stuck at 0x%02X\n", fs);
            if (outCode) *outCode = fs;
            return false;
        }
        delay(FS_MON_POLL_INTERVAL_MS);
    }
}

/**
 * Phase 2 (after I2S clocks are running and stable): Configure
 * registers, wait for clock lock, then PLAY. Three M-specific steps
 * vs the P-variant flow:
 *   - GPIO1_SEL is explicitly set to FAULT (the P used Hybrid-Pro
 *     here; that bit pattern is RESERVED on the M).
 *   - FS_MON is polled before HIZ → PLAY (M won't auto-transition).
 *   - Faults are cleared once before config and once after PLAY (M
 *     latches CDET/UVP harder than P).
 */
static bool initCodecPostClock() {
    Serial.println("[TAS] === Post-clock codec configuration (M-variant) ===");

    uint8_t prePwr = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    uint8_t preFs  = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
    Serial.printf("[TAS] Pre-config: PWR=0x%02X  FS_MON=0x%02X\n", prePwr, preFs);

    // Step 1: Analog gain (12V supply) — same as P
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_ANALOG_CTRL, 0x11);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_MODE_CTRL, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_AGAIN_L, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_AGAIN_R, 0x00);
    Serial.println("[TAS] Analog gain configured (12V)");

    // Step 2: Clock auto-detect + I2S 16-bit format
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_CLK_SRC, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_FS_RATE, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_SDOUT_SEL, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_CLK_CFG, 0x00);  // SAP_CTRL_1: I2S, 16-bit
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DSP_MISC, 0x09);

    // Step 3: M-SPECIFIC — set GPIO1_SEL explicitly. The default value
    // varies by silicon revision (some lots ship with the value the P
    // uses for Hybrid-Pro feedback, which is RESERVED on the M). We
    // route GPIO1 to "FAULT asserted, active high" so the FAULT bit
    // we read on the expander reflects actual codec faults.
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_GPIO1_SEL, TAS_GPIO1_FAULT);
    Serial.printf("[TAS] GPIO1_SEL = 0x%02X (FAULT) — M-specific override\n",
                  TAS_GPIO1_FAULT);

    uint8_t r33 = i2cReadReg(TAS5825M_ADDR, TAS_REG_CLK_SRC);
    uint8_t r34 = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_RATE);
    uint8_t r60 = i2cReadReg(TAS5825M_ADDR, TAS_REG_CLK_CFG);
    uint8_t r4F = i2cReadReg(TAS5825M_ADDR, TAS_REG_GPIO1_SEL);
    Serial.printf("[TAS] Clock regs: 0x33=0x%02X  0x34=0x%02X  0x60=0x%02X  0x4F=0x%02X\n",
                  r33, r34, r60, r4F);

    // Step 4: Volume — 0 dB
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DIGITAL_VOL, 0x30);

    // Step 5: M-SPECIFIC — clear any faults that built up while clocks
    // were absent (CDET in particular). Without this, FS_MON may show
    // a valid lock but the chip refuses HIZ until the latch clears.
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_FAULT_CLEAR, 0x80);
    delay(5);

    // Step 6: M-SPECIFIC — wait for FS_MON to report a valid rate
    // BEFORE the HIZ transition. The P would auto-detect during the
    // transition; the M won't.
    uint8_t fsCode = 0;
    if (!waitForFsLock(&fsCode)) {
        Serial.println("[TAS] *** FAILED: clocks never locked — check I2S BCLK/LRCLK wiring ***");
        return false;
    }

    // Step 7: Deep Sleep → HIZ (PLL stabilizes during this transition)
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_HIZ);
    delay(20);

    uint8_t pwr1 = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    Serial.printf("[TAS] After HIZ: PWR=0x%02X  FS_MON=0x%02X\n",
                  pwr1, i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON));
    if ((pwr1 & 0x0F) != TAS_CTRL_HIZ) {
        Serial.println("[TAS] *** WARN: HIZ transition incomplete — check fault registers ***");
        // Continue anyway — sometimes POWER_STATE lags DEVICE_CTRL by a tick.
    }

    // Step 8: HIZ → PLAY
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_PLAY);
    delay(20);

    // Step 9: M-SPECIFIC — clear post-PLAY transient faults (PVDD UVP
    // can fire briefly while the modulator ramps up).
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_FAULT_CLEAR, 0x80);
    delay(50);

    uint8_t pwr2 = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    uint8_t fault = i2cReadReg(TAS5825M_ADDR, TAS_REG_GLOBAL1);
    Serial.printf("[TAS] After PLAY: PWR=0x%02X  GLOBAL1_FAULT=0x%02X\n", pwr2, fault);

    if ((pwr2 & 0x0F) == TAS_CTRL_PLAY) {
        Serial.println("[TAS] *** SUCCESS: CODEC IS IN PLAY MODE ***");
        return true;
    }

    Serial.println("[TAS] *** FAILED: Codec won't enter PLAY ***");
    Serial.println("[TAS]   Read fault registers below for the reason. Common M-specific causes:");
    Serial.println("[TAS]     - GLOBAL1 bit 7 (CLK_FAULT): clock invalid — re-check FS_MON gating");
    Serial.println("[TAS]     - GLOBAL1 bit 6 (PVDD_OVP):  supply too high");
    Serial.println("[TAS]     - GLOBAL1 bit 5 (PVDD_UVP):  supply too low");
    Serial.println("[TAS]     - GLOBAL1 bit 0 (OTP):       overtemperature");
    return false;
}

// ============================================================================
//  TAS5825M DIAGNOSTICS — M-VARIANT (decodes M-only fault bits)
// ============================================================================

static void dumpCodecDiag() {
    tasSelectBookPage(0x00, 0x00);

    uint8_t devCtrl    = i2cReadReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL);
    uint8_t powerState = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    uint8_t fsMon      = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
    uint8_t automute   = i2cReadReg(TAS5825M_ADDR, TAS_REG_AUTOMUTE);
    uint8_t digVol     = i2cReadReg(TAS5825M_ADDR, TAS_REG_DIGITAL_VOL);
    uint8_t clkCfg     = i2cReadReg(TAS5825M_ADDR, TAS_REG_CLK_CFG);
    uint8_t gpio1Sel   = i2cReadReg(TAS5825M_ADDR, TAS_REG_GPIO1_SEL);
    uint8_t chanFault  = i2cReadReg(TAS5825M_ADDR, TAS_REG_CHAN_FAULT);
    uint8_t global1    = i2cReadReg(TAS5825M_ADDR, TAS_REG_GLOBAL1);
    uint8_t global2    = i2cReadReg(TAS5825M_ADDR, TAS_REG_GLOBAL2);
    uint8_t otWarn     = i2cReadReg(TAS5825M_ADDR, TAS_REG_OT_WARNING);

    uint8_t expPins = i2cReadReg(PCAL6416A_ADDR, 0x01);

    const char* powerStr = "???";
    switch (powerState & 0x0F) {
        case 0x00: powerStr = "DEEP_SLEEP"; break;
        case 0x01: powerStr = "SLEEP"; break;
        case 0x02: powerStr = "HIZ"; break;
        case 0x03: powerStr = "PLAY"; break;
    }

    const char* fsStr = "???";
    switch (fsMon & 0x0F) {
        case 0x00: fsStr = "NONE"; break;
        case 0x01: fsStr = "8kHz"; break;
        case 0x02: fsStr = "16kHz"; break;
        case 0x03: fsStr = "32kHz"; break;
        case 0x04: fsStr = "48kHz"; break;
        case 0x05: fsStr = "96kHz"; break;
        case 0x06: fsStr = "44.1kHz"; break;
        case 0x07: fsStr = "88.2kHz"; break;
        case 0x08: fsStr = "176.4kHz"; break;
        case 0x09: fsStr = "192kHz"; break;
        case 0x0F: fsStr = "INVALID"; break;
    }

    uint32_t frames = totalFramesWritten.load(std::memory_order_relaxed);
    uint32_t errors = writeErrors.load(std::memory_order_relaxed);

    Serial.println("==== TAS5825M DIAGNOSTICS (M-variant) ====");
    Serial.printf("  Expander: PDN=%s  MUTE=%s  FAULT=%s\n",
        (expPins & (1 << EXP_PDN_BIT)) ? "HIGH" : "LOW",
        (expPins & (1 << EXP_MUTE_BIT)) ? "HIGH" : "LOW",
        (expPins & (1 << EXP_FAULT_BIT)) ? "HIGH" : "LOW");
    Serial.printf("  DEVICE_CTRL:  0x%02X → %s%s\n",
                  devCtrl,
                  (devCtrl & 0x03) == 0x03 ? "PLAY" :
                  (devCtrl & 0x03) == 0x02 ? "HIZ" :
                  (devCtrl & 0x03) == 0x01 ? "SLEEP" : "DEEP_SLEEP",
                  (devCtrl & TAS_CTRL_MUTE_BIT) ? " +MUTE" : "");
    Serial.printf("  POWER_STATE:  0x%02X → %s\n", powerState, powerStr);
    Serial.printf("  FS_MON:       0x%02X → %s (lock took %lums)\n",
                  fsMon, fsStr, fsLockTime_ms.load(std::memory_order_relaxed));
    Serial.printf("  CLK_CFG:      0x%02X\n", clkCfg);
    Serial.printf("  GPIO1_SEL:    0x%02X (expect 0x%02X = FAULT)\n",
                  gpio1Sel, TAS_GPIO1_FAULT);
    Serial.printf("  DIGITAL_VOL:  0x%02X (%.1f dB)\n", digVol, (digVol - 0x30) * 0.5f);
    Serial.printf("  AUTOMUTE:     0x%02X\n", automute);
    Serial.printf("  CHAN_FAULT:   0x%02X\n", chanFault);
    Serial.printf("  GLOBAL1:      0x%02X", global1);
    if (global1) {
        Serial.print("  [");
        if (global1 & 0x80) Serial.print("CLK ");
        if (global1 & 0x40) Serial.print("OVP ");
        if (global1 & 0x20) Serial.print("UVP ");
        if (global1 & 0x10) Serial.print("DC ");
        if (global1 & 0x08) Serial.print("OC ");
        if (global1 & 0x04) Serial.print("OTP ");
        Serial.print("]");
    }
    Serial.println();
    Serial.printf("  GLOBAL2:      0x%02X\n", global2);
    Serial.printf("  OT_WARNING:   0x%02X\n", otWarn);
    Serial.printf("  I2S frames:   %lu  errors: %lu\n", frames, errors);
    if ((powerState & 0x0F) != 0x03) {
        Serial.println("  *** WARNING: NOT IN PLAY STATE ***");
    }
    if (chanFault || global1 || global2) {
        Serial.println("  *** FAULT ACTIVE — see GLOBAL1 decode above ***");
    }
    Serial.println("==========================================");
}

// ============================================================================
//  SINE WAVE GENERATION (identical to P-variant)
// ============================================================================

static void generateSineBatch() {
    const float phaseIncrement = (2.0f * M_PI * SINE_FREQ_HZ) / (float)SAMPLE_RATE;
    const float scale = AMPLITUDE * 32767.0f;
    int16_t peak = 0;

    for (size_t i = 0; i < FRAMES_PER_BATCH; i++) {
        int16_t sample = (int16_t)(sinf(sinePhase) * scale);
        batchBuffer[i].left  = sample;
        batchBuffer[i].right = sample;
        int16_t absSample = (sample < 0) ? -sample : sample;
        if (absSample > peak) peak = absSample;
        sinePhase += phaseIncrement;
    }
    if (sinePhase >= 2.0f * M_PI) {
        sinePhase -= 2.0f * M_PI;
    }
    lastPeakSample.store(peak, std::memory_order_relaxed);
}

// ============================================================================
//  I2S SETUP (identical to P-variant — same ESP32-S3 hardware)
// ============================================================================

static bool initI2S() {
    Serial.println("[I2S] Initializing ESP-IDF v5.x standard-mode driver...");
    Serial.printf("[I2S] Pins: DOUT=%d, BCLK=%d, LRCLK=%d\n",
                  PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK);
    Serial.printf("[I2S] Format: %lu Hz, %lu-bit, stereo\n", SAMPLE_RATE, BIT_DEPTH);

    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num  = 8;
    chanCfg.dma_frame_num = 512;
    chanCfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chanCfg, &i2sHandle, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: i2s_new_channel: %s (0x%x)\n", esp_err_to_name(err), err);
        return false;
    }

    i2s_std_config_t stdCfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    err = i2s_channel_init_std_mode(i2sHandle, &stdCfg);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: init_std_mode: %s (0x%x)\n", esp_err_to_name(err), err);
        i2s_del_channel(i2sHandle); i2sHandle = nullptr;
        return false;
    }

    err = i2s_channel_enable(i2sHandle);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: channel_enable: %s (0x%x)\n", esp_err_to_name(err), err);
        i2s_del_channel(i2sHandle); i2sHandle = nullptr;
        return false;
    }
    Serial.println("[I2S] Channel enabled — clocks running");
    Serial.printf("[I2S] BCLK frequency: %lu Hz\n", SAMPLE_RATE * BIT_DEPTH * 2);
    return true;
}

static void audioTask(void* param) {
    Serial.printf("[Audio] Task on Core %d (priority %d)\n",
                  xPortGetCoreID(), uxTaskPriorityGet(nullptr));
    if (!initI2S()) {
        Serial.println("[Audio] FATAL: I2S init failed");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    i2sRunning.store(true, std::memory_order_release);

    Serial.println("[Audio] Pre-filling DMA pipeline (4 batches)...");
    for (int i = 0; i < 4; i++) {
        generateSineBatch();
        size_t bytesWritten = 0;
        i2s_channel_write(i2sHandle, batchBuffer,
                          FRAMES_PER_BATCH * sizeof(StereoSample),
                          &bytesWritten, portMAX_DELAY);
    }

    Serial.printf("[Audio] %.1f Hz sine, %.0f%% amplitude, %d frames/batch (%.1fms)\n",
                  SINE_FREQ_HZ, AMPLITUDE * 100.0f, FRAMES_PER_BATCH,
                  (float)FRAMES_PER_BATCH / SAMPLE_RATE * 1000.0f);

    while (true) {
        audioTaskLoops.fetch_add(1, std::memory_order_relaxed);
        generateSineBatch();
        size_t bytesWritten = 0;
        esp_err_t err = i2s_channel_write(i2sHandle, batchBuffer,
                                           FRAMES_PER_BATCH * sizeof(StereoSample),
                                           &bytesWritten, portMAX_DELAY);
        uint32_t framesWritten = bytesWritten / sizeof(StereoSample);
        totalFramesWritten.fetch_add(framesWritten, std::memory_order_relaxed);
        totalWrites.fetch_add(1, std::memory_order_relaxed);
        if (err != ESP_OK || framesWritten != FRAMES_PER_BATCH) {
            writeErrors.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ============================================================================
//  BANNER + SYSTEM INFO
// ============================================================================

static const char* resetReasonStr(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWER_ON";
        case ESP_RST_EXT:       return "EXTERNAL";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        default:                return "UNKNOWN";
    }
}

static void printBanner() {
    Serial.println();
    Serial.println("================================================================");
    Serial.println("  SFX Test (M-variant) — TAS5825M Sine Wave I2S Test");
    Serial.println("  ESP32-S3 / HubFX board  |  Inductor-less Class-D + smart-amp");
    Serial.println("  Init flow: smart-amp clear, FS_MON gate, GPIO1_SEL override");
    Serial.println("  For TAS5825P boards use ../sfx_test_p/ instead");
    Serial.println("================================================================");
    Serial.println();
}

static void printSystemInfo() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    Serial.println("--- System Info ---");
    Serial.printf("  Chip:       ESP32-S3 rev %d, %d core(s), %d MHz\n",
                  chip.revision, chip.cores, ESP.getCpuFreqMHz());
    Serial.printf("  Flash:      %lu MB\n", ESP.getFlashChipSize() / (1024*1024));
    Serial.printf("  PSRAM:      %d bytes\n", ESP.getPsramSize());
    Serial.printf("  Free heap:  %lu bytes\n", ESP.getFreeHeap());
    Serial.printf("  SDK:        %s\n", ESP.getSdkVersion());
    Serial.printf("  Reset:      %s\n", resetReasonStr(esp_reset_reason()));
    Serial.println();
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================

void setup() {
    Serial.begin(115200);
    uint32_t serialWaitStart = millis();
    while (!Serial && (millis() - serialWaitStart < 3000)) delay(10);

    printBanner();
    printSystemInfo();

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);

    Serial.printf("[I2C] Init: SDA=GPIO%d, SCL=GPIO%d, 100kHz\n", PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);

    Serial.println("[I2C] Bus scan:");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C]   Found device at 0x%02X\n", addr);
        }
    }

    if (!initExpander()) {
        Serial.println("*** WARN: Expander init failed — TAS5825M won't have power ***");
    }
    delay(50);

    bool codecFound = initCodecPreClock();
    if (!codecFound) {
        Serial.println("*** WARN: Codec not found — audio output will be I2S only ***");
    }

    Serial.println("[Main] Launching audio task on Core 1...");
    BaseType_t result = xTaskCreatePinnedToCore(
        audioTask, "AudioSine", 8192, nullptr,
        configMAX_PRIORITIES - 1, &audioTaskHandle, 1);
    if (result != pdPASS) {
        Serial.println("[Main] FATAL: Failed to create audio task!");
    }

    uint32_t waitStart = millis();
    while (!i2sRunning.load(std::memory_order_acquire)) {
        delay(50);
        if (millis() - waitStart > 5000) {
            Serial.println("[Main] WARN: Timeout waiting for I2S init");
            break;
        }
    }

    if (codecFound) {
        delay(200);
        bool codecOk = initCodecPostClock();
        codecInPlay.store(codecOk, std::memory_order_release);
        Serial.println();
        if (codecOk) {
            Serial.println("*** CODEC IN PLAY MODE — YOU SHOULD HEAR A 440Hz TONE ***");
        } else {
            Serial.println("*** CODEC FAILED TO ENTER PLAY — sine wave on I2S only ***");
            dumpCodecDiag();
        }
        Serial.println();
    }

    Serial.println("================================================================");
    Serial.printf("[Main] Diagnostics every %lu ms\n", DIAG_INTERVAL_MS);
    Serial.println("================================================================");
    Serial.println();
}

static uint32_t lastDiagTime_ms   = 0;
static uint32_t lastFrameSnapshot = 0;
static uint32_t diagCount         = 0;

void loop() {
    uint32_t now = millis();

    static uint32_t ledToggle = 0;
    if (now - ledToggle > 500) {
        ledToggle = now;
        digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    }

    if (now - lastDiagTime_ms < DIAG_INTERVAL_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }
    lastDiagTime_ms = now;
    diagCount++;

    uint32_t frames = totalFramesWritten.load(std::memory_order_relaxed);
    uint32_t errors = writeErrors.load(std::memory_order_relaxed);
    int16_t  peak   = lastPeakSample.load(std::memory_order_relaxed);
    bool     running = i2sRunning.load(std::memory_order_acquire);

    uint32_t deltaFrames = frames - lastFrameSnapshot;
    lastFrameSnapshot = frames;
    float frameRate = (float)deltaFrames / ((float)DIAG_INTERVAL_MS / 1000.0f);
    float peakDb    = (peak > 0) ? 20.0f * log10f((float)peak / 32767.0f) : -96.0f;

    if ((diagCount % 10) == 1) {
        Serial.println();
        Serial.println("  #   Uptime   I2S   Frames/s   Total       Errors  Peak dBFS  Heap");
        Serial.println("---  -------  -----  --------   --------    ------  ---------  ------");
    }
    Serial.printf("%3lu  %6.1fs  %-5s  %8.0f   %8lu    %6lu   %5.1f     %5luK\n",
                  diagCount, now / 1000.0f,
                  running ? "OK" : "DOWN",
                  frameRate, frames, errors, peakDb,
                  ESP.getFreeHeap() / 1024);

    if ((diagCount % 15) == 0) {
        Serial.println();
        dumpCodecDiag();
        Serial.println();
    }
}
