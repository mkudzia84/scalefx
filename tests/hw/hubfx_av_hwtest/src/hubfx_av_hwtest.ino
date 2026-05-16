/**
 * @file hubfx_av_hwtest.ino
 * @brief HubFX combined A/V bring-up: synchronised LED breathing + audio
 *        amplitude envelope on the same gamma-corrected sin² curve.
 *
 * What this demonstrates:
 *   - 8 LED rails fading in / out together via PCA9685 (U54, I²C 0x70),
 *     1526 Hz PWM, 12-bit duty, ALL_LED broadcast updates at 60 Hz.
 *   - 440 Hz sine tone whose amplitude follows the same breathing curve,
 *     played through the TAS5825P codec (U55, I²C 0x4C), I²S DOUT=GPIO16
 *     BCLK=GPIO17 LRCLK=GPIO18 at 48 kHz / 16-bit / stereo.
 *   - Both subsystems share a single `breathingEnvelope(elapsed_ms)`
 *     function. Each core reads its own `millis() - bootTime` and feeds
 *     it into the same curve — no cross-core atomics needed, the curves
 *     are deterministic and stay synchronised.
 *
 * Architecture:
 *   Core 0 (Arduino loopTask):
 *     - I²C bring-up, bus health check
 *     - PCA9685 init (SWRST + PRESCALE for 1526 Hz + wake)
 *     - TAS5825P init phase 1 (reset → DEEP_SLEEP)
 *     - Wait for I²S clocks
 *     - TAS5825P init phase 2 (HIZ → PLAY)
 *     - 60 Hz LED tick (ALL_LED broadcast)
 *     - 1 Hz status print
 *   Core 1 (FreeRTOS task):
 *     - ESP-IDF I²S TX channel setup
 *     - Continuous sine wave generation, amplitude scaled by breathing
 *       envelope, blocking I²S DMA writes for natural pacing
 *
 * Detailed references:
 *   - PCA9685 init details: ../hubfx_pca9685_hwtest/README.md
 *   - TAS5825P init details: ../sfx_test_p/ + the production
 *     controllers/lib/sfx_audio/codec/tas5825_p_codec.cpp
 *   - HubFX pinout / I²C address map: ../../../controllers/hubfx/esp32s3/PINOUT.md
 *
 * NO external libraries — pure Arduino + Wire + ESP-IDF I²S driver.
 */

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <cmath>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_chip_info.h>
#include <esp_system.h>

// ============================================================================
//  PIN / BUS / AUDIO CONFIG
// ============================================================================

// I²S pins — HubFX 8-channel rev (see PINOUT.md).
static constexpr gpio_num_t PIN_I2S_DOUT  = GPIO_NUM_16;
static constexpr gpio_num_t PIN_I2S_BCLK  = GPIO_NUM_17;
static constexpr gpio_num_t PIN_I2S_LRCLK = GPIO_NUM_18;

// I²C pins — shared bus.
static constexpr int      PIN_I2C_SDA  = 8;
static constexpr int      PIN_I2C_SCL  = 9;
static constexpr uint32_t I2C_CLOCK_HZ = 100000;

static constexpr int PIN_HEARTBEAT_LED = 48;

// Device addresses.
static constexpr uint8_t TAS5825P_ADDR = 0x4C;
static constexpr uint8_t PCA9685_ADDR  = 0x70;

// Audio parameters.
static constexpr uint32_t SAMPLE_RATE      = 48000;
static constexpr uint32_t BIT_DEPTH        = 16;
static constexpr size_t   FRAMES_PER_BATCH = 512;

// "Sad whale" call: pitch glissandos with the breathing envelope.
// Frequency rises from MIN_HZ at the trough to MAX_HZ at the peak,
// then falls back — same sin² shape as the LED brightness and the
// audio amplitude. The deep low end + slow glide gives the call its
// mournful character.
static constexpr float    WHALE_FREQ_MIN_HZ = 80.0f;
static constexpr float    WHALE_FREQ_MAX_HZ = 300.0f;
static constexpr float    AUDIO_PEAK_LEVEL  = 0.40f;   // peak amplitude as fraction of full-scale int16

// LED parameters.
static constexpr uint16_t DUTY_MAX     = 4095;
static constexpr float    PEAK_FRACTION = 1.00f;   // peak LED duty as fraction of DUTY_MAX
static constexpr uint16_t PEAK_DUTY    =
    (uint16_t)((float)DUTY_MAX * PEAK_FRACTION + 0.5f);
static constexpr uint32_t LED_FRAME_HZ = 60;

// When true, drive LEDs at constant PEAK_DUTY every frame (no envelope
// modulation). Audio path is unaffected — the sad-whale call still
// sweeps amplitude + pitch with the breathing envelope. Diagnostic
// mode for confirming the LED rail is steady at peak current.
static constexpr bool     LED_CONSTANT_ON = false;

// Breathing envelope.
static constexpr uint32_t BREATHING_PERIOD_MS = 4000;
static constexpr float    BREATHING_GAMMA     = 2.2f;

// Diagnostic cadence.
static constexpr uint32_t DIAG_INTERVAL_MS = 1000;

// ============================================================================
//  PCA9685 REGISTER MAP (only what we touch)
// ============================================================================

static constexpr uint8_t PCA_MODE1         = 0x00;
static constexpr uint8_t PCA_MODE2         = 0x01;
static constexpr uint8_t PCA_LED0_ON_L     = 0x06;
static constexpr uint8_t PCA_ALL_LED_ON_L  = 0xFA;
static constexpr uint8_t PCA_PRESCALE      = 0xFE;

static constexpr uint8_t PCA_MODE1_RESTART = 0x80;
static constexpr uint8_t PCA_MODE1_AI      = 0x20;
static constexpr uint8_t PCA_MODE1_SLEEP   = 0x10;
static constexpr uint8_t PCA_MODE1_ALLCALL = 0x01;
static constexpr uint8_t PCA_MODE2_OUTDRV  = 0x04;

// PRESCALE for 1526 Hz (chip max — above human flicker fusion).
// prescale = round(25 MHz / (4096 × 1526)) − 1 = 3
static constexpr uint8_t PCA_PRESCALE_VALUE = 0x03;

// I²C general-call SWRST — write 0x06 to address 0x00 to reset every
// PCA9685 with ALLCALL enabled. NXP UM10851 §7.1.4.
static constexpr uint8_t GENERAL_CALL_ADDR = 0x00;
static constexpr uint8_t PCA_SWRST_BYTE    = 0x06;

// ============================================================================
//  TAS5825P REGISTER MAP (only what we touch)
// ============================================================================

static constexpr uint8_t TAS_REG_PAGE         = 0x00;
static constexpr uint8_t TAS_REG_RESET        = 0x01;
static constexpr uint8_t TAS_REG_MODE_CTRL    = 0x02;
static constexpr uint8_t TAS_REG_DEVICE_CTRL  = 0x03;
static constexpr uint8_t TAS_REG_SDOUT_SEL    = 0x30;
static constexpr uint8_t TAS_REG_CLK_SRC      = 0x33;
static constexpr uint8_t TAS_REG_FS_RATE      = 0x34;
static constexpr uint8_t TAS_REG_FS_MON       = 0x37;
static constexpr uint8_t TAS_REG_ANALOG_CTRL  = 0x46;
static constexpr uint8_t TAS_REG_DIGITAL_VOL  = 0x4C;
static constexpr uint8_t TAS_REG_AGAIN_L      = 0x53;
static constexpr uint8_t TAS_REG_AGAIN_R      = 0x54;
static constexpr uint8_t TAS_REG_SAP_CTRL1    = 0x60;
static constexpr uint8_t TAS_REG_DSP_MISC     = 0x62;
static constexpr uint8_t TAS_REG_POWER_STATE  = 0x68;
static constexpr uint8_t TAS_REG_GLOBAL1      = 0x71;
static constexpr uint8_t TAS_REG_FAULT_CLEAR  = 0x78;
static constexpr uint8_t TAS_REG_BOOK         = 0x7F;

static constexpr uint8_t TAS_MODE_DEEP_SLEEP = 0x00;
static constexpr uint8_t TAS_MODE_HIZ        = 0x02;
static constexpr uint8_t TAS_MODE_PLAY       = 0x03;

static constexpr uint8_t TAS_SAP_WORD_16BIT  = 0x00;

static constexpr uint8_t TAS_RESET_FULL       = 0x11;
static constexpr uint8_t TAS_FAULT_CLEAR_CMD  = 0x80;
static constexpr uint8_t TAS_DIGVOL_0DB       = 0x30;
static constexpr uint8_t TAS_AGAIN_12V        = 0x10;

static constexpr uint8_t TAS_DSP_BOOK = 0x8C;
static constexpr uint8_t TAS_DSP_PAGE = 0x0B;

// ============================================================================
//  TYPES + GLOBALS
// ============================================================================

struct StereoSample {
    int16_t left;
    int16_t right;
};

static i2s_chan_handle_t i2sHandle       = nullptr;
static TaskHandle_t      audioTaskHandle = nullptr;

static std::atomic<bool>     i2sRunning{false};
static std::atomic<bool>     codecInPlay{false};
static std::atomic<bool>     pcaConfigured{false};
static std::atomic<uint32_t> audioFrames{0};
static std::atomic<uint32_t> audioWriteErrors{0};
static std::atomic<uint32_t> ledFrames{0};
static std::atomic<uint32_t> ledNacks{0};
static std::atomic<int16_t>  audioPeakSample{0};
static std::atomic<uint16_t> ledLastDuty{0};

// Sine-wave phase (Core 1 only).
static float sinePhase = 0.0f;

// DMA-safe internal-SRAM batch buffer.
static StereoSample batchBuffer[FRAMES_PER_BATCH];

// Common time origin shared by both cores. Set once in setup().
static uint32_t bootTime_ms = 0;

// ============================================================================
//  BREATHING ENVELOPE
//  Single source of truth for both audio amplitude and LED duty.
//  Returns a value in [0.0, 1.0] over the configured period.
// ============================================================================

static float breathingEnvelope(uint32_t elapsed_ms) {
    const float phase = (float)(elapsed_ms % BREATHING_PERIOD_MS)
                        / (float)BREATHING_PERIOD_MS;
    const float s         = sinf((float)M_PI * phase);
    const float raw       = s * s;                 // sin² → smooth 0 → 1 → 0 bell
    return powf(raw, BREATHING_GAMMA);             // gamma-correct
}

// ============================================================================
//  I²C HELPERS
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
    return Wire.read();
}

static bool i2cProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// ============================================================================
//  PCA9685 — minimal init
//  Full bring-up details in ../hubfx_pca9685_hwtest/README.md.
// ============================================================================

static bool pcaSWRST() {
    Wire.beginTransmission(GENERAL_CALL_ADDR);
    Wire.write(PCA_SWRST_BYTE);
    const bool ok = (Wire.endTransmission() == 0);
    Serial.printf("[PCA] SWRST (general call 0x00 ← 0x06): %s\n",
                  ok ? "ACK ✓" : "NACK ✗");
    return ok;
}

static bool pcaInit() {
    Serial.printf("[PCA] Probing @ 0x%02X... ", PCA9685_ADDR);
    if (!i2cProbe(PCA9685_ADDR)) {
        Serial.println("NOT FOUND");
        return false;
    }
    Serial.println("OK");

    pcaSWRST();
    delay(2);

    // SLEEP + ALLCALL — PRESCALE write requires SLEEP=1.
    i2cWriteReg(PCA9685_ADDR, PCA_MODE1, PCA_MODE1_SLEEP | PCA_MODE1_ALLCALL);
    delay(1);
    i2cWriteReg(PCA9685_ADDR, PCA_PRESCALE, PCA_PRESCALE_VALUE);
    i2cWriteReg(PCA9685_ADDR, PCA_MODE2, PCA_MODE2_OUTDRV);
    // Wake: SLEEP=0, AI=1 for burst writes.
    i2cWriteReg(PCA9685_ADDR, PCA_MODE1, PCA_MODE1_AI | PCA_MODE1_ALLCALL);
    delayMicroseconds(500);  // §7.3.1.1 oscillator stabilisation
    i2cWriteReg(PCA9685_ADDR, PCA_MODE1,
                PCA_MODE1_AI | PCA_MODE1_ALLCALL | PCA_MODE1_RESTART);
    delay(1);

    // Park all channels at duty 0 (FULL_OFF via OFF_H bit 4).
    Wire.beginTransmission(PCA9685_ADDR);
    Wire.write(PCA_ALL_LED_ON_L);
    Wire.write(0x00); Wire.write(0x00);
    Wire.write(0x00); Wire.write(0x10);
    Wire.endTransmission();

    Serial.printf("[PCA] Configured: PRESCALE=0x%02X (%.0f Hz), MODE2=push-pull\n",
                  PCA_PRESCALE_VALUE,
                  25000000.0f / (4096.0f * (PCA_PRESCALE_VALUE + 1)));
    pcaConfigured.store(true, std::memory_order_release);
    return true;
}

// Always-PWM ALL_LED broadcast — no FULL_ON / FULL_OFF flag mode.
// Clamps duty to [1, DUTY_MAX] so we never hit the ON==OFF edge case.
static bool pcaWriteAllChannels(uint16_t duty) {
    if (duty < 1)         duty = 1;
    if (duty > DUTY_MAX)  duty = DUTY_MAX;

    Wire.beginTransmission(PCA9685_ADDR);
    Wire.write(PCA_ALL_LED_ON_L);
    Wire.write(0x00);                              // ON_L
    Wire.write(0x00);                              // ON_H (no FULL_ON)
    Wire.write((uint8_t)(duty & 0xFF));            // OFF_L
    Wire.write((uint8_t)((duty >> 8) & 0x0F));     // OFF_H (no FULL_OFF)
    return (Wire.endTransmission() == 0);
}

// ============================================================================
//  TAS5825P — minimal init
//  Full diagnostic flow in ../sfx_test_p/.
//  This fixture uses the proven init sequence without the verbose verify.
// ============================================================================

static void tasSelectBookPage(uint8_t book, uint8_t page) {
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_PAGE, 0x00);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_BOOK, book);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_PAGE, page);
}

// Phase 1: probe, reset, park in DEEP_SLEEP. Safe before I²S clocks
// are running because the codec's PLL is off in DEEP_SLEEP.
static bool tasPhase1Reset() {
    Serial.printf("[TAS] Probing @ 0x%02X... ", TAS5825P_ADDR);
    if (!i2cProbe(TAS5825P_ADDR)) {
        Serial.println("NOT FOUND");
        return false;
    }
    Serial.println("OK");

    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_DEVICE_CTRL, TAS_MODE_DEEP_SLEEP);
    delay(5);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_RESET, TAS_RESET_FULL);
    delay(50);
    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_DEVICE_CTRL, TAS_MODE_DEEP_SLEEP);
    delay(5);

    const uint8_t pwr = i2cReadReg(TAS5825P_ADDR, TAS_REG_POWER_STATE);
    Serial.printf("[TAS] After reset: POWER_STATE=0x%02X (expect 0x00=DEEP_SLEEP)\n",
                  pwr);
    return true;
}

// Phase 2: configure analog gain + clock regs + DSP coefficients, then
// DEEP_SLEEP → HIZ → PLAY. MUST be called only after I²S clocks are
// live so the codec's PLL has something to lock to.
static bool tasPhase2Activate() {
    Serial.println("[TAS] Post-clock configuration...");

    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_ANALOG_CTRL, 0x11);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_MODE_CTRL,   0x00);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_AGAIN_L,     0x01);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_AGAIN_R,     TAS_AGAIN_12V);

    i2cWriteReg(TAS5825P_ADDR, TAS_REG_CLK_SRC,   0x00);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_FS_RATE,   0x00);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_SDOUT_SEL, 0x00);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_SAP_CTRL1, TAS_SAP_WORD_16BIT);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_DSP_MISC,  0x09);

    // DSP identity coefficients (book 0x8C, page 0x0B).
    tasSelectBookPage(TAS_DSP_BOOK, TAS_DSP_PAGE);
    static const uint8_t identity[] = {
        0x00, 0x80, 0x00, 0x00,
        0x00, 0x80, 0x00, 0x00,
    };
    for (size_t i = 0; i < sizeof(identity); i++) {
        i2cWriteReg(TAS5825P_ADDR, 0x28 + i, identity[i]);
    }

    tasSelectBookPage(0x00, 0x00);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_DIGITAL_VOL, TAS_DIGVOL_0DB);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_DEVICE_CTRL, TAS_MODE_HIZ);
    delay(50);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_DEVICE_CTRL, TAS_MODE_PLAY);
    delay(20);
    i2cWriteReg(TAS5825P_ADDR, TAS_REG_FAULT_CLEAR, TAS_FAULT_CLEAR_CMD);
    delay(20);

    const uint8_t pwr   = i2cReadReg(TAS5825P_ADDR, TAS_REG_POWER_STATE);
    const uint8_t fsMon = i2cReadReg(TAS5825P_ADDR, TAS_REG_FS_MON);
    const uint8_t fault = i2cReadReg(TAS5825P_ADDR, TAS_REG_GLOBAL1);
    Serial.printf("[TAS] After PLAY: POWER_STATE=0x%02X FS_MON=0x%02X GLOBAL_FAULT1=0x%02X\n",
                  pwr, fsMon, fault);

    if ((pwr & 0x0F) == TAS_MODE_PLAY) {
        Serial.println("[TAS] ✓ codec in PLAY");
        codecInPlay.store(true, std::memory_order_release);
        return true;
    }
    Serial.println("[TAS] ✗ codec did not enter PLAY — audio will be silent");
    return false;
}

// ============================================================================
//  I²S SETUP + AUDIO TASK (Core 1)
// ============================================================================

static bool initI2S() {
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num  = 8;
    chanCfg.dma_frame_num = 512;
    chanCfg.auto_clear_after_cb = true;

    if (i2s_new_channel(&chanCfg, &i2sHandle, nullptr) != ESP_OK) return false;

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
    if (i2s_channel_init_std_mode(i2sHandle, &stdCfg) != ESP_OK) return false;
    if (i2s_channel_enable(i2sHandle) != ESP_OK) return false;
    return true;
}

// Generate one batch of "sad whale" samples — sine with frequency AND
// amplitude both driven by the breathing envelope. Envelope is sampled
// once per batch (10.67 ms at 48 kHz, ~94 Hz update rate) and held
// constant across the 512 samples. The 10 ms quantisation is well
// below audible glissando granularity for a 4-second sweep, so no
// stepping is perceptible.
static void generateSineBatch() {
    const uint32_t elapsed = millis() - bootTime_ms;
    const float envelope   = breathingEnvelope(elapsed);
    const float amp        = AUDIO_PEAK_LEVEL * envelope * 32767.0f;

    // Pitch glissando: env=0 → MIN_HZ, env=1 → MAX_HZ.
    const float freqHz   = WHALE_FREQ_MIN_HZ +
                           envelope * (WHALE_FREQ_MAX_HZ - WHALE_FREQ_MIN_HZ);
    const float phaseInc = (2.0f * (float)M_PI * freqHz) / (float)SAMPLE_RATE;

    int16_t peak = 0;
    for (size_t i = 0; i < FRAMES_PER_BATCH; i++) {
        const int16_t sample = (int16_t)(sinf(sinePhase) * amp);
        batchBuffer[i].left  = sample;
        batchBuffer[i].right = sample;

        const int16_t absSample = (sample < 0) ? -sample : sample;
        if (absSample > peak) peak = absSample;

        sinePhase += phaseInc;
    }
    if (sinePhase >= 2.0f * (float)M_PI) sinePhase -= 2.0f * (float)M_PI;

    audioPeakSample.store(peak, std::memory_order_relaxed);
}

static void audioTask(void* /*arg*/) {
    Serial.printf("[Audio] Task on Core %d (priority %d)\n",
                  xPortGetCoreID(), uxTaskPriorityGet(nullptr));

    if (!initI2S()) {
        Serial.println("[Audio] FATAL: I²S init failed");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    i2sRunning.store(true, std::memory_order_release);
    Serial.println("[Audio] I²S TX channel up — clocks running");

    // Pre-fill so the codec's PLL sees clocks immediately when we
    // transition it to HIZ.
    for (int i = 0; i < 4; i++) {
        generateSineBatch();
        size_t written = 0;
        i2s_channel_write(i2sHandle, batchBuffer,
                          FRAMES_PER_BATCH * sizeof(StereoSample),
                          &written, portMAX_DELAY);
    }
    Serial.println("[Audio] DMA pre-filled, entering continuous output loop");

    while (true) {
        generateSineBatch();
        size_t written = 0;
        esp_err_t err = i2s_channel_write(i2sHandle, batchBuffer,
                                          FRAMES_PER_BATCH * sizeof(StereoSample),
                                          &written, portMAX_DELAY);
        const uint32_t frames = written / sizeof(StereoSample);
        audioFrames.fetch_add(frames, std::memory_order_relaxed);
        if (err != ESP_OK || frames != FRAMES_PER_BATCH) {
            audioWriteErrors.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ============================================================================
//  BOOT-TIME LOGGING
// ============================================================================

static void printBanner() {
    Serial.println();
    Serial.println("================================================================");
    Serial.println("  HubFX A/V hwtest — synchronised LED breathing + audio fade");
    Serial.println("  PCA9685 LED PWM (0x70) + TAS5825P codec (0x4C)");
    Serial.println("  See controllers/hubfx/esp32s3/PINOUT.md");
    Serial.println("================================================================");
    Serial.println();
}

static void printSystemInfo() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    Serial.println("--- System Info ---");
    Serial.printf("  Chip:          ESP32-S3 rev %d, %d core(s), %d MHz\n",
                  chip.revision, chip.cores, ESP.getCpuFreqMHz());
    Serial.printf("  Free heap:     %lu bytes\n", ESP.getFreeHeap());
    Serial.printf("  SDK version:   %s\n", ESP.getSdkVersion());
    Serial.println();
}

static void printAVConfig() {
    Serial.println("--- A/V Configuration ---");
    Serial.printf("  Breathing period: %lu ms,  γ = %.1f\n",
                  (unsigned long)BREATHING_PERIOD_MS, BREATHING_GAMMA);
    Serial.println("  Audio:");
    Serial.printf("    %lu Hz / %lu-bit / stereo, peak %.0f%% FS\n",
                  (unsigned long)SAMPLE_RATE, (unsigned long)BIT_DEPTH,
                  AUDIO_PEAK_LEVEL * 100.0f);
    Serial.printf("    \"sad whale\" pitch sweep: %.0f Hz → %.0f Hz → %.0f Hz per breath\n",
                  WHALE_FREQ_MIN_HZ, WHALE_FREQ_MAX_HZ, WHALE_FREQ_MIN_HZ);
    Serial.printf("    pins: DOUT=GPIO%d BCLK=GPIO%d LRCLK=GPIO%d\n",
                  (int)PIN_I2S_DOUT, (int)PIN_I2S_BCLK, (int)PIN_I2S_LRCLK);
    Serial.println("  LED:");
    Serial.printf("    PWM 1526 Hz (PRESCALE=0x%02X), 12-bit duty, %lu Hz update rate\n",
                  PCA_PRESCALE_VALUE, (unsigned long)LED_FRAME_HZ);
    Serial.printf("    peak duty %u/%u (%.0f%% of range), 8 channels via ALL_LED\n",
                  PEAK_DUTY, DUTY_MAX, PEAK_FRACTION * 100.0f);
    Serial.println();
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    uint32_t serialWait = millis();
    while (!Serial && (millis() - serialWait < 3000)) delay(10);

    printBanner();
    printSystemInfo();
    printAVConfig();

    pinMode(PIN_HEARTBEAT_LED, OUTPUT);
    digitalWrite(PIN_HEARTBEAT_LED, HIGH);

    // -- I²C bring-up (3-arg form, see hubfx_pca9685_hwtest README) --
    Serial.printf("[I2C] Initializing: SDA=GPIO%d, SCL=GPIO%d, %lu Hz\n",
                  PIN_I2C_SDA, PIN_I2C_SCL, (unsigned long)I2C_CLOCK_HZ);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ);

    // -- Quick bus scan so the user sees what's reachable --
    Serial.println("[I2C] Scanning bus...");
    int found = 0;
    for (uint8_t a = 0x08; a <= 0x7F; a++) {
        if (i2cProbe(a)) {
            Serial.printf("[I2C]   0x%02X ACK\n", a);
            found++;
        }
    }
    Serial.printf("[I2C] %d device(s) found.\n", found);

    // -- PCA9685 init (no animation yet — just configure) --
    if (!pcaInit()) {
        Serial.println("*** PCA9685 init failed — LEDs will stay off ***");
    }

    // -- TAS5825P phase 1 (before I²S clocks) --
    bool codecFound = tasPhase1Reset();
    if (!codecFound) {
        Serial.println("*** TAS5825P not found — audio will be silent ***");
    }

    // -- Spawn audio task on Core 1 (which brings up I²S) --
    Serial.println("[Main] Launching audio task on Core 1...");
    xTaskCreatePinnedToCore(audioTask, "AudioSine", 8192, nullptr,
                            configMAX_PRIORITIES - 1, &audioTaskHandle, 1);

    // -- Wait up to 5 s for I²S to come up --
    const uint32_t waitStart = millis();
    while (!i2sRunning.load(std::memory_order_acquire) &&
           (millis() - waitStart) < 5000) {
        delay(20);
    }
    if (!i2sRunning.load(std::memory_order_acquire)) {
        Serial.println("[Main] WARNING: I²S did not come up in 5 s");
    } else {
        Serial.println("[Main] I²S confirmed running");
    }

    // -- TAS5825P phase 2 (codec needs live I²S clocks to lock PLL) --
    if (codecFound) {
        delay(200);   // let DMA pump a few batches so BCK/LRCLK are stable
        tasPhase2Activate();
    }

    bootTime_ms = millis();  // both cores reference this for the envelope

    Serial.println();
    Serial.println("─────────  A/V BREATHING ACTIVE  ─────────");
    Serial.printf("  Codec in PLAY:    %s\n",
                  codecInPlay.load(std::memory_order_acquire) ? "YES" : "no");
    Serial.printf("  PCA9685 ready:    %s\n",
                  pcaConfigured.load(std::memory_order_acquire) ? "YES" : "no");
    Serial.println("  Both subsystems share `breathingEnvelope(elapsed_ms)`");
    Serial.println("  → LED brightness and audio amplitude rise and fall together");
    Serial.println("──────────────────────────────────────────");
    Serial.println();
}

// ============================================================================
//  LOOP — LED tick at 60 Hz + 1 Hz status print
// ============================================================================

void loop() {
    static uint32_t lastFrame_ms     = 0;
    static uint32_t lastStatus_ms    = 0;
    static uint32_t lastHeartbeat_ms = 0;
    static uint32_t lastFrameSnap    = 0;
    static uint32_t lastAudioSnap    = 0;
    static uint32_t statusCount      = 0;

    const uint32_t now = millis();

    if (now - lastHeartbeat_ms >= 500) {
        lastHeartbeat_ms = now;
        digitalWrite(PIN_HEARTBEAT_LED, !digitalRead(PIN_HEARTBEAT_LED));
    }

    // LED tick — single ALL_LED broadcast per frame.
    const uint32_t framePeriodMs = 1000UL / LED_FRAME_HZ;
    if (pcaConfigured.load(std::memory_order_relaxed) &&
        now - lastFrame_ms >= framePeriodMs) {
        lastFrame_ms = now;
        uint16_t duty;
        if (LED_CONSTANT_ON) {
            duty = PEAK_DUTY;
        } else {
            const uint32_t elapsed = now - bootTime_ms;
            const float    env     = breathingEnvelope(elapsed);
            duty = (uint16_t)(env * (float)PEAK_DUTY + 0.5f);
        }
        if (!pcaWriteAllChannels(duty)) {
            ledNacks.fetch_add(1, std::memory_order_relaxed);
        }
        ledLastDuty.store(duty, std::memory_order_relaxed);
        ledFrames.fetch_add(1, std::memory_order_relaxed);
    }

    // Status print every second.
    if (now - lastStatus_ms >= DIAG_INTERVAL_MS) {
        lastStatus_ms = now;
        statusCount++;

        const uint32_t curLedFrames    = ledFrames.load(std::memory_order_relaxed);
        const uint32_t curAudioFrames  = audioFrames.load(std::memory_order_relaxed);
        const uint32_t deltaLedFrames   = curLedFrames   - lastFrameSnap;
        const uint32_t deltaAudioFrames = curAudioFrames - lastAudioSnap;
        lastFrameSnap = curLedFrames;
        lastAudioSnap = curAudioFrames;

        const float envNow = breathingEnvelope(now - bootTime_ms);
        const float audioFps = (float)deltaAudioFrames * 1000.0f / (float)DIAG_INTERVAL_MS;

        if ((statusCount % 20) == 1) {
            Serial.println();
            Serial.println("  #  Uptime  Env   LED duty  Audio samp/s  Audio peak  LED NACKs  Audio errs");
            Serial.println("---  ------  ----  --------  ------------  ----------  ---------  ----------");
        }

        Serial.printf("%3lu  %5.1fs  %.2f  %4u/%u  %12.0f  %10d  %9lu  %10lu\n",
                      (unsigned long)statusCount,
                      (now - bootTime_ms) / 1000.0f,
                      envNow,
                      ledLastDuty.load(std::memory_order_relaxed), DUTY_MAX,
                      audioFps,
                      audioPeakSample.load(std::memory_order_relaxed),
                      (unsigned long)ledNacks.load(std::memory_order_relaxed),
                      (unsigned long)audioWriteErrors.load(std::memory_order_relaxed));
    }
}
