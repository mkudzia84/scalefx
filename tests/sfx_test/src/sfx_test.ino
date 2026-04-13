/**
 * SFX Test — TAS5825M Audio Codec Test Firmware
 *
 * Test firmware for verifying TAS5825M codec operation on the HubFX board.
 * Generates a 440 Hz sine wave via I2S, initializes the TAS5825M via I2C,
 * and prints extensive codec diagnostics to Serial.
 *
 * NO external libraries — only Arduino core + ESP-IDF headers.
 * Works in both Arduino IDE (board: "ESP32S3 Dev Module") and PlatformIO.
 *
 * Hardware: ESP32-S3R8 (HubFX board)
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
 *   - Sequence: Expander PDN+MUTE → Deep Sleep → I2S clocks → HIZ → PLAY
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

// ============================================================================
//  PCAL6416A IO EXPANDER REGISTERS
// ============================================================================

static constexpr uint8_t PCAL_OUTPUT_0 = 0x02;
static constexpr uint8_t PCAL_OUTPUT_1 = 0x03;
static constexpr uint8_t PCAL_CONFIG_0 = 0x06;  // Port 0 direction (1=input, 0=output)
static constexpr uint8_t PCAL_CONFIG_1 = 0x07;  // Port 1 direction

// Expander Port 1 bit assignments
static constexpr uint8_t EXP_FAULT_BIT = 0;  // P1_0 — TAS5825M nFAULT (input)
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
static constexpr uint8_t TAS_REG_AGAIN_L     = 0x53;
static constexpr uint8_t TAS_REG_AGAIN_R     = 0x54;
static constexpr uint8_t TAS_REG_CLK_CFG     = 0x60;
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
static constexpr uint8_t TAS_CTRL_HIZ        = 0x02;
static constexpr uint8_t TAS_CTRL_PLAY       = 0x03;

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

// Cross-core stats (atomic for safe reads from Core 0)
static std::atomic<uint32_t> totalFramesWritten{0};
static std::atomic<uint32_t> totalWrites{0};
static std::atomic<uint32_t> writeErrors{0};
static std::atomic<bool>     i2sRunning{false};
static std::atomic<uint32_t> audioTaskLoops{0};
static std::atomic<int16_t>  lastPeakSample{0};

// Codec status
static std::atomic<bool>     codecInPlay{false};

// Phase accumulator (Core 1 only — no atomic needed)
static float sinePhase = 0.0f;

// Batch buffer in internal SRAM (DMA requirement)
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
//  PCAL6416A IO EXPANDER INIT
// ============================================================================

static bool initExpander() {
    Serial.printf("[EXP] Probing PCAL6416A @ 0x%02X... ", PCAL6416A_ADDR);
    if (!i2cProbe(PCAL6416A_ADDR)) {
        Serial.println("NOT FOUND");
        return false;
    }
    Serial.println("OK");

    // Port 1 direction: P1_0 (FAULT) = input, P1_1 (MUTE) + P1_2 (PDN) = output
    // Bits: 1=input, 0=output. Default is 0xFF (all input).
    // We want: bit0=1 (input), bit1=0 (output), bit2=0 (output), rest=1 (input)
    uint8_t dir1 = 0xF9;  // 1111_1001
    i2cWriteReg(PCAL6416A_ADDR, PCAL_CONFIG_1, dir1);

    // Set outputs: PDN=HIGH (run), MUTE=HIGH (unmuted)
    uint8_t out1 = (1 << EXP_PDN_BIT) | (1 << EXP_MUTE_BIT);  // 0x06
    i2cWriteReg(PCAL6416A_ADDR, PCAL_OUTPUT_1, out1);

    // Read back and verify
    uint8_t readDir = i2cReadReg(PCAL6416A_ADDR, PCAL_CONFIG_1);
    uint8_t readOut = i2cReadReg(PCAL6416A_ADDR, PCAL_OUTPUT_1);
    Serial.printf("[EXP] Port 1: dir=0x%02X (expect 0x%02X), out=0x%02X (expect 0x%02X)\n",
                  readDir, dir1, readOut, out1);

    // Read pin states
    uint8_t pins = i2cReadReg(PCAL6416A_ADDR, 0x01);
    Serial.printf("[EXP] Port 1 input: 0x%02X\n", pins);
    Serial.printf("[EXP]   PDN  (P1_2): %s\n", (pins & (1 << EXP_PDN_BIT)) ? "HIGH (run)" : "LOW (!!)");
    Serial.printf("[EXP]   MUTE (P1_1): %s\n", (pins & (1 << EXP_MUTE_BIT)) ? "HIGH (unmuted)" : "LOW (!!)");
    Serial.printf("[EXP]   FAULT(P1_0): %s\n", (pins & (1 << EXP_FAULT_BIT)) ? "HIGH (ok)" : "LOW (FAULT!)");
    return true;
}

// ============================================================================
//  TAS5825M CODEC INIT
// ============================================================================

static void tasSelectBookPage(uint8_t book, uint8_t page) {
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_PAGE, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_BOOK, book);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_PAGE, page);
}

/**
 * Phase 1 (before I2S clocks): Reset codec into Deep Sleep.
 * PLL is OFF, no clock needed.
 */
static bool initCodecPreClock() {
    Serial.printf("[TAS] Probing TAS5825M @ 0x%02X... ", TAS5825M_ADDR);
    if (!i2cProbe(TAS5825M_ADDR)) {
        Serial.println("NOT FOUND");
        return false;
    }
    Serial.println("OK");

    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_DEEP_SLEEP);
    delay(5);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_RESET, 0x11);  // Full reset
    delay(50);

    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_DEEP_SLEEP);
    delay(5);

    uint8_t pwr = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    Serial.printf("[TAS] After reset: POWER_STATE=0x%02X (expect 0x00=Deep Sleep)\n", pwr);
    return true;
}

/**
 * Phase 2 (after I2S clocks are running and stable):
 * Configure registers, then Deep Sleep → HIZ → PLAY.
 */
static bool initCodecPostClock() {
    Serial.println("[TAS] === Post-clock codec configuration ===");

    uint8_t prePwr = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    uint8_t preFs  = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
    Serial.printf("[TAS] Pre-config: PWR=0x%02X  FS_MON=0x%02X\n", prePwr, preFs);

    // Step 1: Analog gain (12V supply)
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_ANALOG_CTRL, 0x11);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_MODE_CTRL, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_AGAIN_L, 0x01);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_AGAIN_R, 0x10);
    Serial.println("[TAS] Analog gain configured (12V)");

    // Step 2: Clock source — AUTOMATIC
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_CLK_SRC, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_FS_RATE, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_SDOUT_SEL, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_CLK_CFG, 0x02);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DSP_MISC, 0x09);

    uint8_t r33 = i2cReadReg(TAS5825M_ADDR, TAS_REG_CLK_SRC);
    uint8_t r34 = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_RATE);
    uint8_t r60 = i2cReadReg(TAS5825M_ADDR, TAS_REG_CLK_CFG);
    Serial.printf("[TAS] Clock regs: 0x33=0x%02X  0x34=0x%02X  0x60=0x%02X\n", r33, r34, r60);

    // Step 3: DSP coefficients (identity pass-through)
    Serial.println("[TAS] Writing DSP coefficients...");
    tasSelectBookPage(0x8C, 0x0B);
    uint8_t identity[] = { 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00 };
    for (size_t i = 0; i < sizeof(identity); i++) {
        i2cWriteReg(TAS5825M_ADDR, 0x28 + i, identity[i]);
    }

    // Step 4: Deep Sleep → HIZ (PLL starts locking)
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_HIZ);
    Serial.println("[TAS] Transitioned to HIZ — PLL locking...");
    delay(50);

    uint8_t fsMon1 = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
    uint8_t pwr1   = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    Serial.printf("[TAS] After HIZ: FS_MON=0x%02X  PWR=0x%02X\n", fsMon1, pwr1);

    if (fsMon1 != 0x00) {
        Serial.printf("[TAS] PLL LOCKED! FS detected: 0x%02X\n", fsMon1);
    } else {
        Serial.println("[TAS] FS_MON still 0x00 — PLL not locked");
    }

    // Step 5: HIZ → PLAY
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DIGITAL_VOL, 0x30);  // 0 dB volume
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_PLAY);
    delay(20);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_FAULT_CLEAR, 0x80);
    delay(50);

    uint8_t fsMon2 = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
    uint8_t pwr2   = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    Serial.printf("[TAS] After PLAY: FS_MON=0x%02X  PWR=0x%02X\n", fsMon2, pwr2);

    if ((pwr2 & 0x0F) == 0x03) {
        Serial.println("[TAS] *** SUCCESS: CODEC IS IN PLAY MODE ***");
        return true;
    }

    // Fallback: try Deep Sleep → PLAY directly
    Serial.println("[TAS] PLAY via HIZ failed. Trying Deep Sleep → PLAY...");
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_DEEP_SLEEP);
    delay(10);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_PLAY);
    delay(50);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_FAULT_CLEAR, 0x80);
    delay(50);

    uint8_t pwr3 = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    Serial.printf("[TAS] Deep Sleep→PLAY: PWR=0x%02X\n", pwr3);

    if ((pwr3 & 0x0F) == 0x03) {
        Serial.println("[TAS] *** SUCCESS: Direct PLAY worked ***");
        return true;
    }

    Serial.println("[TAS] *** FAILED: Codec won't enter PLAY ***");
    return false;
}

// ============================================================================
//  TAS5825M DIAGNOSTICS
// ============================================================================

static void dumpCodecDiag() {
    tasSelectBookPage(0x00, 0x00);

    uint8_t devCtrl    = i2cReadReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL);
    uint8_t powerState = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    uint8_t fsMon      = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
    uint8_t automute   = i2cReadReg(TAS5825M_ADDR, TAS_REG_AUTOMUTE);
    uint8_t digVol     = i2cReadReg(TAS5825M_ADDR, TAS_REG_DIGITAL_VOL);
    uint8_t clkCfg     = i2cReadReg(TAS5825M_ADDR, TAS_REG_CLK_CFG);
    uint8_t chanFault  = i2cReadReg(TAS5825M_ADDR, TAS_REG_CHAN_FAULT);
    uint8_t global1    = i2cReadReg(TAS5825M_ADDR, TAS_REG_GLOBAL1);
    uint8_t global2    = i2cReadReg(TAS5825M_ADDR, TAS_REG_GLOBAL2);
    uint8_t otWarn     = i2cReadReg(TAS5825M_ADDR, TAS_REG_OT_WARNING);

    // Expander pin readback
    uint8_t expPins = i2cReadReg(PCAL6416A_ADDR, 0x01);

    const char* powerStr = "???";
    switch (powerState & 0x0F) {
        case 0x00: powerStr = "DEEP_SLEEP"; break;
        case 0x01: powerStr = "SLEEP"; break;
        case 0x02: powerStr = "HIZ"; break;
        case 0x03: powerStr = "PLAY"; break;
    }

    const char* fsStr = "???";
    switch (fsMon) {
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
    }

    uint32_t frames = totalFramesWritten.load(std::memory_order_relaxed);
    uint32_t errors = writeErrors.load(std::memory_order_relaxed);

    Serial.println("==== TAS5825M DIAGNOSTICS ====");
    Serial.printf("  Expander: PDN=%s  MUTE=%s  FAULT=%s\n",
        (expPins & (1 << EXP_PDN_BIT)) ? "HIGH" : "LOW",
        (expPins & (1 << EXP_MUTE_BIT)) ? "HIGH" : "LOW",
        (expPins & (1 << EXP_FAULT_BIT)) ? "HIGH" : "LOW");
    Serial.printf("  DEVICE_CTRL:  0x%02X → %s\n", devCtrl,
        devCtrl == 0x03 ? "PLAY" : devCtrl == 0x02 ? "HIZ" : devCtrl == 0x01 ? "SLEEP" : "DEEP_SLEEP");
    Serial.printf("  POWER_STATE:  0x%02X → %s\n", powerState, powerStr);
    Serial.printf("  FS_MON:       0x%02X → %s\n", fsMon, fsStr);
    Serial.printf("  CLK_CFG:      0x%02X\n", clkCfg);
    Serial.printf("  DIGITAL_VOL:  0x%02X (%.1f dB)\n", digVol, (digVol - 0x30) * 0.5f);
    Serial.printf("  AUTOMUTE:     0x%02X\n", automute);
    Serial.printf("  CHAN_FAULT:    0x%02X\n", chanFault);
    Serial.printf("  GLOBAL_FAULT1:0x%02X\n", global1);
    Serial.printf("  GLOBAL_FAULT2:0x%02X\n", global2);
    Serial.printf("  OT_WARNING:   0x%02X\n", otWarn);
    Serial.printf("  I2S frames:   %lu  errors: %lu\n", frames, errors);
    if ((powerState & 0x0F) != 0x03) {
        Serial.println("  *** WARNING: NOT IN PLAY STATE ***");
    }
    if (chanFault || global1 || global2) {
        Serial.println("  *** FAULT ACTIVE ***");
    }
    Serial.println("==============================");
}

// ============================================================================
//  SINE WAVE GENERATION
// ============================================================================

/**
 * Fill the batch buffer with one chunk of sine wave samples.
 * Uses a floating-point phase accumulator for glitch-free output.
 */
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

    // Wrap phase to avoid float precision loss over time
    if (sinePhase >= 2.0f * M_PI) {
        sinePhase -= 2.0f * M_PI;
    }

    lastPeakSample.store(peak, std::memory_order_relaxed);
}

// ============================================================================
//  I2S SETUP (ESP-IDF v5.x)
// ============================================================================

static bool initI2S() {
    Serial.println("[I2S] Initializing ESP-IDF v5.x standard-mode driver...");
    Serial.printf("[I2S] Pins: DOUT=%d, BCLK=%d, LRCLK=%d\n",
                  PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK);
    Serial.printf("[I2S] Format: %lu Hz, %lu-bit, stereo\n", SAMPLE_RATE, BIT_DEPTH);

    // Step 1: Create TX channel
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num  = 8;     // 8 DMA descriptors
    chanCfg.dma_frame_num = 512;   // 512 frames per descriptor
    chanCfg.auto_clear_after_cb = true;  // Zero DMA buffers after TX (prevents loops on underrun)

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

    // Step 2: Configure standard mode (I2S Philips)
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
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
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

    // Step 3: Enable channel (starts DMA + clocks)
    err = i2s_channel_enable(i2sHandle);
    if (err != ESP_OK) {
        Serial.printf("[I2S] ERROR: i2s_channel_enable failed: %s (0x%x)\n",
                      esp_err_to_name(err), err);
        i2s_del_channel(i2sHandle);
        i2sHandle = nullptr;
        return false;
    }

    Serial.println("[I2S] Channel enabled — clocks running!");
    Serial.printf("[I2S] BCLK frequency: %lu Hz (= %lu Hz * %lu bits * 2 channels)\n",
                  SAMPLE_RATE * BIT_DEPTH * 2, SAMPLE_RATE, BIT_DEPTH);
    return true;
}

// ============================================================================
//  AUDIO TASK (Core 1)
// ============================================================================

/**
 * Dedicated FreeRTOS task pinned to Core 1.
 * Generates sine wave samples and writes them to I2S DMA.
 * i2s_channel_write() blocks when DMA buffers are full, providing
 * natural sample-rate pacing without any manual timing.
 */
static void audioTask(void* param) {
    Serial.printf("[Audio] Task started on Core %d (priority %d)\n",
                  xPortGetCoreID(), uxTaskPriorityGet(nullptr));

    // Initialize I2S on this core (ESP-IDF requirement: I2S channel
    // must be init'd and written from the same core for DMA coherency)
    if (!initI2S()) {
        Serial.println("[Audio] FATAL: I2S init failed — task will idle");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    i2sRunning.store(true, std::memory_order_release);

    // Pre-fill: generate and write a few batches to prime the DMA pipeline
    Serial.printf("[Audio] Pre-filling DMA pipeline (%d batches)...\n", 4);
    for (int i = 0; i < 4; i++) {
        generateSineBatch();
        size_t bytesWritten = 0;
        i2s_channel_write(i2sHandle, batchBuffer,
                          FRAMES_PER_BATCH * sizeof(StereoSample),
                          &bytesWritten, portMAX_DELAY);
    }

    Serial.println("[Audio] Entering continuous output loop");
    Serial.printf("[Audio] Sine wave: %.1f Hz, amplitude: %.0f%%, "
                  "%d frames/batch (%.1f ms/batch)\n",
                  SINE_FREQ_HZ, AMPLITUDE * 100.0f, FRAMES_PER_BATCH,
                  (float)FRAMES_PER_BATCH / SAMPLE_RATE * 1000.0f);

    // Continuous output loop
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
//  DIAGNOSTICS HELPERS
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
    Serial.println("  NoOp Sound Effects — ESP32-S3 Sine Wave I2S Test");
    Serial.println("  Standalone firmware (no external libraries)");
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
    Serial.printf("  Batch size:    %d frames (%.2f ms)\n",
                  FRAMES_PER_BATCH,
                  (float)FRAMES_PER_BATCH / SAMPLE_RATE * 1000.0f);
    Serial.printf("  Byte rate:     %lu bytes/sec\n",
                  SAMPLE_RATE * 2 * sizeof(int16_t));  // stereo 16-bit
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

    // Current task (Core 0)
    Serial.printf("  Loop task:     Core %d, priority %d, stack free: %u bytes\n",
                  xPortGetCoreID(), uxTaskPriorityGet(nullptr),
                  uxTaskGetStackHighWaterMark(nullptr) * 4);

    // Audio task (Core 1)
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

    // Wait for serial connection (up to 3 seconds)
    uint32_t serialWaitStart = millis();
    while (!Serial && (millis() - serialWaitStart < 3000)) {
        delay(10);
    }

    printBanner();
    printSystemInfo();
    printAudioConfig();

    // === LED heartbeat ===
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);

    // === I2C Bus ===
    Serial.printf("[I2C] Initializing: SDA=GPIO%d, SCL=GPIO%d, 100kHz\n", PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);

    // I2C bus scan
    Serial.println("[I2C] Scanning bus...");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C]   Found device at 0x%02X\n", addr);
        }
    }

    // === IO Expander (controls TAS5825M power + mute) ===
    if (!initExpander()) {
        Serial.println("*** WARN: Expander init failed — TAS5825M won't have power ***");
    }
    delay(50);

    // === TAS5825M Phase 1: Reset into Deep Sleep (before I2S clocks) ===
    bool codecFound = initCodecPreClock();
    if (!codecFound) {
        Serial.println("*** WARN: Codec not found — audio output will be I2S only ***");
    }

    // === Launch audio task on Core 1 ===
    Serial.println("[Main] Launching audio task on Core 1...");
    BaseType_t result = xTaskCreatePinnedToCore(
        audioTask,                      // Task function
        "AudioSine",                    // Name
        8192,                           // Stack (bytes)
        nullptr,                        // Parameters
        configMAX_PRIORITIES - 1,       // Highest priority
        &audioTaskHandle,               // Handle
        1                               // Pin to Core 1
    );

    if (result != pdPASS) {
        Serial.println("[Main] FATAL: Failed to create audio task!");
    } else {
        Serial.println("[Main] Audio task created — waiting for I2S init...");
    }

    // Wait for I2S to come up (with timeout)
    uint32_t waitStart = millis();
    while (!i2sRunning.load(std::memory_order_acquire)) {
        delay(50);
        if (millis() - waitStart > 5000) {
            Serial.println("[Main] WARNING: Timeout waiting for I2S init (5s)");
            break;
        }
    }

    if (i2sRunning.load(std::memory_order_acquire)) {
        Serial.println("[Main] I2S confirmed running — clocks active!");
    }

    // === TAS5825M Phase 2: Configure + PLAY (I2S clocks now running) ===
    if (codecFound) {
        // Wait a bit more for DMA to pump some frames so clocks are stable
        delay(200);

        bool codecOk = initCodecPostClock();
        codecInPlay.store(codecOk, std::memory_order_release);

        if (codecOk) {
            Serial.println("\n*** CODEC IN PLAY MODE — YOU SHOULD HEAR A 440Hz TONE ***\n");
        } else {
            Serial.println("\n*** CODEC FAILED TO ENTER PLAY — sine wave on I2S only ***\n");
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
//  LOOP (Core 0) — Diagnostics only
// ============================================================================

static uint32_t lastDiagTime_ms   = 0;
static uint32_t lastFrameSnapshot = 0;
static uint32_t lastWriteSnapshot = 0;
static uint32_t diagCount         = 0;

void loop() {
    uint32_t now = millis();

    // LED heartbeat
    static uint32_t ledToggle = 0;
    if (now - ledToggle > 500) {
        ledToggle = now;
        digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    }

    if (now - lastDiagTime_ms >= DIAG_INTERVAL_MS) {
        lastDiagTime_ms = now;
        diagCount++;

        // Snapshot atomic counters
        uint32_t frames = totalFramesWritten.load(std::memory_order_relaxed);
        uint32_t writes = totalWrites.load(std::memory_order_relaxed);
        uint32_t errors = writeErrors.load(std::memory_order_relaxed);
        uint32_t loops  = audioTaskLoops.load(std::memory_order_relaxed);
        int16_t  peak   = lastPeakSample.load(std::memory_order_relaxed);
        bool     running = i2sRunning.load(std::memory_order_acquire);

        // Calculate rates since last report
        uint32_t deltaFrames = frames - lastFrameSnapshot;
        uint32_t deltaWrites = writes - lastWriteSnapshot;
        lastFrameSnapshot = frames;
        lastWriteSnapshot = writes;

        float frameRate = (float)deltaFrames / ((float)DIAG_INTERVAL_MS / 1000.0f);
        float uptime_s  = (float)now / 1000.0f;
        float peakDb    = (peak > 0) ? 20.0f * log10f((float)peak / 32767.0f) : -96.0f;

        // Header every 10 reports
        if ((diagCount % 10) == 1) {
            Serial.println();
            Serial.println("  #   Uptime   I2S   Frames/s     Total     Writes   Errors"
                           "   Loops   Peak dBFS   Heap    PSRAM   Stack(C1)");
            Serial.println("---  -------  -----  ----------  ---------  -------  ------"
                           "  ------  ---------  ------  ------  ---------");
        }

        // One-line status
        Serial.printf("%3lu  %6.1fs  %-5s  %10.0f  %9lu  %7lu  %6lu  %6lu    %5.1f  "
                      "%5luK  %5luK",
                      diagCount,
                      uptime_s,
                      running ? "OK" : "DOWN",
                      frameRate,
                      frames,
                      writes,
                      errors,
                      loops,
                      peakDb,
                      ESP.getFreeHeap() / 1024,
                      ESP.getFreePsram() / 1024);

        // Audio task stack high-water mark
        if (audioTaskHandle) {
            Serial.printf("   %5u",
                          uxTaskGetStackHighWaterMark(audioTaskHandle) * 4);
        }
        Serial.println();

        // Expanded diagnostics every 30 seconds
        if ((diagCount % 15) == 0) {
            Serial.println();
            Serial.println("--- Periodic Detailed Report ---");
            Serial.printf("  Uptime:                %.1f seconds\n", uptime_s);
            Serial.printf("  Free heap:             %lu bytes (min: %lu)\n",
                          ESP.getFreeHeap(), ESP.getMinFreeHeap());
            Serial.printf("  Free PSRAM:            %d bytes\n", ESP.getFreePsram());
            Serial.printf("  Total frames written:  %lu (%.1f seconds of audio)\n",
                          frames, (float)frames / SAMPLE_RATE);
            Serial.printf("  Effective sample rate: %.0f Hz (target %lu Hz)\n",
                          frameRate, SAMPLE_RATE);
            Serial.printf("  Write errors:          %lu\n", errors);
            Serial.printf("  Peak sample:           %d / 32767 (%.1f dBFS)\n",
                          peak, peakDb);
            Serial.printf("  Audio task loops:      %lu\n", loops);
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

            // Full codec diagnostics every 30s
            dumpCodecDiag();

            Serial.println();
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));  // 10 Hz loop — light on CPU
}
