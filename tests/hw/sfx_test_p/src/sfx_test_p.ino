/**
 * @file sfx_test_p.ino
 * @brief Standalone TAS5825P bring-up: 440 Hz I²S sine + codec diagnostics.
 *
 * Target chip:
 *   Texas Instruments TAS5825P (Class-H + Hybrid-Pro variant, QFN-32 RHB).
 *   I²C-controlled stereo Class-D amplifier sharing the page-0/book-0
 *   register layout with the TAS5825M but using a permissive clock-detect
 *   path — no smart-amp speaker-protection latch, no FS_MON validity gate
 *   before HIZ → PLAY. For TAS5825M silicon use ../sfx_test_m/ instead.
 *
 * Target host: ESP32-S3R8 on the HubFX PCB.
 *   - PDN and MUTE are statically pulled HIGH via 10 kΩ resistors on this
 *     PCB revision; no expander or ESP32 GPIO drives them. The codec is
 *     hardware-enabled and unmuted from power-on; mode control is purely
 *     via DEVICE_CTRL2 (0x03) over I²C.
 *
 * Init sequence (per TAS5825P datasheet §8.4 power-up flow):
 *   1. I²C probe + RESET (0x01 ← 0x11) + DEVICE_CTRL2 ← DEEP_SLEEP.
 *   2. Start I²S TX (BCLK + LRCLK active so the codec's clock-detect has
 *      something to lock to).
 *   3. Write ANA_CTRL, AGAIN_L/R for the target supply, then
 *      CLK_SRC = 0x00 (auto), FS_RATE = 0x00 (auto), SDOUT_SEL = 0x00,
 *      SAP_CTRL1 ← word length matching the I²S TX bit depth, DSP_MISC.
 *   4. DEVICE_CTRL2 ← HIZ. PLL locks here (FS_MON becomes non-zero).
 *   5. DEVICE_CTRL2 ← PLAY. POWER_STATE (0x68) tracks the requested mode.
 *
 * Bus wiring:
 *   I²C: SDA=GPIO8, SCL=GPIO9, 100 kHz. Codec at 7-bit address 0x4C.
 *   I²S: DOUT=GPIO1, BCLK=GPIO4, LRCLK/WS=GPIO3, no MCLK (codec auto-
 *        derives PLL from BCK).
 *
 * Serial:
 *   115200 baud on UART0 (USB-UART bridge — WCH CH343 on the dev PCB).
 *
 * Architecture:
 *   Core 0 (Arduino loopTask): I²C transactions, codec config, diagnostics.
 *   Core 1 (FreeRTOS task):    I²S DMA pipeline + sine wave generation.
 *
 * Build: `pio run -t upload` from this directory.
 *
 * Datasheet references (italics = field name from the datasheet):
 *   §7.5  Register map — page 0 / book 0
 *   §7.6  Detailed register descriptions
 *   §8.4  Initialization flow / clock-detect
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
//  PIN / BUS / AUDIO CONFIG
// ============================================================================

// I²S pins (HubFX PCB rev with 8 LED channels + PCA9685 expander).
// Verified against EasyEDA netlist (May 2026):
//   U26.22 = GPIO16 → TAS5825P DIN  (TAS_DI net)
//   U26.23 = GPIO17 → TAS5825P BCK  (TAS_BCK net)
//   U26.24 = GPIO18 → TAS5825P FS   (TAS_FS net  = LRCLK)
// The earlier GPIO1/4/3 assignment was for the original prototype rev
// and on this rev clashes with PPM inputs IN_10 / IN_11.
static constexpr gpio_num_t PIN_I2S_DOUT  = GPIO_NUM_16;
static constexpr gpio_num_t PIN_I2S_BCLK  = GPIO_NUM_17;
static constexpr gpio_num_t PIN_I2S_LRCLK = GPIO_NUM_18;

// I²C pins (HubFX PCB)
static constexpr int PIN_I2C_SDA = 8;
static constexpr int PIN_I2C_SCL = 9;
static constexpr uint32_t I2C_CLOCK_HZ = 100000;

// LED heartbeat
static constexpr int PIN_LED = 48;

// Audio parameters — 16-bit stereo PCM, 48 kHz.
static constexpr uint32_t SAMPLE_RATE      = 48000;
static constexpr uint32_t BIT_DEPTH        = 16;
static constexpr float    SINE_FREQ_HZ     = 440.0f;
static constexpr float    AMPLITUDE        = 0.8f;
static constexpr size_t   FRAMES_PER_BATCH = 512;

// Diagnostic cadence.
static constexpr uint32_t DIAG_INTERVAL_MS    = 2000;  // compact line
static constexpr uint32_t VERBOSE_DUMP_EVERY  = 15;    // verbose dump every N ticks (30 s)

// ============================================================================
//  TAS5825P REGISTER MAP
//  Source: TI TAS5825P datasheet §7.6 (matches sfx_audio/codec/tas5825_regs.h)
// ============================================================================

// 7-bit I²C address on the HubFX PCB (ADDR pin → GND).
static constexpr uint8_t TAS5825P_ADDR = 0x4C;

// Navigation registers (every page/book switch goes through these).
static constexpr uint8_t REG_PAGE         = 0x00;  // Page select within current book
static constexpr uint8_t REG_BOOK         = 0x7F;  // Book select
static constexpr uint8_t BOOK_0           = 0x00;  // Main control book
static constexpr uint8_t PAGE_0           = 0x00;  // Main control page

// Core control registers (book 0, page 0).
static constexpr uint8_t REG_RESET        = 0x01;  // [4]=DSP reset, [0]=registers reset
static constexpr uint8_t REG_MODE_CTRL    = 0x02;  // DEVICE_CTRL1
static constexpr uint8_t REG_DEVICE_CTRL  = 0x03;  // DEVICE_CTRL2 — mode select
static constexpr uint8_t REG_SDOUT_SEL    = 0x30;
static constexpr uint8_t REG_CLK_SRC      = 0x33;  // Clock source (0x00 = auto)
static constexpr uint8_t REG_FS_RATE      = 0x34;  // Forced sample rate (0x00 = auto-detect)
static constexpr uint8_t REG_FS_MON       = 0x37;  // RO — sample-rate monitor / PLL lock indicator
static constexpr uint8_t REG_ANALOG_CTRL  = 0x46;
static constexpr uint8_t REG_DIGITAL_VOL  = 0x4C;
static constexpr uint8_t REG_GPIO1_SEL    = 0x4F;  // P-only: GPIO1 routing (FAULT vs HPFB)
static constexpr uint8_t REG_AGAIN_L      = 0x53;
static constexpr uint8_t REG_AGAIN_R      = 0x54;
static constexpr uint8_t REG_SAP_CTRL1    = 0x60;  // Serial Audio Port Control 1
static constexpr uint8_t REG_DSP_MISC     = 0x62;
static constexpr uint8_t REG_POWER_STATE  = 0x68;  // RO — current power state
static constexpr uint8_t REG_AUTOMUTE     = 0x69;
static constexpr uint8_t REG_CHAN_FAULT   = 0x70;  // RO — per-channel OC/DC faults
static constexpr uint8_t REG_GLOBAL1      = 0x71;  // RO — GLOBAL_FAULT1
static constexpr uint8_t REG_GLOBAL2      = 0x72;  // RO — GLOBAL_FAULT2
static constexpr uint8_t REG_OT_WARNING   = 0x73;
static constexpr uint8_t REG_FAULT_CLEAR  = 0x78;  // W — write 0x80 to clear latched faults

// DSP coefficient page (book 0x8C, page 0x0B — identity passthrough taps).
static constexpr uint8_t DSP_BOOK = 0x8C;
static constexpr uint8_t DSP_PAGE = 0x0B;

// DEVICE_CTRL2 (0x03) mode-field values.
static constexpr uint8_t MODE_DEEP_SLEEP  = 0x00;
static constexpr uint8_t MODE_SLEEP       = 0x01;
static constexpr uint8_t MODE_HIZ         = 0x02;
static constexpr uint8_t MODE_PLAY        = 0x03;

// SAP_CTRL1 (0x60) word-length values — must match the I²S TX bit depth.
// Other bit fields (data format, LRCLK pulse) left at reset defaults
// (Philips I²S, auto LRCLK).
static constexpr uint8_t SAP_WORD_16BIT   = 0x00;
static constexpr uint8_t SAP_WORD_20BIT   = 0x01;
static constexpr uint8_t SAP_WORD_24BIT   = 0x02;
static constexpr uint8_t SAP_WORD_32BIT   = 0x03;

// Analog-gain values per supply rail (datasheet §7.4 AGAIN table).
static constexpr uint8_t AGAIN_12V        = 0x10;  // 3S LiPo nominal — what HubFX uses
static constexpr uint8_t AGAIN_15V        = 0x0C;
static constexpr uint8_t AGAIN_20V        = 0x07;
static constexpr uint8_t AGAIN_24V        = 0x05;

// DIGITAL_VOL (0x4C) — 0.5 dB/step, 0x30 = 0 dB reference.
static constexpr uint8_t DIGVOL_0DB       = 0x30;

// RESET (0x01) — bit pattern that resets both registers and DSP.
static constexpr uint8_t RESET_FULL       = 0x11;

// FAULT_CLEAR (0x78) — write this byte to clear all latched faults.
static constexpr uint8_t FAULT_CLEAR_CMD  = 0x80;

// GLOBAL_FAULT1 (0x71) bit decode (empirically validated against
// production HubFX firmware — see controllers/hubfx/esp32s3/src/
// hubfx_esp32s3.ino:398-403).
static constexpr uint8_t GF1_PVDD_OV      = 0x01;  // PVDD over-voltage
static constexpr uint8_t GF1_PVDD_UV      = 0x02;  // PVDD under-voltage
static constexpr uint8_t GF1_CLOCK        = 0x04;  // No valid I²S clock detected
static constexpr uint8_t GF1_BQ           = 0x08;  // BQ (boost converter) fault

// GLOBAL_FAULT2 (0x72) bit decode.
static constexpr uint8_t GF2_OT_SHUTDOWN  = 0x01;  // Over-temperature shutdown
static constexpr uint8_t GF2_OT_WARNING   = 0x08;  // Over-temperature warning

// CHAN_FAULT (0x70) bit decode.
static constexpr uint8_t CF_OC_LEFT       = 0x01;
static constexpr uint8_t CF_OC_RIGHT      = 0x02;
static constexpr uint8_t CF_DC_LEFT       = 0x04;
static constexpr uint8_t CF_DC_RIGHT      = 0x08;

// ============================================================================
//  TYPES
// ============================================================================

struct StereoSample {
    int16_t left;
    int16_t right;
};

// Decode entry for printing the set bits of a fault register.
struct FaultBit {
    uint8_t     mask;
    const char* name;
};

// ============================================================================
//  GLOBALS
// ============================================================================

static i2s_chan_handle_t i2sHandle       = nullptr;
static TaskHandle_t      audioTaskHandle = nullptr;

// Audio-task → diagnostics counters. Audio task writes (relaxed),
// diagnostics task reads (relaxed for stats, acquire on the `running` flag).
static std::atomic<uint32_t> totalFramesWritten{0};
static std::atomic<uint32_t> totalWrites{0};
static std::atomic<uint32_t> writeErrors{0};
static std::atomic<uint32_t> audioTaskLoops{0};
static std::atomic<int16_t>  lastPeakSample{0};
static std::atomic<bool>     i2sRunning{false};
static std::atomic<bool>     codecInPlay{false};

// Sine-wave phase accumulator (Core 1 only).
static float sinePhase = 0.0f;

// DMA-safe internal-SRAM batch buffer.
static StereoSample batchBuffer[FRAMES_PER_BATCH];

// ============================================================================
//  I²C PRIMITIVES
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
    if (Wire.endTransmission(false) != 0) return 0xFF;
    if (Wire.requestFrom(addr, (uint8_t)1) != 1) return 0xFF;
    return Wire.available() ? Wire.read() : 0xFF;
}

static bool i2cProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// ============================================================================
//  TAS5825P REGISTER HELPERS
// ============================================================================

// Navigate to (book, page). Datasheet §7.5 mandates: select page 0 first,
// then change BOOK, then move to the target page.
static void selectBookPage(uint8_t book, uint8_t page) {
    i2cWriteReg(TAS5825P_ADDR, REG_PAGE, 0x00);
    i2cWriteReg(TAS5825P_ADDR, REG_BOOK, book);
    i2cWriteReg(TAS5825P_ADDR, REG_PAGE, page);
}

static const char* modeStr(uint8_t modeField) {
    switch (modeField & 0x0F) {
        case MODE_DEEP_SLEEP: return "DEEP_SLEEP";
        case MODE_SLEEP:      return "SLEEP";
        case MODE_HIZ:        return "HIZ";
        case MODE_PLAY:       return "PLAY";
    }
    return "???";
}

static const char* fsMonStr(uint8_t fs) {
    switch (fs & 0x0F) {
        case 0x00: return "NONE";
        case 0x01: return "8kHz";
        case 0x02: return "16kHz";
        case 0x03: return "32kHz";
        case 0x04: return "48kHz";
        case 0x05: return "96kHz";
        case 0x06: return "44.1kHz";
        case 0x07: return "88.2kHz";
        case 0x08: return "176.4kHz";
        case 0x09: return "192kHz";
        case 0x0F: return "INVALID";
    }
    return "???";
}

// SAP_CTRL1 word-length matching the compile-time BIT_DEPTH.
static constexpr uint8_t SAP_WORD_FOR_BIT_DEPTH =
    (BIT_DEPTH == 32) ? SAP_WORD_32BIT :
    (BIT_DEPTH == 24) ? SAP_WORD_24BIT :
    (BIT_DEPTH == 20) ? SAP_WORD_20BIT :
                        SAP_WORD_16BIT;

// ============================================================================
//  CODEC PHASE 1 — pre-clock reset into DEEP_SLEEP
//  Safe to call before I²S clocks are running; PLL is off in DEEP_SLEEP.
// ============================================================================

static bool codecPhase1Reset() {
    Serial.printf("[TAS] Probing TAS5825P @ 0x%02X... ", TAS5825P_ADDR);
    if (!i2cProbe(TAS5825P_ADDR)) {
        Serial.println("NOT FOUND");
        return false;
    }
    Serial.println("OK");

    selectBookPage(BOOK_0, PAGE_0);
    i2cWriteReg(TAS5825P_ADDR, REG_DEVICE_CTRL, MODE_DEEP_SLEEP);
    delay(5);
    i2cWriteReg(TAS5825P_ADDR, REG_RESET, RESET_FULL);
    delay(50);

    selectBookPage(BOOK_0, PAGE_0);
    i2cWriteReg(TAS5825P_ADDR, REG_DEVICE_CTRL, MODE_DEEP_SLEEP);
    delay(5);

    uint8_t pwr = i2cReadReg(TAS5825P_ADDR, REG_POWER_STATE);
    Serial.printf("[TAS] After reset: POWER_STATE=0x%02X (expect 0x00=DEEP_SLEEP)\n", pwr);
    return true;
}

// ============================================================================
//  CODEC PHASE 2 — configure + DEEP_SLEEP → HIZ → PLAY
//  MUST be called only after I²S BCLK + LRCLK are active; the codec's
//  clock-detect block needs to see a clock during the HIZ transition.
// ============================================================================

static bool codecPhase2Activate() {
    Serial.println("[TAS] === Post-clock codec configuration ===");

    uint8_t prePwr = i2cReadReg(TAS5825P_ADDR, REG_POWER_STATE);
    uint8_t preFs  = i2cReadReg(TAS5825P_ADDR, REG_FS_MON);
    Serial.printf("[TAS] Pre-config: POWER_STATE=0x%02X  FS_MON=0x%02X\n", prePwr, preFs);

    // Analog stage — set for 12 V supply (HubFX nominal). ANA_CTRL=0x11
    // selects the high-gain Class-H modulator; AGAIN_L/R load the per-
    // channel attenuation matching the supply.
    selectBookPage(BOOK_0, PAGE_0);
    i2cWriteReg(TAS5825P_ADDR, REG_ANALOG_CTRL, 0x11);
    i2cWriteReg(TAS5825P_ADDR, REG_MODE_CTRL,   0x00);
    i2cWriteReg(TAS5825P_ADDR, REG_AGAIN_L,     0x01);
    i2cWriteReg(TAS5825P_ADDR, REG_AGAIN_R,     AGAIN_12V);
    Serial.println("[TAS] Analog gain configured for 12 V");

    // Clock + serial-audio port — auto-detect sample rate from BCLK/LRCLK.
    // SAP_CTRL1 word-length MUST match the I²S TX bit depth or the codec
    // can't align its frame detect and never reports a valid FS_MON.
    i2cWriteReg(TAS5825P_ADDR, REG_CLK_SRC,   0x00);                  // auto
    i2cWriteReg(TAS5825P_ADDR, REG_FS_RATE,   0x00);                  // auto-detect
    i2cWriteReg(TAS5825P_ADDR, REG_SDOUT_SEL, 0x00);
    i2cWriteReg(TAS5825P_ADDR, REG_SAP_CTRL1, SAP_WORD_FOR_BIT_DEPTH);
    i2cWriteReg(TAS5825P_ADDR, REG_DSP_MISC,  0x09);

    uint8_t r33 = i2cReadReg(TAS5825P_ADDR, REG_CLK_SRC);
    uint8_t r34 = i2cReadReg(TAS5825P_ADDR, REG_FS_RATE);
    uint8_t r60 = i2cReadReg(TAS5825P_ADDR, REG_SAP_CTRL1);
    Serial.printf("[TAS] Clock regs: CLK_SRC(0x33)=0x%02X  FS_RATE(0x34)=0x%02X  SAP_CTRL1(0x60)=0x%02X\n",
                  r33, r34, r60);

    // DSP coefficient block — identity passthrough so left/right ride
    // through unprocessed (matches production tas5825_p_codec.cpp).
    Serial.println("[TAS] Writing DSP identity coefficients (book 0x8C, page 0x0B)");
    selectBookPage(DSP_BOOK, DSP_PAGE);
    static const uint8_t identity[] = {
        0x00, 0x80, 0x00, 0x00,  // left  = 1.0 in 1.31 fixed
        0x00, 0x80, 0x00, 0x00,  // right = 1.0 in 1.31 fixed
    };
    for (size_t i = 0; i < sizeof(identity); i++) {
        i2cWriteReg(TAS5825P_ADDR, 0x28 + i, identity[i]);
    }

    // Volume to 0 dB and transition DEEP_SLEEP → HIZ. The P silicon's
    // permissive flow allows direct entry — no FS_MON gate required.
    selectBookPage(BOOK_0, PAGE_0);
    i2cWriteReg(TAS5825P_ADDR, REG_DIGITAL_VOL, DIGVOL_0DB);
    i2cWriteReg(TAS5825P_ADDR, REG_DEVICE_CTRL, MODE_HIZ);
    Serial.println("[TAS] DEVICE_CTRL ← HIZ — PLL acquiring lock...");
    delay(50);

    uint8_t fsMon1 = i2cReadReg(TAS5825P_ADDR, REG_FS_MON);
    uint8_t pwr1   = i2cReadReg(TAS5825P_ADDR, REG_POWER_STATE);
    Serial.printf("[TAS] After HIZ:  FS_MON=0x%02X (%s)  POWER_STATE=0x%02X (%s)\n",
                  fsMon1, fsMonStr(fsMon1), pwr1, modeStr(pwr1));
    if (fsMon1 != 0x00) {
        Serial.printf("[TAS] PLL LOCKED at %s\n", fsMonStr(fsMon1));
    } else {
        Serial.println("[TAS] FS_MON still 0x00 — PLL not locked");
    }

    // HIZ → PLAY.
    i2cWriteReg(TAS5825P_ADDR, REG_DEVICE_CTRL, MODE_PLAY);
    delay(20);
    i2cWriteReg(TAS5825P_ADDR, REG_FAULT_CLEAR, FAULT_CLEAR_CMD);
    delay(50);

    uint8_t fsMon2 = i2cReadReg(TAS5825P_ADDR, REG_FS_MON);
    uint8_t pwr2   = i2cReadReg(TAS5825P_ADDR, REG_POWER_STATE);
    Serial.printf("[TAS] After PLAY: FS_MON=0x%02X (%s)  POWER_STATE=0x%02X (%s)\n",
                  fsMon2, fsMonStr(fsMon2), pwr2, modeStr(pwr2));

    if ((pwr2 & 0x0F) == MODE_PLAY) {
        Serial.println("[TAS] *** SUCCESS: codec is in PLAY ***");
        return true;
    }

    // Fallback path the original P-flow uses to re-trigger auto-detect:
    // bounce through DEEP_SLEEP → PLAY directly.
    Serial.println("[TAS] PLAY via HIZ failed — bouncing through DEEP_SLEEP");
    i2cWriteReg(TAS5825P_ADDR, REG_DEVICE_CTRL, MODE_DEEP_SLEEP);
    delay(10);
    i2cWriteReg(TAS5825P_ADDR, REG_DEVICE_CTRL, MODE_PLAY);
    delay(50);
    i2cWriteReg(TAS5825P_ADDR, REG_FAULT_CLEAR, FAULT_CLEAR_CMD);
    delay(50);

    uint8_t pwr3 = i2cReadReg(TAS5825P_ADDR, REG_POWER_STATE);
    Serial.printf("[TAS] DEEP_SLEEP → PLAY: POWER_STATE=0x%02X (%s)\n", pwr3, modeStr(pwr3));
    if ((pwr3 & 0x0F) == MODE_PLAY) {
        Serial.println("[TAS] *** SUCCESS: direct PLAY worked ***");
        return true;
    }

    Serial.println("[TAS] *** FAILED: codec won't enter PLAY ***");
    return false;
}

// ============================================================================
//  CODEC DIAGNOSTICS — register dump + decoded fault bits + I²S + I²C state
// ============================================================================

static void dumpFaultBits(const char* prefix, uint8_t value,
                          const FaultBit* bits, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (value & bits[i].mask) {
            Serial.printf("%s    %s\n", prefix, bits[i].name);
        }
    }
}

static void dumpCodecDiag() {
    selectBookPage(BOOK_0, PAGE_0);

    uint8_t devCtrl    = i2cReadReg(TAS5825P_ADDR, REG_DEVICE_CTRL);
    uint8_t powerState = i2cReadReg(TAS5825P_ADDR, REG_POWER_STATE);
    uint8_t fsMon      = i2cReadReg(TAS5825P_ADDR, REG_FS_MON);
    uint8_t automute   = i2cReadReg(TAS5825P_ADDR, REG_AUTOMUTE);
    uint8_t digVol     = i2cReadReg(TAS5825P_ADDR, REG_DIGITAL_VOL);
    uint8_t sapCtrl1   = i2cReadReg(TAS5825P_ADDR, REG_SAP_CTRL1);
    uint8_t chanFault  = i2cReadReg(TAS5825P_ADDR, REG_CHAN_FAULT);
    uint8_t global1    = i2cReadReg(TAS5825P_ADDR, REG_GLOBAL1);
    uint8_t global2    = i2cReadReg(TAS5825P_ADDR, REG_GLOBAL2);
    uint8_t otWarn     = i2cReadReg(TAS5825P_ADDR, REG_OT_WARNING);

    uint32_t frames    = totalFramesWritten.load(std::memory_order_relaxed);
    uint32_t writes    = totalWrites.load(std::memory_order_relaxed);
    uint32_t errors    = writeErrors.load(std::memory_order_relaxed);
    uint32_t taskLoops = audioTaskLoops.load(std::memory_order_relaxed);
    int16_t  peak      = lastPeakSample.load(std::memory_order_relaxed);
    bool     running   = i2sRunning.load(std::memory_order_acquire);

    // Frames-per-second derived from the delta between consecutive diag
    // dumps. First call after boot reports 0 fps until a second sample lands.
    static uint32_t lastDumpMs = 0;
    static uint32_t lastFrames = 0;
    uint32_t nowMs = millis();
    uint32_t framesPerSec = 0;
    if (lastDumpMs != 0 && nowMs > lastDumpMs) {
        framesPerSec = (uint32_t)((uint64_t)(frames - lastFrames) * 1000 / (nowMs - lastDumpMs));
    }
    lastDumpMs = nowMs;
    lastFrames = frames;

    // Live re-probe the codec so we can see if the bus dropped at runtime.
    bool tasOnBus = i2cProbe(TAS5825P_ADDR);

    Serial.println("==== TAS5825P DIAGNOSTICS ====");
    Serial.printf("  DEVICE_CTRL  (0x03): 0x%02X → %s\n",  devCtrl,    modeStr(devCtrl));
    Serial.printf("  POWER_STATE  (0x68): 0x%02X → %s\n",  powerState, modeStr(powerState));
    Serial.printf("  FS_MON       (0x37): 0x%02X → %s\n",  fsMon,      fsMonStr(fsMon));
    Serial.printf("  SAP_CTRL1    (0x60): 0x%02X (word length bits [1:0]=%u)\n",
                  sapCtrl1, sapCtrl1 & 0x03);
    Serial.printf("  DIGITAL_VOL  (0x4C): 0x%02X (%.1f dB)\n",
                  digVol, ((int)digVol - DIGVOL_0DB) * 0.5f);
    Serial.printf("  AUTOMUTE     (0x69): 0x%02X%s\n", automute,
                  (automute & 0x03) ? "  (channels automuted)" : "");
    Serial.printf("  CHAN_FAULT   (0x70): 0x%02X\n", chanFault);
    static const FaultBit chanBits[] = {
        {CF_OC_LEFT,  "bit0: Left  over-current"},
        {CF_OC_RIGHT, "bit1: Right over-current"},
        {CF_DC_LEFT,  "bit2: Left  DC offset"},
        {CF_DC_RIGHT, "bit3: Right DC offset"},
    };
    dumpFaultBits("    ", chanFault, chanBits, sizeof(chanBits) / sizeof(chanBits[0]));

    Serial.printf("  GLOBAL_FAULT1(0x71): 0x%02X\n", global1);
    static const FaultBit global1Bits[] = {
        {GF1_PVDD_OV, "bit0: PVDD over-voltage"},
        {GF1_PVDD_UV, "bit1: PVDD under-voltage"},
        {GF1_CLOCK,   "bit2: Clock fault — no valid serial clock detected"},
        {GF1_BQ,      "bit3: BQ / boost converter fault"},
    };
    dumpFaultBits("    ", global1, global1Bits, sizeof(global1Bits) / sizeof(global1Bits[0]));

    Serial.printf("  GLOBAL_FAULT2(0x72): 0x%02X\n", global2);
    static const FaultBit global2Bits[] = {
        {GF2_OT_SHUTDOWN, "bit0: Over-temperature shutdown"},
        {GF2_OT_WARNING,  "bit3: Over-temperature warning"},
    };
    dumpFaultBits("    ", global2, global2Bits, sizeof(global2Bits) / sizeof(global2Bits[0]));

    Serial.printf("  OT_WARNING   (0x73): 0x%02X\n", otWarn);

    Serial.println("  -- I2S --");
    Serial.printf("    pins: DOUT=%d BCLK=%d LRCLK=%d  fs=%lu Hz  bits=%lu  batch=%u\n",
                  (int)PIN_I2S_DOUT, (int)PIN_I2S_BCLK, (int)PIN_I2S_LRCLK,
                  (unsigned long)SAMPLE_RATE, (unsigned long)BIT_DEPTH,
                  (unsigned)FRAMES_PER_BATCH);
    Serial.printf("    running=%s  frames=%lu (%lu fps)  writes=%lu  errors=%lu  loops=%lu  peak=%d/32767\n",
                  running ? "YES" : "NO",
                  (unsigned long)frames, (unsigned long)framesPerSec,
                  (unsigned long)writes, (unsigned long)errors,
                  (unsigned long)taskLoops, (int)peak);

    Serial.println("  -- I2C --");
    Serial.printf("    pins: SDA=%d SCL=%d  clk=%lu Hz\n",
                  PIN_I2C_SDA, PIN_I2C_SCL, (unsigned long)Wire.getClock());
    Serial.printf("    TAS5825P @ 0x%02X: %s\n",
                  TAS5825P_ADDR, tasOnBus ? "ACK" : "NACK");

    if ((powerState & 0x0F) != MODE_PLAY) {
        Serial.println("  *** WARNING: NOT IN PLAY STATE ***");
    }
    if (chanFault || global1 || global2) {
        Serial.println("  *** FAULT ACTIVE ***");
    }
    Serial.println("==============================");
}

// ============================================================================
//  SINE-WAVE GENERATOR
// ============================================================================

static void generateSineBatch() {
    const float phaseIncrement = (2.0f * (float)M_PI * SINE_FREQ_HZ) / (float)SAMPLE_RATE;
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

    // Wrap phase so the float accumulator never drifts.
    if (sinePhase >= 2.0f * (float)M_PI) {
        sinePhase -= 2.0f * (float)M_PI;
    }

    lastPeakSample.store(peak, std::memory_order_relaxed);
}

// ============================================================================
//  I²S SETUP (ESP-IDF v5.x standard mode)
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

    Serial.printf("[I2S] DMA config: %d descriptors x %d frames = %d frame ring\n",
                  chanCfg.dma_desc_num, chanCfg.dma_frame_num,
                  chanCfg.dma_desc_num * chanCfg.dma_frame_num);

    esp_err_t err = i2s_new_channel(&chanCfg, &i2sHandle, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: i2s_new_channel failed: %s (0x%x)\n",
                      esp_err_to_name(err), err);
        return false;
    }
    Serial.println("[I2S] TX channel created OK");

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
        Serial.printf("[I2S] ERROR: i2s_channel_init_std_mode failed: %s (0x%x)\n",
                      esp_err_to_name(err), err);
        i2s_del_channel(i2sHandle);
        i2sHandle = nullptr;
        return false;
    }
    Serial.println("[I2S] Standard mode initialized (Philips format)");

    err = i2s_channel_enable(i2sHandle);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: i2s_channel_enable failed: %s (0x%x)\n",
                      esp_err_to_name(err), err);
        i2s_del_channel(i2sHandle);
        i2sHandle = nullptr;
        return false;
    }

    Serial.println("[I2S] Channel enabled — clocks running");
    Serial.printf("[I2S] BCLK frequency: %lu Hz (= %lu Hz × %lu bits × 2 channels)\n",
                  SAMPLE_RATE * BIT_DEPTH * 2, SAMPLE_RATE, BIT_DEPTH);
    return true;
}

// ============================================================================
//  AUDIO TASK (Core 1) — I²S DMA write loop
// ============================================================================

static void audioTask(void* /*param*/) {
    Serial.printf("[Audio] Task started on Core %d (priority %d)\n",
                  xPortGetCoreID(), uxTaskPriorityGet(nullptr));

    // ESP-IDF requirement: I²S channel must be created and written from
    // the same core for DMA descriptor cache coherency.
    if (!initI2S()) {
        Serial.println("[Audio] FATAL: I2S init failed — task will idle");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    i2sRunning.store(true, std::memory_order_release);

    // Prime the DMA ring so codec sees clocks immediately on first read.
    Serial.println("[Audio] Pre-filling DMA pipeline (4 batches)...");
    for (int i = 0; i < 4; i++) {
        generateSineBatch();
        size_t bytesWritten = 0;
        i2s_channel_write(i2sHandle, batchBuffer,
                          FRAMES_PER_BATCH * sizeof(StereoSample),
                          &bytesWritten, portMAX_DELAY);
    }

    Serial.println("[Audio] Entering continuous output loop");
    Serial.printf("[Audio] Sine wave: %.1f Hz, amplitude %.0f%%, "
                  "%u frames/batch (%.1f ms/batch)\n",
                  SINE_FREQ_HZ, AMPLITUDE * 100.0f, (unsigned)FRAMES_PER_BATCH,
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
//  BOOT-TIME LOGGING HELPERS
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
    Serial.println("  SFX Test (P-variant) — TAS5825P sine-wave I²S bring-up");
    Serial.println("  ESP32-S3 / HubFX board | Class-H + Hybrid-Pro silicon (RHB)");
    Serial.println("  Permissive init flow (no smart-amp / FS_MON gating)");
    Serial.println("  For TAS5825M boards use ../sfx_test_m/ instead");
    Serial.println("================================================================");
    Serial.println();
}

static void printSystemInfo() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    Serial.println("--- System Info ---");
    Serial.printf("  Chip:          ESP32-S3 rev %d, %d core(s), %d MHz\n",
                  chip.revision, chip.cores, ESP.getCpuFreqMHz());
    Serial.printf("  Flash:         %lu MB (%s)\n",
                  ESP.getFlashChipSize() / (1024 * 1024),
                  (chip.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    Serial.printf("  PSRAM:         %d bytes (%.1f MB)\n",
                  ESP.getPsramSize(), ESP.getPsramSize() / (1024.0f * 1024.0f));
    Serial.printf("  Free heap:     %lu bytes (min ever: %lu)\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());
    Serial.printf("  Free PSRAM:    %d bytes\n", ESP.getFreePsram());
    Serial.printf("  SDK version:   %s\n", ESP.getSdkVersion());
    Serial.printf("  Reset reason:  %s\n", resetReasonStr(esp_reset_reason()));
    Serial.printf("  Arduino core:  %d.%d.%d\n",
                  ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
                  ESP_ARDUINO_VERSION_PATCH);
    Serial.println();
}

static void printAudioConfig() {
    Serial.println("--- Audio Configuration ---");
    Serial.printf("  Sample rate:   %lu Hz\n", SAMPLE_RATE);
    Serial.printf("  Bit depth:     %lu-bit\n", BIT_DEPTH);
    Serial.printf("  Channels:      2 (stereo, both = mono sine)\n");
    Serial.printf("  Sine freq:     %.1f Hz\n", SINE_FREQ_HZ);
    Serial.printf("  Amplitude:     %.0f%%\n", AMPLITUDE * 100.0f);
    Serial.printf("  Batch size:    %u frames (%.2f ms)\n",
                  (unsigned)FRAMES_PER_BATCH,
                  (float)FRAMES_PER_BATCH / SAMPLE_RATE * 1000.0f);
    Serial.printf("  Byte rate:     %lu bytes/sec\n",
                  SAMPLE_RATE * 2 * sizeof(int16_t));
    Serial.printf("  I2S BCLK:      %.3f MHz\n",
                  (float)(SAMPLE_RATE * BIT_DEPTH * 2) / 1e6f);
    Serial.println();

    Serial.println("--- I2S Pin Mapping ---");
    Serial.printf("  DOUT  (data):  GPIO%d\n", PIN_I2S_DOUT);
    Serial.printf("  BCLK  (clock): GPIO%d\n", PIN_I2S_BCLK);
    Serial.printf("  LRCLK (ws):    GPIO%d\n", PIN_I2S_LRCLK);
    Serial.println();
}

static void printTaskInfo() {
    Serial.println("--- FreeRTOS Tasks ---");
    Serial.printf("  Loop task:     Core %d, priority %d, stack free: %u bytes\n",
                  xPortGetCoreID(), uxTaskPriorityGet(nullptr),
                  uxTaskGetStackHighWaterMark(nullptr) * 4);
    if (audioTaskHandle) {
        Serial.printf("  Audio task:    Core 1, priority %d, stack free: %u bytes\n",
                      uxTaskPriorityGet(audioTaskHandle),
                      uxTaskGetStackHighWaterMark(audioTaskHandle) * 4);
    }
    Serial.printf("  Total tasks:   %lu\n", (unsigned long)uxTaskGetNumberOfTasks());
    Serial.println();
}

// ============================================================================
//  SETUP (Core 0)
// ============================================================================

void setup() {
    Serial.begin(115200);

    // Block briefly while USB-CDC enumerates so the first banner isn't lost.
    uint32_t serialWaitStart = millis();
    while (!Serial && (millis() - serialWaitStart < 3000)) delay(10);

    printBanner();
    printSystemInfo();
    printAudioConfig();

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);

    // -- I²C bring-up + bus scan --
    Serial.printf("[I2C] Initializing: SDA=GPIO%d, SCL=GPIO%d, %lu Hz\n",
                  PIN_I2C_SDA, PIN_I2C_SCL, (unsigned long)I2C_CLOCK_HZ);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(I2C_CLOCK_HZ);

    Serial.println("[I2C] Scanning bus...");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C]   Found device at 0x%02X\n", addr);
        }
    }

    // -- Codec phase 1: reset into DEEP_SLEEP (no I²S clocks required) --
    bool codecFound = codecPhase1Reset();
    if (!codecFound) {
        Serial.println("*** WARN: codec not found — audio output will be I²S only ***");
    }

    // -- Launch audio task on Core 1 --
    Serial.println("[Main] Launching audio task on Core 1...");
    BaseType_t result = xTaskCreatePinnedToCore(
        audioTask,                // entry
        "AudioSine",              // name
        8192,                     // stack (bytes)
        nullptr,                  // arg
        configMAX_PRIORITIES - 1, // priority
        &audioTaskHandle,         // handle
        1                         // pin to Core 1
    );
    if (result != pdPASS) {
        Serial.println("[Main] FATAL: failed to create audio task!");
    } else {
        Serial.println("[Main] Audio task created — waiting for I2S init...");
    }

    // Wait for the audio task to bring up I²S.
    uint32_t waitStart = millis();
    while (!i2sRunning.load(std::memory_order_acquire)) {
        delay(50);
        if (millis() - waitStart > 5000) {
            Serial.println("[Main] WARNING: timeout waiting for I2S init (5s)");
            break;
        }
    }
    if (i2sRunning.load(std::memory_order_acquire)) {
        Serial.println("[Main] I2S confirmed running — clocks active");
    }

    // -- Codec phase 2: configure + DEEP_SLEEP → HIZ → PLAY --
    if (codecFound) {
        delay(200);  // let DMA pump a few frames so BCK/LRCLK are stable
        bool codecOk = codecPhase2Activate();
        codecInPlay.store(codecOk, std::memory_order_release);
        if (codecOk) {
            Serial.println("\n*** CODEC IN PLAY — 440 Hz tone should be audible ***\n");
        } else {
            Serial.println("\n*** CODEC FAILED TO ENTER PLAY — sine wave on I²S only ***\n");
            dumpCodecDiag();
        }
    }

    printTaskInfo();
    Serial.println("================================================================");
    Serial.printf("[Main] Diagnostics will print every %lu ms\n", DIAG_INTERVAL_MS);
    Serial.println("================================================================");
    Serial.println();
}

// ============================================================================
//  LOOP (Core 0) — periodic diagnostics
// ============================================================================

void loop() {
    static uint32_t lastDiagTime_ms   = 0;
    static uint32_t lastFrameSnapshot = 0;
    static uint32_t lastWriteSnapshot = 0;
    static uint32_t diagCount         = 0;
    static uint32_t ledToggle_ms      = 0;

    uint32_t now = millis();

    // 1 Hz LED heartbeat — visible alive signal that doesn't depend on
    // either I²C or I²S.
    if (now - ledToggle_ms > 500) {
        ledToggle_ms = now;
        digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    }

    if (now - lastDiagTime_ms < DIAG_INTERVAL_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }
    lastDiagTime_ms = now;
    diagCount++;

    // Snapshot atomics once per tick.
    uint32_t frames  = totalFramesWritten.load(std::memory_order_relaxed);
    uint32_t writes  = totalWrites.load(std::memory_order_relaxed);
    uint32_t errors  = writeErrors.load(std::memory_order_relaxed);
    uint32_t loops   = audioTaskLoops.load(std::memory_order_relaxed);
    int16_t  peak    = lastPeakSample.load(std::memory_order_relaxed);
    bool     running = i2sRunning.load(std::memory_order_acquire);

    // Throughput derived from the per-tick delta.
    uint32_t deltaFrames = frames - lastFrameSnapshot;
    lastFrameSnapshot = frames;
    lastWriteSnapshot = writes;

    float frameRate = (float)deltaFrames / ((float)DIAG_INTERVAL_MS / 1000.0f);
    float uptime_s  = (float)now / 1000.0f;
    float peakDb    = (peak > 0) ? 20.0f * log10f((float)peak / 32767.0f) : -96.0f;

    // Print a column header every 10 ticks for readability.
    if ((diagCount % 10) == 1) {
        Serial.println();
        Serial.println("  #   Uptime   I2S   Frames/s     Total     Writes   Errors"
                       "   Loops   Peak dBFS   Heap    PSRAM   Stack(C1)");
        Serial.println("---  -------  -----  ----------  ---------  -------  ------"
                       "  ------  ---------  ------  ------  ---------");
    }

    Serial.printf("%3lu  %6.1fs  %-5s  %10.0f  %9lu  %7lu  %6lu  %6lu    %5.1f  "
                  "%5luK  %5luK",
                  diagCount,
                  uptime_s,
                  running ? "OK" : "DOWN",
                  frameRate,
                  (unsigned long)frames,
                  (unsigned long)writes,
                  (unsigned long)errors,
                  (unsigned long)loops,
                  peakDb,
                  ESP.getFreeHeap() / 1024,
                  ESP.getFreePsram() / 1024);
    if (audioTaskHandle) {
        Serial.printf("   %5u", uxTaskGetStackHighWaterMark(audioTaskHandle) * 4);
    }
    Serial.println();

    // Verbose dump every 30 s — full register/fault decode + I²S/I²C state.
    if ((diagCount % VERBOSE_DUMP_EVERY) == 0) {
        Serial.println();
        Serial.println("--- Periodic Detailed Report ---");
        Serial.printf("  Uptime:                %.1f seconds\n", uptime_s);
        Serial.printf("  Free heap:             %lu bytes (min: %lu)\n",
                      ESP.getFreeHeap(), ESP.getMinFreeHeap());
        Serial.printf("  Free PSRAM:            %d bytes\n", ESP.getFreePsram());
        Serial.printf("  Total frames written:  %lu (%.1f seconds of audio)\n",
                      (unsigned long)frames, (float)frames / SAMPLE_RATE);
        Serial.printf("  Effective sample rate: %.0f Hz (target %lu Hz)\n",
                      frameRate, (unsigned long)SAMPLE_RATE);
        Serial.printf("  Write errors:          %lu\n", (unsigned long)errors);
        Serial.printf("  Peak sample:           %d / 32767 (%.1f dBFS)\n", peak, peakDb);
        Serial.printf("  Audio task loops:      %lu\n", (unsigned long)loops);
        Serial.printf("  CPU frequency:         %u MHz\n", ESP.getCpuFreqMHz());
        Serial.printf("  Codec in PLAY:         %s\n",
                      codecInPlay.load(std::memory_order_relaxed) ? "YES" : "NO");
        if (audioTaskHandle) {
            Serial.printf("  Audio task stack HWM:  %u bytes\n",
                          uxTaskGetStackHighWaterMark(audioTaskHandle) * 4);
        }
        Serial.printf("  Loop task stack HWM:   %u bytes\n",
                      uxTaskGetStackHighWaterMark(nullptr) * 4);
        Serial.printf("  FreeRTOS tasks:        %lu\n",
                      (unsigned long)uxTaskGetNumberOfTasks());
        Serial.println("--- End Report ---");

        dumpCodecDiag();
        Serial.println();
    }
}
