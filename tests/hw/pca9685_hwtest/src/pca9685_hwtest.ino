/**
 * PCA9685 Hardware Test
 *
 * Bare-metal test firmware for the NXP PCA9685 16-channel 12-bit I²C
 * PWM controller. Drives all 16 channels with hardware PWM at a
 * user-configurable frequency (24 Hz – 1526 Hz, default 1000 Hz),
 * push-pull voltage output — no pull-up trick, no software PWM.
 *
 * Hardware
 * --------
 *   I²C bus:  SDA = GPIO 8, SCL = GPIO 9, 400 kHz
 *   Chip:     PCA9685 @ 0x40 (default, all address straps grounded;
 *             range 0x40..0x7F via 6 strap pins)
 *   Outputs:  PWM0..PWM15, push-pull totem-pole, ±25 mA per pin
 *   OE/:      tie LOW for outputs always enabled
 *   V+:       PWM output drive rail (typically VDD = 3.3 V)
 *
 * The PCA9685 is what you use when an I²C expander needs to drive a
 * MOSFET gate / LED / servo directly: each pin is a real push-pull
 * voltage output (HIGH = V+, LOW = GND), modulated in hardware by an
 * independent 12-bit comparator at the chip's PWM frequency.
 *
 * Datasheet (NXP PCA9685 Rev 4, 2015) sections referenced below:
 *   §7.2   Hardware PWM — 12-bit (4096 ticks/cycle), shared PRESCALE
 *   §7.3   PWM frequency — prescale = round(25MHz / (4096 × f_Hz)) − 1
 *   §7.3.3 LEDn_ON / LEDn_OFF registers (4 bytes per channel; bit-12 of
 *          each register is the FULL_ON / FULL_OFF override)
 *   §7.3.1 ALL_LED_ON / ALL_LED_OFF — broadcast write to all 16
 *   §7.3.2 MODE1 — RESTART, AI (auto-increment), SLEEP, ALLCALL
 *   §7.3.3 MODE2 — INVRT, OCH, OUTDRV, OUTNE
 *
 * Polarity (no pull-up trick required):
 *
 *   user duty = 0      → LEDn_FULL_OFF set            → PWMn = LOW   (steady)
 *   user duty = 4095   → LEDn_FULL_ON  set            → PWMn = HIGH  (steady)
 *   user duty = D      → LEDn_ON=0, LEDn_OFF=D        → PWMn HIGH for D/4096 of cycle
 *
 * Serial commands (line-based — type and press Enter)
 *
 *   Brightness (12-bit, 0..4095):
 *     <0..4095>      set ALL 16 channels  (e.g. "2048" for 50 %)
 *     <ch>=<value>   set one channel       (ch = 1..16, value = 0..4095)
 *
 *   Frequency:
 *     f<hz>          set PWM frequency (24..1526 Hz, e.g. "f1000")
 *
 *   Diagnostics:
 *     r   re-run boot self-test
 *     s   state snapshot (mode + freq + per-channel mirror)
 *     d   dump key registers
 *     b   I²C bus scan
 *     t   toggle [TRACE] mode (one line per I²C op)
 *     m   menu
 *
 * No external libraries — pure Arduino + Wire.
 */

#include <Arduino.h>
#include <Wire.h>
#include <ctype.h>
#include <esp_chip_info.h>
#include <esp_system.h>

// ── Pin mapping ────────────────────────────────────────────────────────

static constexpr int      PIN_I2C_SDA = 8;
static constexpr int      PIN_I2C_SCL = 9;
static constexpr uint32_t I2C_FREQ_HZ = 400000;   // Fast Mode

// ── PCA9685 I²C address + registers ────────────────────────────────────

static constexpr uint8_t PCA9685_ADDR_DEFAULT = 0x40;
static uint8_t           pca_addr             = PCA9685_ADDR_DEFAULT;

static constexpr uint8_t REG_MODE1            = 0x00;
static constexpr uint8_t REG_MODE2            = 0x01;
static constexpr uint8_t REG_SUBADR1          = 0x02;
static constexpr uint8_t REG_SUBADR2          = 0x03;
static constexpr uint8_t REG_SUBADR3          = 0x04;
static constexpr uint8_t REG_ALLCALLADR       = 0x05;
static constexpr uint8_t REG_LED0_ON_L        = 0x06;   // four bytes per channel
static constexpr uint8_t REG_ALL_LED_ON_L     = 0xFA;
static constexpr uint8_t REG_ALL_LED_ON_H     = 0xFB;
static constexpr uint8_t REG_ALL_LED_OFF_L    = 0xFC;
static constexpr uint8_t REG_ALL_LED_OFF_H    = 0xFD;
static constexpr uint8_t REG_PRESCALE         = 0xFE;
static constexpr uint8_t REG_TESTMODE         = 0xFF;

// MODE1 bits
static constexpr uint8_t MODE1_RESTART = 0x80;
static constexpr uint8_t MODE1_EXTCLK  = 0x40;
static constexpr uint8_t MODE1_AI      = 0x20;   // auto-increment
static constexpr uint8_t MODE1_SLEEP   = 0x10;   // oscillator off (1=off)
static constexpr uint8_t MODE1_ALLCALL = 0x01;

// MODE2 bits
static constexpr uint8_t MODE2_INVRT   = 0x10;
static constexpr uint8_t MODE2_OCH     = 0x08;   // 0=on STOP, 1=on ACK
static constexpr uint8_t MODE2_OUTDRV  = 0x04;   // 0=open-drain, 1=push-pull
static constexpr uint8_t MODE2_OUTNE0  = 0x01;
static constexpr uint8_t MODE2_OUTNE1  = 0x02;

// Bit 12 of LEDn_*_H acts as the FULL_ON / FULL_OFF override.
static constexpr uint8_t LED_FULL = 0x10;

// ── Channel mapping ────────────────────────────────────────────────────

static constexpr uint8_t  CHANNEL_COUNT = 16;
static constexpr uint16_t DUTY_MAX      = 4095;   // 12-bit cycle is 0..4095

static constexpr int PIN_BOARD_LED = 48;          // DevKitC-1 onboard LED

// ── Globals ────────────────────────────────────────────────────────────

static int      passCount = 0;
static int      failCount = 0;
static bool     traceI2C  = false;
static uint32_t i2cWrites = 0;
static uint32_t i2cReads  = 0;
static uint32_t i2cNacks  = 0;

// Mirror of last duty per channel (registers are read-back-able but a
// mirror is cheaper for snapshots and avoids 64 bytes of register reads).
static uint16_t dutyMirror[CHANNEL_COUNT] = {0};

// Active PWM frequency (set by configurePwmFrequency()).
static uint16_t pwmFrequency_Hz = 0;

// Line input buffer for parsing user commands.
static constexpr size_t LINE_BUF_SIZE = 32;
static char     lineBuf[LINE_BUF_SIZE];
static size_t   lineLen = 0;

// ── I²C helpers ────────────────────────────────────────────────────────

static bool i2cWriteReg(uint8_t reg, uint8_t value) {
    uint32_t t0 = micros();
    Wire.beginTransmission(pca_addr);
    Wire.write(reg);
    Wire.write(value);
    uint8_t err = Wire.endTransmission();
    bool ok = (err == 0);
    i2cWrites++;
    if (!ok) i2cNacks++;
    if (traceI2C) {
        Serial.printf("[TRACE]  %lu us  W reg=0x%02X val=0x%02X %s err=%u\n",
                      (unsigned long)(micros() - t0), reg, value,
                      ok ? "ACK" : "NACK", err);
    }
    return ok;
}

// 4-byte burst write starting at `firstReg` (auto-increment must be
// enabled in MODE1 for this to land in consecutive registers). Used for
// per-channel LEDn_ON_L / _H / OFF_L / _H so a brightness change is one
// atomic I²C transaction.
static bool i2cWrite4(uint8_t firstReg, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint32_t t0 = micros();
    Wire.beginTransmission(pca_addr);
    Wire.write(firstReg);
    Wire.write(a); Wire.write(b); Wire.write(c); Wire.write(d);
    uint8_t err = Wire.endTransmission();
    bool ok = (err == 0);
    i2cWrites++;
    if (!ok) i2cNacks++;
    if (traceI2C) {
        Serial.printf("[TRACE]  %lu us  W4 reg=0x%02X bytes=%02X %02X %02X %02X %s err=%u\n",
                      (unsigned long)(micros() - t0), firstReg, a, b, c, d,
                      ok ? "ACK" : "NACK", err);
    }
    return ok;
}

static bool i2cReadReg(uint8_t reg, uint8_t& out) {
    uint32_t t0 = micros();
    Wire.beginTransmission(pca_addr);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(false);
    if (err != 0) {
        i2cReads++; i2cNacks++;
        if (traceI2C) Serial.printf("[TRACE]  %lu us  R reg=0x%02X NACK on addr-ptr err=%u\n",
                                    (unsigned long)(micros() - t0), reg, err);
        return false;
    }
    if (Wire.requestFrom(pca_addr, (uint8_t)1) != 1) {
        i2cReads++; i2cNacks++;
        if (traceI2C) Serial.printf("[TRACE]  %lu us  R reg=0x%02X NACK on data\n",
                                    (unsigned long)(micros() - t0), reg);
        return false;
    }
    out = Wire.read();
    i2cReads++;
    if (traceI2C) Serial.printf("[TRACE]  %lu us  R reg=0x%02X val=0x%02X ACK\n",
                                (unsigned long)(micros() - t0), reg, out);
    return true;
}

static bool i2cProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// ── Per-channel API ────────────────────────────────────────────────────
//
// Duty 0 maps to LEDn_FULL_OFF (steady LOW). Duty 4095 maps to
// LEDn_FULL_ON (steady HIGH). Everything in between writes
// LEDn_ON = 0, LEDn_OFF = duty so the output is HIGH for duty/4096
// of each PWM cycle. Phase is 0 across all channels — extending this
// to staggered phases for switching-noise spreading is one extra line
// per channel (write LEDn_ON = phase_offset[ch]).

static bool setChannel(uint8_t ch, uint16_t duty) {
    if (ch >= CHANNEL_COUNT) return false;
    if (duty > DUTY_MAX) duty = DUTY_MAX;
    dutyMirror[ch] = duty;

    uint8_t firstReg = (uint8_t)(REG_LED0_ON_L + 4 * ch);
    uint8_t on_l, on_h, off_l, off_h;

    if (duty == 0) {
        // FULL_OFF — output is steady LOW, comparator never fires.
        on_l = 0; on_h = 0;
        off_l = 0; off_h = LED_FULL;
    } else if (duty >= DUTY_MAX) {
        // FULL_ON — output is steady HIGH, comparator always on.
        on_l = 0; on_h = LED_FULL;
        off_l = 0; off_h = 0;
    } else {
        on_l  = 0;
        on_h  = 0;
        off_l = (uint8_t)(duty & 0xFF);
        off_h = (uint8_t)((duty >> 8) & 0x0F);
    }
    return i2cWrite4(firstReg, on_l, on_h, off_l, off_h);
}

// ALL_LED_* broadcast — single 4-byte transaction that affects every
// channel at once. Cheaper than 16 per-channel writes when patterns are
// uniform.
static bool setAllChannels(uint16_t duty) {
    if (duty > DUTY_MAX) duty = DUTY_MAX;
    for (uint8_t ch = 0; ch < CHANNEL_COUNT; ch++) dutyMirror[ch] = duty;

    uint8_t on_l, on_h, off_l, off_h;
    if (duty == 0) {
        on_l = 0; on_h = 0;
        off_l = 0; off_h = LED_FULL;
    } else if (duty >= DUTY_MAX) {
        on_l = 0; on_h = LED_FULL;
        off_l = 0; off_h = 0;
    } else {
        on_l  = 0;
        on_h  = 0;
        off_l = (uint8_t)(duty & 0xFF);
        off_h = (uint8_t)((duty >> 8) & 0x0F);
    }
    return i2cWrite4(REG_ALL_LED_ON_L, on_l, on_h, off_l, off_h);
}

// ── Chip configuration ─────────────────────────────────────────────────

// Set PWM frequency. Datasheet §7.3 formula:
//     prescale = round(25 MHz / (4096 × f_Hz)) − 1
// Valid prescale range: 3..255 → ~24 Hz min, ~1526 Hz max.
// PRESCALE can only be written while MODE1.SLEEP = 1.
static bool configurePwmFrequency(uint16_t freq_Hz) {
    if (freq_Hz < 24)   freq_Hz = 24;
    if (freq_Hz > 1526) freq_Hz = 1526;

    uint32_t prescale = (25000000UL + 2048UL * freq_Hz) / (4096UL * freq_Hz);
    if (prescale > 0) prescale--;
    if (prescale < 3)   prescale = 3;
    if (prescale > 255) prescale = 255;

    uint8_t mode1Old = 0;
    i2cReadReg(REG_MODE1, mode1Old);
    uint8_t mode1Sleep = (uint8_t)((mode1Old & ~MODE1_RESTART) | MODE1_SLEEP);
    bool ok = true;
    ok &= i2cWriteReg(REG_MODE1,    mode1Sleep);   // sleep first
    ok &= i2cWriteReg(REG_PRESCALE, (uint8_t)prescale);
    ok &= i2cWriteReg(REG_MODE1,    (uint8_t)(mode1Old & ~MODE1_SLEEP));   // wake
    delayMicroseconds(500);                                                // §7.3.1.1 oscillator stabilisation
    ok &= i2cWriteReg(REG_MODE1,    (uint8_t)((mode1Old & ~MODE1_SLEEP) | MODE1_RESTART));

    if (ok) {
        uint32_t actual_Hz = 25000000UL / (4096UL * (prescale + 1));
        pwmFrequency_Hz = (uint16_t)actual_Hz;
        Serial.printf("[INFO]   PWM frequency → %lu Hz (prescale=%lu, requested %u Hz)\n",
                      (unsigned long)actual_Hz, (unsigned long)prescale, freq_Hz);
    }
    return ok;
}

static void configureChip() {
    // Wake from default sleep state, enable register auto-increment so
    // 4-byte channel updates land in consecutive LEDn_* registers.
    i2cWriteReg(REG_MODE1, MODE1_AI);
    delayMicroseconds(500);
    // Push-pull totem-pole output (datasheet recommends this for direct
    // LED / MOSFET-gate drive). OUTNE = 0 → outputs follow LEDn_* during
    // OE/ deassert. Polarity not inverted.
    i2cWriteReg(REG_MODE2, MODE2_OUTDRV);
    // Default frequency — overridable at runtime via `f<hz>`.
    configurePwmFrequency(1000);
    // Start with everything fully OFF.
    setAllChannels(0);
}

// ── Reporting helpers ──────────────────────────────────────────────────

static void pass(const char* fmt, ...) {
    char buf[128]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    Serial.printf("[PASS]   %s\n", buf); passCount++;
}

static void fail(const char* fmt, ...) {
    char buf[128]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    Serial.printf("[FAIL]   %s\n", buf); failCount++;
}

static void verifyEq(const char* what, uint8_t got, uint8_t expected) {
    if (got == expected) {
        Serial.printf("[VERIFY] %s = 0x%02X ✓\n", what, got); passCount++;
    } else {
        Serial.printf("[VERIFY] %s = 0x%02X ✗ (expected 0x%02X)\n",
                      what, got, expected); failCount++;
    }
}

// ── Self-test ──────────────────────────────────────────────────────────

static void selfTest() {
    Serial.println();
    Serial.println("================================================================");
    Serial.println("[INFO]   PCA9685 Hardware Self-Test");
    Serial.println("================================================================");
    passCount = 0; failCount = 0;

    Serial.println("[STEP 1/4] PCA9685 probe");
    Serial.printf("[INFO]   I²C: SDA=GPIO%d SCL=GPIO%d freq=%lu Hz\n",
                  PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    if (!i2cProbe(pca_addr)) {
        fail("PCA9685 did NOT ACK at 0x%02X — check power, OE/ pin (must be LOW), wiring",
             pca_addr);
        Serial.println("[INFO]   bus scan results:");
        bool anyDev = false;
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                Serial.printf("[INFO]     • 0x%02X responded\n", addr);
                anyDev = true;
            }
        }
        if (!anyDev) Serial.println("[INFO]     • no devices found on the bus");
        Serial.println("[DONE]   Self-test ABORTED — chip not reachable");
        return;
    }
    pass("PCA9685 ACK at 0x%02X", pca_addr);

    Serial.println("[STEP 2/4] MODE1 / MODE2 configuration");
    configureChip();
    uint8_t mode1 = 0, mode2 = 0;
    if (i2cReadReg(REG_MODE1, mode1)) {
        // After configure: AI=1, SLEEP=0, RESTART bit varies (datasheet says
        // it self-clears once the restart actually happens). Verify SLEEP is 0.
        if ((mode1 & MODE1_SLEEP) == 0) pass("MODE1.SLEEP = 0 (oscillator running, mode1=0x%02X)", mode1);
        else                            fail("MODE1.SLEEP still 1 (mode1=0x%02X)", mode1);
        if ((mode1 & MODE1_AI) != 0)    pass("MODE1.AI    = 1 (auto-increment enabled)");
        else                            fail("MODE1.AI    = 0 (auto-increment disabled)");
    } else {
        fail("MODE1 read failed");
    }
    if (i2cReadReg(REG_MODE2, mode2)) {
        if ((mode2 & MODE2_OUTDRV) != 0) pass("MODE2.OUTDRV = 1 (push-pull, mode2=0x%02X)", mode2);
        else                             fail("MODE2.OUTDRV = 0 (open-drain — wrong for MOSFET drive)");
    } else {
        fail("MODE2 read failed");
    }

    Serial.println("[STEP 3/4] PRESCALE readback (PWM frequency)");
    uint8_t prescale = 0;
    if (i2cReadReg(REG_PRESCALE, prescale)) {
        uint32_t f_Hz = 25000000UL / (4096UL * (prescale + 1));
        pass("PRESCALE = 0x%02X → %lu Hz", prescale, (unsigned long)f_Hz);
    } else {
        fail("PRESCALE read failed");
    }

    Serial.println("[STEP 4/4] Per-channel LEDn_OFF round-trip");
    int writeFails = 0, readFails = 0;
    for (uint8_t ch = 0; ch < CHANNEL_COUNT; ch++) {
        uint16_t testDuty = (uint16_t)((ch + 1) * 200);   // 200..3200, all valid
        if (testDuty > DUTY_MAX) testDuty = DUTY_MAX - 1;

        if (!setChannel(ch, testDuty)) { writeFails++; continue; }

        uint8_t off_l = 0, off_h = 0;
        bool readOk = i2cReadReg((uint8_t)(REG_LED0_ON_L + 4 * ch + 2), off_l) &&
                      i2cReadReg((uint8_t)(REG_LED0_ON_L + 4 * ch + 3), off_h);
        if (!readOk) { readFails++; continue; }
        uint16_t got = (uint16_t)((off_h & 0x0F) << 8) | off_l;
        if (got == testDuty) {
            Serial.printf("[VERIFY] CH%2u (PWM%u, reg 0x%02X..0x%02X) duty=%u/4095 ✓\n",
                          ch + 1, ch,
                          REG_LED0_ON_L + 4 * ch + 2, REG_LED0_ON_L + 4 * ch + 3,
                          got);
            passCount++;
        } else {
            Serial.printf("[VERIFY] CH%2u expected duty=%u, got %u ✗\n",
                          ch + 1, testDuty, got);
            failCount++;
        }
    }
    if (writeFails) fail("%d channel write(s) NACKed", writeFails);
    if (readFails)  fail("%d channel readback(s) failed", readFails);

    setAllChannels(0);   // park outputs LOW

    Serial.println("================================================================");
    if (failCount == 0) Serial.printf("[DONE]   Self-test PASS (%d assertions)\n", passCount);
    else                Serial.printf("[DONE]   Self-test FAIL — %d assertions failed (%d passed)\n",
                                       failCount, passCount);
    Serial.println("================================================================");
    Serial.println();
}

// ── Diagnostics ────────────────────────────────────────────────────────

static void busScan() {
    Serial.println("[INFO]   I²C bus scan 0x08..0x77");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            const char* tag = (addr >= 0x40 && addr <= 0x7F) ? "candidate PCA9685" : "?";
            if (addr == pca_addr) tag = "PCA9685 (active)";
            Serial.printf("[SCAN]   • 0x%02X — %s\n", addr, tag);
            found++;
        }
    }
    Serial.printf("[INFO]   bus scan complete — %d device(s) responded\n", found);
}

static void dumpRegisters() {
    Serial.println("[INFO]   PCA9685 register dump");
    struct { uint8_t reg; const char* name; } regs[] = {
        {REG_MODE1,        "MODE1"},
        {REG_MODE2,        "MODE2"},
        {REG_SUBADR1,      "SUBADR1"},
        {REG_SUBADR2,      "SUBADR2"},
        {REG_SUBADR3,      "SUBADR3"},
        {REG_ALLCALLADR,   "ALLCALLADR"},
        {REG_PRESCALE,     "PRESCALE"},
    };
    for (const auto& r : regs) {
        uint8_t v = 0;
        if (i2cReadReg(r.reg, v)) Serial.printf("[INFO]     0x%02X %-12s = 0x%02X\n",
                                                r.reg, r.name, v);
        else                      Serial.printf("[INFO]     0x%02X %-12s = ERR\n",
                                                r.reg, r.name);
    }
    Serial.println("[INFO]   Per-channel duty (mirror — registers are 4 bytes/ch):");
    for (uint8_t ch = 0; ch < CHANNEL_COUNT; ch++) {
        uint8_t firstReg = (uint8_t)(REG_LED0_ON_L + 4 * ch);
        uint16_t d = dutyMirror[ch];
        Serial.printf("[INFO]     CH%2u (PWM%u, regs 0x%02X..0x%02X)  duty=%4u/4095  (%2u.%01u %%)\n",
                      ch + 1, ch, firstReg, (uint8_t)(firstReg + 3),
                      d, (unsigned)(d * 100 / DUTY_MAX), (unsigned)((d * 1000 / DUTY_MAX) % 10));
    }
}

static void snapshot() {
    Serial.println("[SNAP]   ── State snapshot ────────────────────");
    Serial.printf("[SNAP]   uptime=%lu ms  trace=%s\n",
                  (unsigned long)millis(), traceI2C ? "ON" : "off");
    Serial.printf("[SNAP]   chip: PCA9685 @ 0x%02X  PWM=%u Hz  push-pull, 12-bit hardware\n",
                  pca_addr, pwmFrequency_Hz);
    Serial.printf("[SNAP]   self-test pass=%d fail=%d\n", passCount, failCount);
    Serial.printf("[SNAP]   I²C: writes=%lu reads=%lu nacks=%lu\n",
                  (unsigned long)i2cWrites, (unsigned long)i2cReads,
                  (unsigned long)i2cNacks);
    Serial.printf("[SNAP]   heap free=%lu min=%lu  cpu=%d MHz\n",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMinFreeHeap(), ESP.getCpuFreqMHz());
    dumpRegisters();
}

// ── Menu ───────────────────────────────────────────────────────────────

static void printMenu() {
    Serial.println("[INFO]   ── Commands (line-based — type, then Enter) ──");
    Serial.println("[INFO]   Brightness (12-bit, 0..4095):");
    Serial.println("[INFO]     <0..4095>     set ALL 16 channels  (e.g. \"2048\" = 50 %)");
    Serial.println("[INFO]     <ch>=<value>  set one channel       (ch=1..16, value=0..4095)");
    Serial.println("[INFO]   Frequency:");
    Serial.println("[INFO]     f<hz>         PWM frequency 24..1526 Hz (e.g. \"f1000\")");
    Serial.println("[INFO]   Diagnostics:");
    Serial.println("[INFO]     r   re-run self-test");
    Serial.println("[INFO]     s   state snapshot");
    Serial.println("[INFO]     d   dump registers");
    Serial.println("[INFO]     b   I²C bus scan");
    Serial.println("[INFO]     t   toggle [TRACE]");
    Serial.println("[INFO]     m   this menu");
}

// ── Line parser ────────────────────────────────────────────────────────

static void trimInPlace(char* s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

static bool parseUInt(const char* s, uint32_t& out) {
    if (!*s) return false;
    uint32_t v = 0;
    for (const char* p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
        v = v * 10 + (uint32_t)(*p - '0');
        if (v > 100000) return false;     // way out of range for any of our fields
    }
    out = v;
    return true;
}

static void handleSingleChar(char c) {
    switch (c) {
        case 'r': case 'R':
            setAllChannels(0);
            selfTest();
            break;
        case 's': case 'S': snapshot();      break;
        case 'd': case 'D': dumpRegisters(); break;
        case 'b':           busScan();       break;
        case 't': case 'T':
            traceI2C = !traceI2C;
            Serial.printf("[INFO]   I²C trace %s\n", traceI2C ? "ENABLED" : "disabled");
            break;
        case 'm': case 'M': case '?': printMenu(); break;
        default:
            Serial.printf("[INFO]   Unknown command '%c' — type 'm' for menu\n", c);
            break;
    }
}

static void handleLine(char* line) {
    trimInPlace(line);
    if (*line == 0) return;

    // Single letter command (no digit prefix).
    if (line[0] && !line[1] && !isdigit((unsigned char)line[0])) {
        handleSingleChar(line[0]);
        return;
    }

    // Frequency: "f<hz>".
    if (line[0] == 'f' || line[0] == 'F') {
        uint32_t hz = 0;
        if (!parseUInt(line + 1, hz)) {
            Serial.printf("[INFO]   bad frequency: 'f<hz>' (got '%s')\n", line);
            return;
        }
        if (hz < 24 || hz > 1526) {
            Serial.printf("[INFO]   frequency out of range: %lu (24..1526 Hz)\n",
                          (unsigned long)hz);
            return;
        }
        configurePwmFrequency((uint16_t)hz);
        return;
    }

    // Per-channel: "<ch>=<value>".
    char* eq = strchr(line, '=');
    if (eq) {
        *eq = 0;
        char* lhs = line;
        char* rhs = eq + 1;
        trimInPlace(lhs); trimInPlace(rhs);

        uint32_t chNum = 0, value = 0;
        if (!parseUInt(lhs, chNum) || !parseUInt(rhs, value)) {
            Serial.printf("[INFO]   bad format: '<channel>=<value>' (got '%s=%s')\n", lhs, rhs);
            return;
        }
        if (chNum < 1 || chNum > CHANNEL_COUNT) {
            Serial.printf("[INFO]   channel out of range: %lu (must be 1..%u)\n",
                          (unsigned long)chNum, CHANNEL_COUNT);
            return;
        }
        if (value > DUTY_MAX) {
            Serial.printf("[INFO]   value out of range: %lu (must be 0..%u)\n",
                          (unsigned long)value, DUTY_MAX);
            return;
        }
        uint8_t  ch  = (uint8_t)(chNum - 1);
        uint16_t d   = (uint16_t)value;
        setChannel(ch, d);
        Serial.printf("[INFO]   CH%u (PWM%u) duty=%u/4095  (%u.%u %%)\n",
                      (unsigned)chNum, (unsigned)ch, d,
                      (unsigned)(d * 100 / DUTY_MAX),
                      (unsigned)((d * 1000 / DUTY_MAX) % 10));
        return;
    }

    // Pure number → set all channels.
    uint32_t value = 0;
    if (!parseUInt(line, value)) {
        Serial.printf("[INFO]   unrecognised input '%s' — type 'm' for menu\n", line);
        return;
    }
    if (value > DUTY_MAX) {
        Serial.printf("[INFO]   value out of range: %lu (must be 0..%u)\n",
                      (unsigned long)value, DUTY_MAX);
        return;
    }
    uint16_t d = (uint16_t)value;
    setAllChannels(d);
    Serial.printf("[INFO]   ALL channels duty=%u/4095  (%u.%u %%)\n",
                  d, (unsigned)(d * 100 / DUTY_MAX),
                  (unsigned)((d * 1000 / DUTY_MAX) % 10));
}

// ── Setup / loop ───────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    uint32_t serialWait = millis();
    while (!Serial && (millis() - serialWait < 3000)) delay(10);

    Serial.println();
    Serial.println("================================================================");
    Serial.println("[INFO]   PCA9685 Hardware Test");
    Serial.println("[INFO]   16-channel 12-bit hardware PWM, push-pull voltage drive");
    Serial.println("================================================================");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    Serial.printf("[INFO]   ESP32-S3 rev %d, %d cores @ %d MHz, free heap %lu B\n",
                  chip.revision, chip.cores, ESP.getCpuFreqMHz(), ESP.getFreeHeap());

    pinMode(PIN_BOARD_LED, OUTPUT);
    digitalWrite(PIN_BOARD_LED, HIGH);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(I2C_FREQ_HZ);

    selfTest();
    printMenu();
    Serial.println();
    Serial.print("> ");
}

void loop() {
    uint32_t now = millis();

    // Heartbeat LED — confirms loop is alive without spamming serial.
    static uint32_t ledToggle = 0;
    if (now - ledToggle > 500) {
        ledToggle = now;
        digitalWrite(PIN_BOARD_LED, !digitalRead(PIN_BOARD_LED));
    }

    // Local-echo line buffer (PIO device monitor doesn't echo by
    // default, so we echo each printable char ourselves).
    while (Serial.available()) {
        int raw = Serial.read();
        if (raw < 0) break;
        char c = (char)raw;

        if (c == '\r' || c == '\n') {
            Serial.println();
            if (lineLen > 0) {
                lineBuf[lineLen] = 0;
                handleLine(lineBuf);
                lineLen = 0;
            }
            Serial.print("> ");
            continue;
        }
        if (c == 0x08 || c == 0x7F) {
            if (lineLen > 0) {
                lineLen--;
                Serial.print("\b \b");
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E) continue;
        if (lineLen + 1 < LINE_BUF_SIZE) {
            lineBuf[lineLen++] = c;
            Serial.write(c);
        }
    }
}
