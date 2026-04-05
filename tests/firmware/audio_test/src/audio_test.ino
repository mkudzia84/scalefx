/**
 * Audio Test Firmware — Sine Wave Generator (ESP32-S3)
 *
 * Single-core, zero-dependency test that verifies the full audio path:
 *   ESP32-S3 I2S → TAS5825M codec → speaker
 *
 * Sequence:
 *   1. Init I2C (GPIO8/9)
 *   2. Init PCAL6416A expander: PDN=HIGH, MUTE=HIGH
 *   3. Init TAS5825M codec registers (hardcoded, no library)
 *   4. Init ESP-IDF I2S standard-mode driver (GPIO1/4/3)
 *   5. Continuously write 440Hz sine wave to I2S
 *   6. Every 5 seconds: dump all TAS5825M diagnostic registers
 *
 * Uses Serial (UART0 @ 115200) for all output — readable in any terminal.
 */

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_reg.h>
#include <soc/i2s_reg.h>
#include <hal/gpio_hal.h>
#include <esp_rom_gpio.h>
#include <esp32-hal-periman.h>
#include <math.h>

// ============================================================================
// Pin Definitions (HubFX v1 board)
// ============================================================================
#define PIN_I2S_DOUT    1     // I2S serial data out (to TAS5825M DIN)
#define PIN_I2S_BCLK    4     // I2S bit clock
#define PIN_I2S_LRCLK   3     // I2S word select / frame sync
#define PIN_I2C_SDA     8     // I2C data
#define PIN_I2C_SCL     9     // I2C clock
#define PIN_LED         48    // Onboard LED (heartbeat)

// ============================================================================
// I2C Addresses
// ============================================================================
#define TAS5825M_ADDR   0x4C
#define PCAL6416A_ADDR  0x20

// ============================================================================
// PCAL6416A Registers
// ============================================================================
#define PCAL_OUTPUT_0   0x02
#define PCAL_OUTPUT_1   0x03
#define PCAL_CONFIG_0   0x06  // Port 0 direction (1=input, 0=output)
#define PCAL_CONFIG_1   0x07  // Port 1 direction

// Expander Port 1 bits (pin 8-15 mapped as 0-7 within Port 1)
#define EXP_FAULT_BIT   0    // P1_0 — TAS5825M nFAULT (input)
#define EXP_MUTE_BIT    1    // P1_1 — TAS5825M MUTE (output, HIGH=unmuted)
#define EXP_PDN_BIT     2    // P1_2 — TAS5825M PDN (output, HIGH=run)

// ============================================================================
// TAS5825M Registers
// ============================================================================
#define TAS_REG_PAGE        0x00
#define TAS_REG_RESET       0x01
#define TAS_REG_MODE_CTRL   0x02
#define TAS_REG_DEVICE_CTRL 0x03
#define TAS_REG_SIG_CH_CTRL 0x28
#define TAS_REG_SDOUT_SEL   0x30
#define TAS_REG_FS_MON      0x37
#define TAS_REG_ANALOG_CTRL 0x46
#define TAS_REG_DIGITAL_VOL 0x4C
#define TAS_REG_AGAIN_L     0x53
#define TAS_REG_AGAIN_R     0x54
#define TAS_REG_CLK_SRC     0x33  // Clock source: 0x00=Auto, 0x10=PLL
#define TAS_REG_FS_RATE     0x34  // Internal clock divider / BCK-FS ratio
#define TAS_REG_CLK_CFG     0x60
#define TAS_REG_DSP_MISC    0x62
#define TAS_REG_POWER_STATE 0x68
#define TAS_REG_AUTOMUTE    0x69
#define TAS_REG_CHAN_FAULT   0x70
#define TAS_REG_GLOBAL1     0x71
#define TAS_REG_GLOBAL2     0x72
#define TAS_REG_OT_WARNING  0x73
#define TAS_REG_FAULT_CLEAR 0x78
#define TAS_REG_BOOK        0x7F

#define TAS_CTRL_DEEP_SLEEP 0x00
#define TAS_CTRL_SLEEP      0x01
#define TAS_CTRL_HIZ        0x02
#define TAS_CTRL_PLAY       0x03

// ============================================================================
// Audio Configuration
// ============================================================================
#define SAMPLE_RATE     48000
#define SINE_FREQ       440       // A4
#define SINE_AMPLITUDE  8000      // ~25% of int16 range (conservative for testing)
#define I2S_DMA_BUFS    8
#define I2S_DMA_FRAMES  512

// Pre-computed sine table (one full period at 48kHz/440Hz ≈ 109 samples)
#define SINE_TABLE_SIZE 256
static int16_t sineTable[SINE_TABLE_SIZE];

// I2S DMA write buffer (interleaved L/R)
#define WRITE_BUF_FRAMES 256
static int16_t writeBuf[WRITE_BUF_FRAMES * 2];  // stereo interleaved

// I2S handle
static i2s_chan_handle_t txHandle = nullptr;

// Phase accumulator for sine generation
static float phase = 0.0f;
static const float phaseInc = (float)SINE_FREQ / (float)SAMPLE_RATE;

// Stats
static uint32_t totalFramesWritten = 0;
static uint32_t i2sWriteErrors = 0;
static uint32_t lastDiagTime_ms = 0;

// ============================================================================
// I2C Helpers
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
// PCAL6416A Expander Init
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
    uint8_t dir1 = 0xF9;  // 1111_1001 — P1_1 and P1_2 are outputs
    i2cWriteReg(PCAL6416A_ADDR, PCAL_CONFIG_1, dir1);

    // Set outputs: PDN=HIGH (run), MUTE=HIGH (unmuted)
    uint8_t out1 = (1 << EXP_PDN_BIT) | (1 << EXP_MUTE_BIT);  // 0x06
    i2cWriteReg(PCAL6416A_ADDR, PCAL_OUTPUT_1, out1);

    // Read back and verify
    uint8_t readDir = i2cReadReg(PCAL6416A_ADDR, PCAL_CONFIG_1);
    uint8_t readOut = i2cReadReg(PCAL6416A_ADDR, PCAL_OUTPUT_1);
    Serial.printf("[EXP] Port 1: dir=0x%02X (expect 0x%02X), out=0x%02X (expect 0x%02X)\n",
                  readDir, dir1, readOut, out1);

    // Read actual pin states (Input Port 1 register = 0x01)
    uint8_t pins = i2cReadReg(PCAL6416A_ADDR, 0x01);
    Serial.printf("[EXP] Port 1 input register: 0x%02X\n", pins);
    Serial.printf("[EXP]   PDN  (P1_2): %s\n", (pins & (1 << EXP_PDN_BIT)) ? "HIGH (run)" : "LOW (!!)");
    Serial.printf("[EXP]   MUTE (P1_1): %s\n", (pins & (1 << EXP_MUTE_BIT)) ? "HIGH (unmuted)" : "LOW (!!)");
    Serial.printf("[EXP]   FAULT(P1_0): %s\n", (pins & (1 << EXP_FAULT_BIT)) ? "HIGH (ok)" : "LOW (FAULT!)");

    return true;
}

// ============================================================================
// TAS5825M Codec Init — Bare registers, no library
// ============================================================================

static void tasSelectBookPage(uint8_t book, uint8_t page) {
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_PAGE, 0x00);    // Go to page 0 first
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_BOOK, book);     // Select book
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_PAGE, page);     // Select page
}

// Minimal pre-I2S codec init: reset + deep sleep.
// ALL configuration happens AFTER I2S clocks are running.
static bool initCodecPreClock() {
    Serial.printf("[TAS] Probing TAS5825M @ 0x%02X... ", TAS5825M_ADDR);
    if (!i2cProbe(TAS5825M_ADDR)) {
        Serial.println("NOT FOUND");
        return false;
    }
    Serial.println("OK");

    // === Reset into DEEP SLEEP — do NOT use HIZ (HIZ attempts PLL lock) ===
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_DEEP_SLEEP);
    delay(5);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_RESET, 0x11);  // Full reset
    delay(50);

    // Put back in deep sleep after reset (reset may change state)
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_DEEP_SLEEP);
    delay(5);

    uint8_t pwr = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    Serial.printf("[TAS] After reset: POWER_STATE=0x%02X (expect 0x00=Deep Sleep)\n", pwr);
    Serial.println("[TAS] Codec in DEEP SLEEP — PLL is OFF, waiting for I2S clocks");

    return true;
}

// Full codec configuration AFTER I2S clocks are running and stable.
// Sequence: Deep Sleep → configure regs → HIZ → PLAY
static bool initCodecPostClock() {
    Serial.println("[TAS] === Post-clock codec configuration ===");
    Serial.println("[TAS] I2S clocks should be running on GPIOs now.");

    // Read pre-config state
    uint8_t prePwr = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    uint8_t preFs  = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
    Serial.printf("[TAS] Pre-config: PWR=0x%02X  FS_MON=0x%02X\n", prePwr, preFs);

    // === Step 1: Analog gain (12V supply) — can configure in Deep Sleep ===
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_ANALOG_CTRL, 0x11);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_MODE_CTRL, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_AGAIN_L, 0x01);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_AGAIN_R, 0x10);  // 12V gain
    Serial.println("[TAS] Analog gain configured (12V)");

    // === Step 2: Clock source — AUTOMATIC ===
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_CLK_SRC, 0x00);     // 0x33: Automatic clock source
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_FS_RATE, 0x00);     // 0x34: Auto FS detection
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_SDOUT_SEL, 0x00);   // 0x30: SDOUT = DSP
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_CLK_CFG, 0x02);     // 0x60: Clock detection config
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DSP_MISC, 0x09);    // 0x62: DSP misc

    // Read back clock regs
    uint8_t r33 = i2cReadReg(TAS5825M_ADDR, TAS_REG_CLK_SRC);
    uint8_t r34 = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_RATE);
    uint8_t r60 = i2cReadReg(TAS5825M_ADDR, TAS_REG_CLK_CFG);
    Serial.printf("[TAS] Clock regs: 0x33=0x%02X  0x34=0x%02X  0x60=0x%02X\n", r33, r34, r60);

    // === Step 3: DSP coefficients (identity pass-through) ===
    Serial.println("[TAS] Writing DSP coefficients...");
    tasSelectBookPage(0x8C, 0x0B);
    uint8_t identity[] = { 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00 };
    for (size_t i = 0; i < sizeof(identity); i++) {
        i2cWriteReg(TAS5825M_ADDR, 0x28 + i, identity[i]);
    }

    // === Step 4: Transition Deep Sleep → HIZ (PLL starts locking) ===
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_HIZ);
    Serial.println("[TAS] Transitioned to HIZ — PLL should now lock to BCK...");
    delay(50);  // Wait for PLL lock

    uint8_t fsMon1 = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
    uint8_t pwr1   = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    Serial.printf("[TAS] After HIZ: FS_MON=0x%02X  PWR=0x%02X\n", fsMon1, pwr1);

    if (fsMon1 != 0x00) {
        Serial.printf("[TAS] *** PLL LOCKED! FS detected: 0x%02X ***\n", fsMon1);
    } else {
        Serial.println("[TAS] FS_MON still 0x00 after HIZ — PLL not locking");
    }

    // === Step 5: HIZ → PLAY ===
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DIGITAL_VOL, 0x30); // 0 dB volume
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

    // === Fallback: try Deep Sleep → PLAY directly (skip HIZ) ===
    Serial.println("[TAS] PLAY failed via HIZ. Trying Deep Sleep → PLAY directly...");
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_DEEP_SLEEP);
    delay(10);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_PLAY);
    delay(50);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_FAULT_CLEAR, 0x80);
    delay(50);

    uint8_t fsMon3 = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
    uint8_t pwr3   = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
    Serial.printf("[TAS] Deep Sleep→PLAY: FS_MON=0x%02X  PWR=0x%02X\n", fsMon3, pwr3);

    if ((pwr3 & 0x0F) == 0x03) {
        Serial.println("[TAS] *** SUCCESS: Direct PLAY worked ***");
        return true;
    }

    Serial.println("[TAS] *** FAILED: Codec won't enter PLAY ***");
    return false;
}

static void codecSetPlay() {
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_PLAY);
    i2cWriteReg(TAS5825M_ADDR, TAS_REG_FAULT_CLEAR, 0x80);
    Serial.println("[TAS] PLAY command issued + faults cleared");
}

// ============================================================================
// TAS5825M Diagnostics
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
    uint8_t faultClr   = i2cReadReg(TAS5825M_ADDR, TAS_REG_FAULT_CLEAR);

    // Expander pin readback
    uint8_t expPins = i2cReadReg(PCAL6416A_ADDR, 0x01);

    const char* powerStr = "???";
    switch (powerState & 0x0F) {
        case 0x00: powerStr = "DEEP_SLEEP"; break;
        case 0x01: powerStr = "SLEEP"; break;
        case 0x02: powerStr = "HIZ"; break;
        case 0x03: powerStr = "PLAY"; break;
    }

    const char* fsStr = "NONE";
    switch (fsMon) {
        case 0x00: fsStr = "NONE (no clocks!)"; break;
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

    Serial.println("==== TAS5825M DIAGNOSTICS ====");
    Serial.printf("  Expander: PDN=%s  MUTE=%s  FAULT=%s\n",
        (expPins & (1 << EXP_PDN_BIT)) ? "HIGH" : "LOW",
        (expPins & (1 << EXP_MUTE_BIT)) ? "HIGH" : "LOW",
        (expPins & (1 << EXP_FAULT_BIT)) ? "HIGH" : "LOW");
    Serial.printf("  DEVICE_CTRL:  0x%02X → %s\n", devCtrl,
        devCtrl == 0x03 ? "PLAY" : devCtrl == 0x02 ? "HIZ" : devCtrl == 0x01 ? "SLEEP" : "???");
    Serial.printf("  POWER_STATE:  0x%02X → %s\n", powerState, powerStr);
    Serial.printf("  FS_MON:       0x%02X → %s\n", fsMon, fsStr);
    Serial.printf("  CLK_CFG:      0x%02X\n", clkCfg);
    Serial.printf("  DIGITAL_VOL:  0x%02X (%.1f dB)\n", digVol, (digVol - 0x30) * 0.5f);
    Serial.printf("  AUTOMUTE:     0x%02X\n", automute);
    Serial.printf("  CHAN_FAULT:    0x%02X\n", chanFault);
    Serial.printf("  GLOBAL_FAULT1:0x%02X\n", global1);
    Serial.printf("  GLOBAL_FAULT2:0x%02X\n", global2);
    Serial.printf("  OT_WARNING:   0x%02X\n", otWarn);
    Serial.printf("  FAULT_CLEAR:  0x%02X\n", faultClr);
    Serial.printf("  I2S frames written: %u  errors: %u\n", totalFramesWritten, i2sWriteErrors);
    if (powerState != 0x03) {
        Serial.println("  *** WARNING: NOT IN PLAY STATE ***");
    }
    if (fsMon == 0x00) {
        Serial.println("  *** WARNING: NO I2S CLOCKS DETECTED ***");
    }
    if (chanFault || global1 || global2) {
        Serial.println("  *** FAULT ACTIVE ***");
    }
    Serial.println("==============================");
}

// ============================================================================
// I2S Init — ESP-IDF v5.x standard mode
// ============================================================================

static bool initI2S() {
    Serial.println("[I2S] Configuring ESP-IDF I2S standard mode...");
    Serial.printf("[I2S] Pins: DOUT=GPIO%d, BCLK=GPIO%d, LRCLK=GPIO%d\n",
                  PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK);

    // Explicitly detach any Arduino peripheral manager claims on these pins
    // (Arduino-ESP32 v3.x periman can silently prevent GPIO mux changes)
    perimanClearPinBus(PIN_I2S_DOUT);
    perimanClearPinBus(PIN_I2S_BCLK);
    perimanClearPinBus(PIN_I2S_LRCLK);

    // Step 1: Create TX channel
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = I2S_DMA_BUFS;
    chanCfg.dma_frame_num = I2S_DMA_FRAMES;
    chanCfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chanCfg, &txHandle, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[I2S] i2s_new_channel FAILED: %d (%s)\n", err, esp_err_to_name(err));
        return false;
    }
    Serial.println("[I2S] TX channel created");

    // Step 2: Configure standard mode (I2S Philips)
    i2s_std_config_t stdCfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_I2S_BCLK,
            .ws   = (gpio_num_t)PIN_I2S_LRCLK,
            .dout = (gpio_num_t)PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(txHandle, &stdCfg);
    if (err != ESP_OK) {
        Serial.printf("[I2S] i2s_channel_init_std_mode FAILED: %d (%s)\n", err, esp_err_to_name(err));
        i2s_del_channel(txHandle);
        txHandle = nullptr;
        return false;
    }
    Serial.println("[I2S] Standard mode configured (Philips, 48kHz, 16-bit stereo)");

    // Step 3: Enable channel (this starts DMA and clocks!)
    err = i2s_channel_enable(txHandle);
    if (err != ESP_OK) {
        Serial.printf("[I2S] i2s_channel_enable FAILED: %d (%s)\n", err, esp_err_to_name(err));
        i2s_del_channel(txHandle);
        txHandle = nullptr;
        return false;
    }
    Serial.println("[I2S] Channel ENABLED — BCLK and LRCLK should now be running");

    // ---- Comprehensive GPIO diagnostics ----
    Serial.println("[I2S] GPIO diagnostics after enable:");

    // Expected I2S0 signal indices (from soc/gpio_sig_map.h)
    Serial.printf("[I2S]   Expected signal IDs: I2S0O_SD_OUT=%d, I2S0O_WS_OUT=%d, I2S0O_BCK_OUT=%d\n",
                  I2S0O_SD_OUT_IDX, I2S0O_WS_OUT_IDX, I2S0O_BCK_OUT_IDX);

    // GPIO_ENABLE_REG: which GPIOs have output enabled (GPIO 0-31)
    uint32_t gpioEnable = REG_READ(GPIO_ENABLE_REG);
    Serial.printf("[I2S]   GPIO_ENABLE_REG = 0x%08X\n", gpioEnable);
    for (int pin : {PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK}) {
        bool outEnabled = (gpioEnable >> pin) & 1;
        Serial.printf("[I2S]     GPIO%d output enabled: %s\n", pin, outEnabled ? "YES" : "NO");
    }

    // IO_MUX register for each pin: FUN_SEL, drive strength, input enable
    for (int pin : {PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK}) {
        uint32_t iomux = REG_READ(GPIO_PIN_MUX_REG[pin]);
        uint32_t funSel  = (iomux >> 12) & 0x7;
        uint32_t funIE   = (iomux >> 9) & 0x1;
        uint32_t funDrv  = (iomux >> 10) & 0x3;
        Serial.printf("[I2S]     GPIO%d: IO_MUX=0x%08X FUN_SEL=%u FUN_IE=%u FUN_DRV=%u\n",
                      pin, iomux, funSel, funIE, funDrv);
    }

    // GPIO Matrix: what signal is routed to each output pin
    // GPIO_FUNCn_OUT_SEL_CFG_REG (address = GPIO_FUNC0_OUT_SEL_CFG_REG + n*4)
    for (int pin : {PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK}) {
        uint32_t outSel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + (pin * 4));
        uint32_t sigIdx   = outSel & 0x1FF;        // bits [8:0] = signal index
        uint32_t invEn    = (outSel >> 9) & 1;      // bit 9 = invert
        uint32_t oenSel   = (outSel >> 10) & 1;     // bit 10 = OEN select
        uint32_t oenInvEn = (outSel >> 11) & 1;     // bit 11 = OEN invert
        Serial.printf("[I2S]     GPIO%d: OUT_SEL=0x%08X  signal_idx=%u  inv=%u  oen_sel=%u  oen_inv=%u\n",
                      pin, outSel, sigIdx, invEn, oenSel, oenInvEn);
        // signal=256 means simple GPIO output, not peripheral
        if (sigIdx == 256) {
            Serial.printf("[I2S]     *** GPIO%d: signal=256 → SIMPLE GPIO (not I2S peripheral!)\n", pin);
        }
    }

    // Check if I2S signals were actually connected — if not, force-connect them
    bool needsManualFix = false;
    {
        uint32_t doutSig = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + (PIN_I2S_DOUT * 4)) & 0x1FF;
        uint32_t bclkSig = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + (PIN_I2S_BCLK * 4)) & 0x1FF;
        uint32_t lrclkSig = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + (PIN_I2S_LRCLK * 4)) & 0x1FF;

        if (doutSig != I2S0O_SD_OUT_IDX || bclkSig != I2S0O_BCK_OUT_IDX || lrclkSig != I2S0O_WS_OUT_IDX) {
            needsManualFix = true;
            Serial.println("[I2S] *** I2S signals NOT connected to GPIO matrix! Attempting manual fix...");

            // Force GPIO matrix routing
            gpio_set_direction((gpio_num_t)PIN_I2S_DOUT, GPIO_MODE_OUTPUT);
            esp_rom_gpio_connect_out_signal(PIN_I2S_DOUT, I2S0O_SD_OUT_IDX, false, false);

            gpio_set_direction((gpio_num_t)PIN_I2S_BCLK, GPIO_MODE_OUTPUT);
            esp_rom_gpio_connect_out_signal(PIN_I2S_BCLK, I2S0O_BCK_OUT_IDX, false, false);

            gpio_set_direction((gpio_num_t)PIN_I2S_LRCLK, GPIO_MODE_OUTPUT);
            esp_rom_gpio_connect_out_signal(PIN_I2S_LRCLK, I2S0O_WS_OUT_IDX, false, false);

            Serial.println("[I2S] Manual GPIO matrix fix applied. Re-checking:");

            for (int pin : {PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK}) {
                uint32_t outSel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + (pin * 4));
                uint32_t sigIdx = outSel & 0x1FF;
                Serial.printf("[I2S]     GPIO%d: signal_idx=%u\n", pin, sigIdx);
            }

            // Re-check enable
            gpioEnable = REG_READ(GPIO_ENABLE_REG);
            Serial.printf("[I2S]   GPIO_ENABLE_REG after fix = 0x%08X\n", gpioEnable);

            delay(200);  // Let clocks propagate
        } else {
            Serial.println("[I2S] GPIO matrix looks correct — I2S signals ARE connected.");
        }
    }

    // ---- I2S peripheral register dump ----
    Serial.println("[I2S] I2S peripheral register dump:");
    Serial.printf("[I2S]   I2S_TX_CONF_REG       = 0x%08X\n", REG_READ(I2S_TX_CONF_REG(0)));
    Serial.printf("[I2S]   I2S_TX_CONF1_REG      = 0x%08X\n", REG_READ(I2S_TX_CONF1_REG(0)));
    Serial.printf("[I2S]   I2S_TX_CLKM_CONF_REG  = 0x%08X\n", REG_READ(I2S_TX_CLKM_CONF_REG(0)));
    Serial.printf("[I2S]   I2S_TX_CLKM_DIV_CONF  = 0x%08X\n", REG_READ(I2S_TX_CLKM_DIV_CONF_REG(0)));
    Serial.printf("[I2S]   I2S_TX_TDM_CTRL_REG   = 0x%08X\n", REG_READ(I2S_TX_TDM_CTRL_REG(0)));
    Serial.printf("[I2S]   I2S_TX_TIMING_REG      = 0x%08X\n", REG_READ(I2S_TX_TIMING_REG(0)));

    // Check key bits
    uint32_t txConf = REG_READ(I2S_TX_CONF_REG(0));
    Serial.printf("[I2S]   TX_START=%u  TX_SLAVE_MOD=%u  TX_STOP=%u\n",
        (txConf >> 2) & 1, (txConf >> 3) & 1, (txConf >> 4) & 1);

    // ---- GPIO pin sampling: detect if BCLK is actually toggling ----
    Serial.println("[I2S] Sampling BCLK (GPIO4) and LRCLK (GPIO3) for activity...");

    // CRITICAL: Enable FUN_IE (input enable) on all I2S pins so we can read
    // back the output via GPIO_IN_REG. Without this, the input path is disabled
    // and GPIO_IN always reads 0 regardless of the output state.
    for (int pin : {PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK}) {
        uint32_t iomux = REG_READ(GPIO_PIN_MUX_REG[pin]);
        iomux |= (1 << 9);  // Set FUN_IE bit
        REG_WRITE(GPIO_PIN_MUX_REG[pin], iomux);
    }
    Serial.println("[I2S]   FUN_IE enabled on all I2S pins for readback");

    // Pump some silence first to make sure DMA is active
    {
        int16_t silenceBuf[512] = {0};
        size_t bw = 0;
        for (int i = 0; i < 5; i++) {
            i2s_channel_write(txHandle, silenceBuf, sizeof(silenceBuf), &bw, 100);
        }
    }

    // Give DMA time to start processing
    delay(100);

    // Verify FUN_IE is set
    for (int pin : {PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK}) {
        uint32_t iomux = REG_READ(GPIO_PIN_MUX_REG[pin]);
        uint32_t funIE = (iomux >> 9) & 1;
        Serial.printf("[I2S]     GPIO%d FUN_IE=%u (after set)\n", pin, funIE);
    }

    // Rapidly sample GPIO_IN for BCLK and LRCLK 10000 times
    {
        uint32_t bclkTransitions = 0;
        uint32_t lrclkTransitions = 0;
        uint32_t prevIn = REG_READ(GPIO_IN_REG);
        uint32_t bclkMask = (1 << PIN_I2S_BCLK);
        uint32_t lrclkMask = (1 << PIN_I2S_LRCLK);

        for (int i = 0; i < 10000; i++) {
            uint32_t curIn = REG_READ(GPIO_IN_REG);
            if ((curIn ^ prevIn) & bclkMask) bclkTransitions++;
            if ((curIn ^ prevIn) & lrclkMask) lrclkTransitions++;
            prevIn = curIn;
        }

        Serial.printf("[I2S]   BCLK  transitions in 10000 samples: %u\n", bclkTransitions);
        Serial.printf("[I2S]   LRCLK transitions in 10000 samples: %u\n", lrclkTransitions);

        if (bclkTransitions == 0) {
            Serial.println("[I2S]   *** BCLK is NOT toggling! I2S peripheral may not be generating clocks ***");
        } else {
            Serial.printf("[I2S]   BCLK is toggling (%u transitions) — signal IS present on GPIO\n", bclkTransitions);
        }
        if (lrclkTransitions == 0) {
            Serial.println("[I2S]   *** LRCLK is NOT toggling ***");
        } else {
            Serial.printf("[I2S]   LRCLK is toggling (%u transitions)\n", lrclkTransitions);
        }

        // Also read current pin levels
        uint32_t inReg = REG_READ(GPIO_IN_REG);
        Serial.printf("[I2S]   GPIO_IN_REG = 0x%08X  DOUT=%u  BCLK=%u  LRCLK=%u\n",
            inReg,
            (inReg >> PIN_I2S_DOUT) & 1,
            (inReg >> PIN_I2S_BCLK) & 1,
            (inReg >> PIN_I2S_LRCLK) & 1);
    }

    return true;
}

// ============================================================================
// Sine Wave Generation
// ============================================================================

static void initSineTable() {
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        sineTable[i] = (int16_t)(sinf(2.0f * M_PI * i / SINE_TABLE_SIZE) * SINE_AMPLITUDE);
    }
    Serial.printf("[SINE] Table generated: %d entries, freq=%dHz, amplitude=%d\n",
                  SINE_TABLE_SIZE, SINE_FREQ, SINE_AMPLITUDE);
}

static void fillSineBuffer() {
    for (int i = 0; i < WRITE_BUF_FRAMES; i++) {
        // Linear interpolation into sine table
        float idx = phase * SINE_TABLE_SIZE;
        int i0 = (int)idx % SINE_TABLE_SIZE;
        int16_t sample = sineTable[i0];

        writeBuf[i * 2]     = sample;  // Left
        writeBuf[i * 2 + 1] = sample;  // Right (mono sine on both channels)

        phase += phaseInc;
        if (phase >= 1.0f) phase -= 1.0f;
    }
}

// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(2000);  // Wait for USB-UART to settle
    Serial.println("\n========================================");
    Serial.println("  Audio Test Firmware — Sine Wave 440Hz");
    Serial.println("  ESP32-S3, Single Core, Direct I2S");
    Serial.println("========================================\n");

    // LED
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);

    // Pre-compute sine table
    initSineTable();

    // === I2C Bus ===
    Serial.printf("[I2C] Initializing: SDA=GPIO%d, SCL=GPIO%d, 100kHz\n", PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);

    // Quick I2C scan
    Serial.println("[I2C] Scanning bus...");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C]   Found device at 0x%02X\n", addr);
        }
    }

    // === Expander ===
    if (!initExpander()) {
        Serial.println("*** FATAL: Expander init failed — TAS5825M won't have power ***");
    }
    delay(50);  // Let PDN stabilize

    // === TAS5825M Phase 1: Reset into DEEP SLEEP (PLL off, no clock needed) ===
    if (!initCodecPreClock()) {
        Serial.println("*** FATAL: Codec pre-clock init failed ***");
    }

    Serial.println("\n--- Diagnostics BEFORE I2S (codec in Deep Sleep) ---");
    dumpCodecDiag();

    // === GPIO loopback test ===
    Serial.println("\n[GPIO TEST] Testing I2S pins as plain GPIO before I2S init...");
    for (int pin : {PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK}) {
        gpio_reset_pin((gpio_num_t)pin);
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT);

        gpio_set_level((gpio_num_t)pin, 1);
        delayMicroseconds(10);
        int readHigh = gpio_get_level((gpio_num_t)pin);

        gpio_set_level((gpio_num_t)pin, 0);
        delayMicroseconds(10);
        int readLow = gpio_get_level((gpio_num_t)pin);

        Serial.printf("[GPIO TEST]   GPIO%d: write HIGH→read %d, write LOW→read %d  %s\n",
            pin, readHigh, readLow,
            (readHigh == 1 && readLow == 0) ? "OK" : "*** FAIL — pin stuck or shorted! ***");

        gpio_reset_pin((gpio_num_t)pin);  // Clean up for I2S
    }
    Serial.println();

    // === I2S — start clocks first, codec configures after ===
    if (!initI2S()) {
        Serial.println("*** FATAL: I2S init failed ***");
    }

    // Wait for I2S clocks to stabilize
    Serial.println("[I2S] Waiting 200ms for clocks to stabilize...");
    delay(200);

    // Pump a few frames of silence / sine so DMA is actively clocking
    Serial.println("[I2S] Pumping initial audio data to ensure DMA is running...");
    for (int i = 0; i < 10; i++) {
        fillSineBuffer();
        size_t written = 0;
        i2s_channel_write(txHandle, writeBuf, WRITE_BUF_FRAMES * 4, &written, 500);
    }
    delay(50);

    Serial.println("\n--- Diagnostics AFTER I2S start, codec still in Deep Sleep ---");
    dumpCodecDiag();

    // === TAS5825M Phase 2: Configure registers + PLAY (clocks now running) ===
    bool codecOk = initCodecPostClock();

    if (codecOk) {
        Serial.println("\n*** CODEC IN PLAY MODE — YOU SHOULD HEAR A 440Hz TONE ***\n");
    } else {
        // === Fallback: dump all clock registers for debugging ===
        Serial.println("\n[CLK] --- Full clock register dump (0x30-0x6F) ---");
        tasSelectBookPage(0x00, 0x00);
        for (uint8_t reg = 0x30; reg <= 0x6F; reg++) {
            uint8_t val = i2cReadReg(TAS5825M_ADDR, reg);
            Serial.printf("[CLK]   reg 0x%02X = 0x%02X\n", reg, val);
        }

        Serial.println("\n--- Full diagnostics after failed init ---");
        dumpCodecDiag();

        // =================================================================
        // HARDWARE CONNECTIVITY TEST
        // Disable I2S, bit-bang a ~48kHz clock on BCLK/LRCLK pins manually,
        // then check if TAS5825M FS_MON changes. If it doesn't, the signals
        // are NOT reaching the codec — PCB trace / solder issue.
        // =================================================================
        Serial.println("\n[HW TEST] === HARDWARE CONNECTIVITY TEST ===");
        Serial.println("[HW TEST] Disabling I2S to reclaim GPIO pins...");

        // Disable and delete the I2S channel
        i2s_channel_disable(txHandle);
        i2s_del_channel(txHandle);
        txHandle = nullptr;
        delay(50);

        // Reconfigure as plain GPIO output
        gpio_reset_pin((gpio_num_t)PIN_I2S_DOUT);
        gpio_reset_pin((gpio_num_t)PIN_I2S_BCLK);
        gpio_reset_pin((gpio_num_t)PIN_I2S_LRCLK);
        gpio_set_direction((gpio_num_t)PIN_I2S_DOUT, GPIO_MODE_OUTPUT);
        gpio_set_direction((gpio_num_t)PIN_I2S_BCLK, GPIO_MODE_OUTPUT);
        gpio_set_direction((gpio_num_t)PIN_I2S_LRCLK, GPIO_MODE_OUTPUT);

        // Put codec in Deep Sleep first, then back to HIZ with clocks present
        tasSelectBookPage(0x00, 0x00);
        i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_DEEP_SLEEP);
        delay(10);

        // Bit-bang: simulate I2S clocks for ~500ms
        // BCLK toggles every iteration, LRCLK toggles every 32 BCLK cycles
        // At ~1.5 MHz BCLK, this gives ~48kHz LRCLK
        Serial.println("[HW TEST] Bit-banging BCLK (GPIO4) + LRCLK (GPIO3) for 500ms...");
        Serial.println("[HW TEST] BCLK ~ 1.5 MHz, LRCLK ~ 48 kHz (32 BCLK per cycle)");

        uint32_t startBB = millis();
        uint32_t bclkCount = 0;
        while (millis() - startBB < 500) {
            // 32 BCLK cycles per LRCLK half-period
            for (int half = 0; half < 2; half++) {
                gpio_set_level((gpio_num_t)PIN_I2S_LRCLK, half);
                for (int b = 0; b < 32; b++) {
                    gpio_set_level((gpio_num_t)PIN_I2S_BCLK, 1);
                    gpio_set_level((gpio_num_t)PIN_I2S_BCLK, 0);
                    bclkCount++;
                }
            }
        }
        uint32_t elapsed = millis() - startBB;
        float bclkFreq = (float)bclkCount / ((float)elapsed / 1000.0f);
        float lrclkFreq = bclkFreq / 64.0f;
        Serial.printf("[HW TEST] Bit-banged %u BCLK edges in %u ms\n", bclkCount, elapsed);
        Serial.printf("[HW TEST] Effective BCLK=%.0f Hz, LRCLK=%.0f Hz\n", bclkFreq, lrclkFreq);

        // Now wake the codec and check FS_MON
        // Keep bit-banging while transitioning to HIZ
        // (We need clocks present during HIZ for PLL to lock)

        // Start a background bit-bang for 2 seconds, checking FS_MON periodically
        Serial.println("[HW TEST] Transitioning codec to HIZ while bit-banging...");
        tasSelectBookPage(0x00, 0x00);
        i2cWriteReg(TAS5825M_ADDR, TAS_REG_DEVICE_CTRL, TAS_CTRL_HIZ);

        // Bit-bang for 2s, check FS_MON every 250ms
        for (int check = 0; check < 8; check++) {
            uint32_t bbStart = millis();
            while (millis() - bbStart < 250) {
                for (int half = 0; half < 2; half++) {
                    gpio_set_level((gpio_num_t)PIN_I2S_LRCLK, half);
                    for (int b = 0; b < 32; b++) {
                        gpio_set_level((gpio_num_t)PIN_I2S_BCLK, 1);
                        gpio_set_level((gpio_num_t)PIN_I2S_BCLK, 0);
                    }
                }
            }
            uint8_t fsMon = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
            uint8_t pwr   = i2cReadReg(TAS5825M_ADDR, TAS_REG_POWER_STATE);
            const char* marker = (fsMon != 0x00) ? " *** DETECTED! ***" : "";
            Serial.printf("[HW TEST]   t=%dms: FS_MON=0x%02X  PWR=0x%02X%s\n",
                          (check + 1) * 250, fsMon, pwr, marker);
            if (fsMon != 0x00) {
                Serial.println("[HW TEST] *** CODEC SEES BIT-BANGED CLOCKS — connectivity OK ***");
                break;
            }
        }

        uint8_t finalFs = i2cReadReg(TAS5825M_ADDR, TAS_REG_FS_MON);
        if (finalFs == 0x00) {
            Serial.println("\n[HW TEST] *** CODEC DOES NOT SEE BIT-BANGED CLOCKS ***");
            Serial.println("[HW TEST] CONCLUSION: I2S signals are NOT reaching the TAS5825M.");
            Serial.println("[HW TEST] Check PCB traces: GPIO4→TAS_SCK, GPIO3→TAS_FSYNC, GPIO1→TAS_SDIN");
            Serial.println("[HW TEST] Possible causes:");
            Serial.println("[HW TEST]   - Open / cold solder joint on BCLK or LRCLK trace");
            Serial.println("[HW TEST]   - Traces routed to wrong TAS5825M pins");
            Serial.println("[HW TEST]   - Level issue (TAS5825M needs 3.3V logic, check DVDD)");
        }

        // Re-init I2S for the loop() sine wave output
        Serial.println("\n[HW TEST] Re-initializing I2S for continued sine wave output...");
        gpio_reset_pin((gpio_num_t)PIN_I2S_DOUT);
        gpio_reset_pin((gpio_num_t)PIN_I2S_BCLK);
        gpio_reset_pin((gpio_num_t)PIN_I2S_LRCLK);
        initI2S();
        delay(100);

        Serial.println("\n*** CODEC FAILED TO ENTER PLAY — continuing sine output for scope debugging ***\n");
    }

    lastDiagTime_ms = millis();
    Serial.println("[LOOP] Starting sine wave output...\n");
}

// ============================================================================
// Main Loop — Generate and output sine wave
// ============================================================================

void loop() {
    // Toggle LED every 500ms as heartbeat
    static uint32_t ledToggle = 0;
    if (millis() - ledToggle > 500) {
        ledToggle = millis();
        digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    }

    // Fill buffer with sine wave samples
    fillSineBuffer();

    // Write to I2S (blocks until DMA has space)
    size_t bytesWritten = 0;
    esp_err_t err = i2s_channel_write(txHandle, writeBuf, WRITE_BUF_FRAMES * 4, &bytesWritten, 1000);
    if (err == ESP_OK) {
        totalFramesWritten += bytesWritten / 4;
    } else {
        i2sWriteErrors++;
        if (i2sWriteErrors <= 5) {
            Serial.printf("[I2S] Write error: %d (%s)\n", err, esp_err_to_name(err));
        }
    }

    // Periodic diagnostics every 5 seconds
    if (millis() - lastDiagTime_ms >= 5000) {
        lastDiagTime_ms = millis();
        dumpCodecDiag();
    }
}
