/**
 * HubFX LED Blink Test
 *
 * Bare-metal test firmware for the PCAL6416A I2C GPIO expander on the
 * HubFX ESP32-S3 board.  Drives all 8 LED channels (Port 0, P0_0–P0_7)
 * with a 1 Hz blink (500 ms on / 500 ms off).
 *
 * Every 5 seconds the current I2C configuration and LED state are printed
 * to the serial console at 115200 baud.
 *
 * Hardware
 * --------
 *   I2C bus:        SDA = GPIO 8, SCL = GPIO 9, 100 kHz
 *   GPIO expander:  NXP PCAL6416A @ 0x20
 *   LED channels:   Port 0 (P0_0 – P0_7), accent low = LED on
 *
 * No ScaleFX library code is used — pure Arduino Wire calls only.
 */

#include <Arduino.h>
#include <Wire.h>

// ── Hardware constants ──────────────────────────────────────────────────

static constexpr uint8_t PIN_I2C_SDA     = 8;
static constexpr uint8_t PIN_I2C_SCL     = 9;
static constexpr uint32_t I2C_FREQ_HZ    = 100000;

static constexpr uint8_t PCAL6416A_ADDR  = 0x20;

// PCAL6416A register addresses (see NXP datasheet)
static constexpr uint8_t REG_INPUT_PORT_0     = 0x00;
static constexpr uint8_t REG_INPUT_PORT_1     = 0x01;
static constexpr uint8_t REG_OUTPUT_PORT_0    = 0x02;
static constexpr uint8_t REG_OUTPUT_PORT_1    = 0x03;
static constexpr uint8_t REG_CONFIG_PORT_0    = 0x06;   // 1 = input, 0 = output
static constexpr uint8_t REG_CONFIG_PORT_1    = 0x07;
static constexpr uint8_t REG_PUPD_ENABLE_0    = 0x46;
static constexpr uint8_t REG_PUPD_ENABLE_1    = 0x47;
static constexpr uint8_t REG_PUPD_SELECT_0    = 0x48;
static constexpr uint8_t REG_PUPD_SELECT_1    = 0x49;
static constexpr uint8_t REG_OUTPUT_PORT_CFG  = 0x4F;   // bit per port: 0 = push-pull, 1 = open-drain

// ── Timing ──────────────────────────────────────────────────────────────

static constexpr uint32_t BLINK_HALF_PERIOD_MS = 500;   // → 1 Hz blink
static constexpr uint32_t REPORT_INTERVAL_MS   = 5000;

// ── State ───────────────────────────────────────────────────────────────

static bool     ledsOn            = false;
static uint32_t lastToggle_ms     = 0;
static uint32_t lastReport_ms     = 0;
static bool     expanderPresent   = false;

// ── I2C helpers ─────────────────────────────────────────────────────────

/** Write a single byte to a register. Returns true on success. */
static bool writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(PCAL6416A_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

/** Read a single byte from a register.  Returns 0xFF on failure. */
static uint8_t readRegister(uint8_t reg) {
    Wire.beginTransmission(PCAL6416A_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFF;

    if (Wire.requestFrom(PCAL6416A_ADDR, (uint8_t)1) != 1) return 0xFF;
    return Wire.read();
}

// ── Bit-string helper ───────────────────────────────────────────────────

/** Format an 8-bit value as "b76543210" string. */
static void printBits(uint8_t v) {
    for (int i = 7; i >= 0; --i) {
        Serial.print((v >> i) & 1);
    }
}

// ── I2C bus scan ────────────────────────────────────────────────────────

static void scanI2C() {
    Serial.println("I2C bus scan:");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  0x%02X", addr);
            if (addr == PCAL6416A_ADDR) Serial.print(" ← PCAL6416A");
            Serial.println();
            ++found;
        }
    }
    if (found == 0) Serial.println("  (no devices found)");
    Serial.printf("  %d device(s)\n\n", found);
}

// ── Setup ───────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1500);   // let USB CDC settle

    Serial.println();
    Serial.println("========================================");
    Serial.println("  HubFX LED Blink Test");
    Serial.println("  PCAL6416A @ 0x20  —  Port 0 LEDs");
    Serial.println("========================================");
    Serial.println();

    // ── Init I2C ──
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    Serial.printf("I2C initialised  SDA=%d  SCL=%d  freq=%lu Hz\n\n",
                  PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);

    // ── Scan bus ──
    scanI2C();

    // ── Probe PCAL6416A ──
    Wire.beginTransmission(PCAL6416A_ADDR);
    expanderPresent = (Wire.endTransmission() == 0);

    if (!expanderPresent) {
        Serial.println("ERROR: PCAL6416A not found at 0x20!");
        Serial.println("Check wiring / pull-ups / power.");
        return;
    }
    Serial.println("PCAL6416A detected at 0x20");

    // ── Configure Port 0 as all outputs, push-pull ──

    // 1. Set Port 0 outputs HIGH first (LEDs off — active-low)
    if (!writeRegister(REG_OUTPUT_PORT_0, 0xFF)) {
        Serial.println("WARN: failed to write OUTPUT_PORT_0");
    }

    // 2. Direction: 0x00 = all 8 pins are outputs
    if (!writeRegister(REG_CONFIG_PORT_0, 0x00)) {
        Serial.println("WARN: failed to write CONFIG_PORT_0");
    }

    // 3. Push-pull mode for Port 0 (bit 0 = port 0)
    //    Read current value first so we don't clobber Port 1 setting.
    uint8_t outCfg = readRegister(REG_OUTPUT_PORT_CFG);
    outCfg &= ~0x01;   // clear bit 0 → push-pull for Port 0
    if (!writeRegister(REG_OUTPUT_PORT_CFG, outCfg)) {
        Serial.println("WARN: failed to write OUTPUT_PORT_CFG");
    }

    // 4. Disable pull resistors on Port 0 (not needed for push-pull outputs)
    writeRegister(REG_PUPD_ENABLE_0, 0x00);

    Serial.println("Port 0 configured: 8 outputs, push-pull, pull-ups disabled");
    Serial.println();

    // ── Verify readback ──
    uint8_t cfgBack = readRegister(REG_CONFIG_PORT_0);
    uint8_t outBack = readRegister(REG_OUTPUT_PORT_0);
    Serial.printf("  CONFIG_PORT_0  (0x06) = 0x%02X  [", cfgBack);
    printBits(cfgBack);
    Serial.println("]");
    Serial.printf("  OUTPUT_PORT_0  (0x02) = 0x%02X  [", outBack);
    printBits(outBack);
    Serial.println("]");
    Serial.println();

    Serial.println("Starting static pattern: CH0-3 ON, CH4-7 OFF\n");

    // Set static pattern: CH0-3 on (low), CH4-7 off (high) → 0xF0
    if (!writeRegister(REG_OUTPUT_PORT_0, 0xF0)) {
        Serial.println("ERROR: failed to set LED pattern");
    }

    lastReport_ms = millis();
}

// ── Main loop ───────────────────────────────────────────────────────────

void loop() {
    if (!expanderPresent) {
        // Nothing to do — blink onboard LED as heartbeat
        static uint32_t hb = 0;
        if (millis() - hb > 200) {
            hb = millis();
            // no onboard LED on this board variant — just idle
        }
        delay(100);
        return;
    }

    uint32_t now = millis();

    // ── Static half on / half off ──
    // (set once after setup, no toggling needed)

    // ── Periodic status report ──
    if (now - lastReport_ms >= REPORT_INTERVAL_MS) {
        lastReport_ms = now;

        Serial.println("────────────────────────────────────────");
        Serial.printf("Uptime: %lu ms\n", now);
        Serial.println();

        // Read back key registers
        uint8_t config0   = readRegister(REG_CONFIG_PORT_0);
        uint8_t config1   = readRegister(REG_CONFIG_PORT_1);
        uint8_t output0   = readRegister(REG_OUTPUT_PORT_0);
        uint8_t output1   = readRegister(REG_OUTPUT_PORT_1);
        uint8_t input0    = readRegister(REG_INPUT_PORT_0);
        uint8_t input1    = readRegister(REG_INPUT_PORT_1);
        uint8_t outCfg    = readRegister(REG_OUTPUT_PORT_CFG);
        uint8_t pupdEn0   = readRegister(REG_PUPD_ENABLE_0);
        uint8_t pupdSel0  = readRegister(REG_PUPD_SELECT_0);

        Serial.println("I2C Register Dump (PCAL6416A @ 0x20):");
        Serial.println();

        Serial.printf("  CONFIG_PORT_0  (0x06) = 0x%02X  [", config0);
        printBits(config0);
        Serial.println("]  (0=out, 1=in)");

        Serial.printf("  CONFIG_PORT_1  (0x07) = 0x%02X  [", config1);
        printBits(config1);
        Serial.println("]");

        Serial.println();

        Serial.printf("  OUTPUT_PORT_0  (0x02) = 0x%02X  [", output0);
        printBits(output0);
        Serial.printf("]  LEDs %s\n", (output0 == 0x00) ? "ALL ON" : (output0 == 0xFF) ? "ALL OFF" : "MIXED");

        Serial.printf("  OUTPUT_PORT_1  (0x03) = 0x%02X  [", output1);
        printBits(output1);
        Serial.println("]");

        Serial.println();

        Serial.printf("  INPUT_PORT_0   (0x00) = 0x%02X  [", input0);
        printBits(input0);
        Serial.println("]  (actual pin levels)");

        Serial.printf("  INPUT_PORT_1   (0x01) = 0x%02X  [", input1);
        printBits(input1);
        Serial.println("]");

        Serial.println();

        Serial.printf("  OUTPUT_CFG     (0x4F) = 0x%02X  P0=%s  P1=%s\n",
                      outCfg,
                      (outCfg & 0x01) ? "open-drain" : "push-pull",
                      (outCfg & 0x02) ? "open-drain" : "push-pull");

        Serial.printf("  PUPD_ENABLE_0  (0x46) = 0x%02X\n", pupdEn0);
        Serial.printf("  PUPD_SELECT_0  (0x48) = 0x%02X\n", pupdSel0);

        Serial.println();

        // Per-channel LED state
        Serial.println("LED Channel State (Port 0, active-low):");
        for (int ch = 0; ch < 8; ++ch) {
            bool pinLow = !(output0 & (1 << ch));
            Serial.printf("  CH%d (P0_%d): %s\n", ch, ch, pinLow ? "ON" : "OFF");
        }

        Serial.println("────────────────────────────────────────");
        Serial.println();
    }

    delay(10);   // yield to RTOS — 10 ms gives plenty of headroom
}
