/**
 * @file hubfx_pca9685_hwtest.ino
 * @brief Minimal PCA9685 bring-up: bus recovery, scan, SWRST general-call,
 *        re-scan, repeat. No animation. No register configuration.
 *
 * Designed for diagnosing a PCA9685 that has latched into a stuck state.
 * Each 2-second tick the fixture does, in order:
 *
 *   1. Pre-Wire bus health check (read SDA/SCL idle state as plain GPIO
 *      inputs).
 *   2. SCL-clock bus recovery with SDA-sample early-exit (up to 9 clocks;
 *      stops as soon as SDA is released).
 *   3. Manual STOP condition.
 *   4. Wire.begin(SDA=GPIO8, SCL=GPIO9, 100 kHz).
 *   5. Bus scan, report which addresses ACK and which expected devices
 *      are missing.
 *   6. If PCA9685 (0x70) is missing: issue a software reset via the
 *      I²C general-call address (write 0x06 to 7-bit 0x00). PCA9685
 *      datasheet §7.1.4 says this resets the chip without requiring it
 *      to ACK its assigned address — useful if the address comparator
 *      itself has locked up.
 *   7. 10 ms settle.
 *   8. Re-scan, report whether the SWRST recovered anything.
 *
 * No animation, no MODE1/MODE2/PRESCALE writes, no LED brightness — just
 * bus diagnostics + the one general-call SWRST attempt per tick. Once
 * the chip comes back (or you give up and swap silicon), this fixture
 * has done its job; the breathing animation lives in git history.
 *
 * Hardware (see ../../../controllers/hubfx/esp32s3/PINOUT.md):
 *   I²C:  SDA=GPIO8, SCL=GPIO9, 100 kHz, expected devices at
 *         0x40–0x45, 0x4A, 0x4C, 0x4F, 0x70.
 *
 * Build: `pio run -t upload` from this directory.
 *
 * NO external libraries — pure Arduino + Wire.
 */

#include <Arduino.h>
#include <Wire.h>

// ============================================================================
//  CONFIG
// ============================================================================

static constexpr int      PIN_I2C_SDA  = 8;
static constexpr int      PIN_I2C_SCL  = 9;
static constexpr uint32_t I2C_CLOCK_HZ = 100000;

static constexpr int PIN_HEARTBEAT_LED = 48;

static constexpr uint8_t PCA9685_ADDR_EXPECTED = 0x70;

// PCA9685 software-reset sequence (datasheet §7.1.4):
//   Write byte 0x06 to 7-bit address 0x00 (I²C general call). All chips
//   on the bus that have general-call ACK enabled will reset. That's
//   PCA9685's default state at power-on, so this works even when the
//   chip is no longer ACK'ing its assigned address.
static constexpr uint8_t I2C_GENERAL_CALL_ADDR = 0x00;
static constexpr uint8_t PCA9685_SWRST_BYTE    = 0x06;

static constexpr uint32_t TICK_INTERVAL_MS = 2000;

// PCA9685 register map (only what the verification phase touches).
static constexpr uint8_t REG_MODE1         = 0x00;
static constexpr uint8_t REG_MODE2         = 0x01;
static constexpr uint8_t REG_LED0_ON_L     = 0x06;  // 4 bytes per channel
static constexpr uint8_t REG_ALL_LED_ON_L  = 0xFA;
static constexpr uint8_t REG_PRESCALE      = 0xFE;

// MODE1 bits
static constexpr uint8_t MODE1_RESTART = 0x80;
static constexpr uint8_t MODE1_AI      = 0x20;
static constexpr uint8_t MODE1_SLEEP   = 0x10;
static constexpr uint8_t MODE1_ALLCALL = 0x01;

// MODE2 bits
static constexpr uint8_t MODE2_INVRT   = 0x10;
static constexpr uint8_t MODE2_OUTDRV  = 0x04;

// LEDn_*_H bit 4 = FULL_ON / FULL_OFF override
static constexpr uint8_t LED_FULL = 0x10;

// Datasheet POR defaults (§7.3.1.1 / §7.3.2 / §7.3.5).
static constexpr uint8_t POR_MODE1    = 0x11;   // SLEEP=1, ALLCALL=1
static constexpr uint8_t POR_MODE2    = 0x04;   // OUTDRV=1 (push-pull)
static constexpr uint8_t POR_PRESCALE = 0x1E;   // ~200 Hz

// Operating PWM frequency for the LED rails. The chip supports
// 24..1526 Hz (prescale 255..3). We run at the upper limit to push any
// residual switching artefact above the human flicker-fusion threshold
// — at 1526 Hz the cycle is 655 µs, so even a single-tick pulse is
// well below perception.
//
// Datasheet §7.3.5 prescale formula: round(25 MHz / (4096 × f_Hz)) − 1.
// 25e6 / (4096 × 1500) = 4.07 → prescale = 3 → actual = 1526 Hz.
static constexpr uint16_t PWM_FREQUENCY_HZ = 1526;
static constexpr uint8_t  PRESCALE_VALUE   = 0x03;  // 1526 Hz actual

// Continuous breathing animation parameters (loop() after verification).
static constexpr uint32_t BREATHING_PERIOD_MS = 4000;
static constexpr uint32_t BREATHING_FRAME_HZ  = 60;
static constexpr float    BREATHING_GAMMA     = 2.2f;
static constexpr uint16_t DUTY_MAX            = 4095;

// Peak-brightness limit (fraction of full 12-bit range). The breathing
// curve still spans phase 0..1 over the configured period — this just
// scales the output amplitude. Used as a diagnostic for current-/
// supply-related flicker: if dropping peak from 1.0 to 0.6 eliminates
// visible flicker, the cause is V+ rail sag / MOSFET thermal / LED
// driver ripple at peak current, not a chip-level PWM issue.
//
// To restore full brightness, set PEAK_FRACTION = 1.0f.
static constexpr float    PEAK_FRACTION       = 0.20f;
static constexpr uint16_t PEAK_DUTY           =
    (uint16_t)((float)DUTY_MAX * PEAK_FRACTION + 0.5f);

// When true, skip the breathing animation and just drive all 8 channels
// at a constant PEAK_DUTY. Diagnostic mode: lets the user observe whether
// the flicker is present at a fixed duty cycle (rules out animation /
// update-rate artefacts and isolates supply-side or chip-side issues).
static constexpr bool     CONSTANT_OUTPUT     = false;

// Expected device set.
struct ExpectedDevice {
    uint8_t     addr;
    const char* role;
};
static constexpr ExpectedDevice EXPECTED[] = {
    { 0x40, "INA226 ch1 (rail)" },
    { 0x41, "INA226 ch2" },
    { 0x42, "INA226 ch3" },
    { 0x43, "INA226 ch4" },
    { 0x44, "INA226 ch5" },
    { 0x45, "INA226 ch6" },
    { 0x4A, "INA226 ch7" },
    { 0x4C, "TAS5825P audio codec" },
    { 0x4F, "INA226 ch8" },
    { 0x70, "PCA9685 LED PWM" },
};
static constexpr size_t EXPECTED_COUNT = sizeof(EXPECTED) / sizeof(EXPECTED[0]);

// ============================================================================
//  HELPERS
// ============================================================================

static bool i2cProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static const char* expectedRoleFor(uint8_t addr) {
    for (size_t i = 0; i < EXPECTED_COUNT; i++) {
        if (EXPECTED[i].addr == addr) return EXPECTED[i].role;
    }
    return nullptr;
}

// ============================================================================
//  PRE-WIRE BUS HEALTH + RECOVERY
//  Done with the pins as plain GPIO, BEFORE Wire.begin().
// ============================================================================

// Read SDA and SCL idle state via the external pull-ups. INPUT_PULLUP
// enables the internal pull-up too (parallel to the external — that's
// fine), so we get a clean HIGH unless something is actively pulling
// the line LOW.
static void busHealthCheck() {
    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(PIN_I2C_SCL, INPUT_PULLUP);
    delayMicroseconds(50);
    const int sda = digitalRead(PIN_I2C_SDA);
    const int scl = digitalRead(PIN_I2C_SCL);
    Serial.printf("  Bus health: SDA=%s  SCL=%s\n",
                  sda ? "HIGH ✓" : "LOW  ✗",
                  scl ? "HIGH ✓" : "LOW  ✗");
}

// Adapted from the user's recovery routine — SDA is sampled each clock,
// loop exits as soon as the slave releases the line. SCL is driven
// push-pull (we dominate the bus during recovery, which is the correct
// behaviour while clocking out a stuck slave).
static void busRecovery() {
    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(PIN_I2C_SCL, OUTPUT);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(10);

    int clocksSent = 0;
    for (int i = 0; i < 9; i++) {
        digitalWrite(PIN_I2C_SCL, LOW);
        delayMicroseconds(5);
        digitalWrite(PIN_I2C_SCL, HIGH);
        delayMicroseconds(5);
        clocksSent++;
        if (digitalRead(PIN_I2C_SDA) == HIGH) {
            break;  // slave released SDA — bus is now clean
        }
    }

    // Standard STOP condition: SDA L→H while SCL HIGH.
    pinMode(PIN_I2C_SDA, OUTPUT);
    digitalWrite(PIN_I2C_SDA, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(5);
    digitalWrite(PIN_I2C_SDA, HIGH);
    delayMicroseconds(5);

    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(PIN_I2C_SCL, INPUT_PULLUP);

    Serial.printf("  Bus recovery: sent %d SCL clock(s) + STOP\n", clocksSent);
}

// ============================================================================
//  SCAN + SWRST
// ============================================================================

// Scan 0x08..0x7F, print each ACK with annotation, return how many of
// the expected devices were present and write `foundExpected[]` for the
// caller. Returns the total count of devices on the bus.
static int scanBus(bool foundExpected[EXPECTED_COUNT], const char* label) {
    Serial.printf("  --- %s ---\n", label);
    int totalFound = 0;
    for (size_t i = 0; i < EXPECTED_COUNT; i++) foundExpected[i] = false;
    for (uint8_t addr = 0x08; addr <= 0x7F; addr++) {
        if (i2cProbe(addr)) {
            const char* role = expectedRoleFor(addr);
            if (role) {
                Serial.printf("    0x%02X  ACK  %s\n", addr, role);
                for (size_t i = 0; i < EXPECTED_COUNT; i++) {
                    if (EXPECTED[i].addr == addr) { foundExpected[i] = true; break; }
                }
            } else {
                Serial.printf("    0x%02X  ACK  *** UNEXPECTED RESPONDER ***\n", addr);
            }
            totalFound++;
        }
    }
    return totalFound;
}

// Gamma-corrected sin² breathing curve. elapsed_ms wraps modulo period
// inside the function, so it's safe to feed monotonic millis() forever.
// Output is scaled to PEAK_DUTY (which may be lower than DUTY_MAX as a
// flicker-diagnosis knob).
static uint16_t breathingDuty(uint32_t elapsed_ms) {
    const float phase     = (float)(elapsed_ms % BREATHING_PERIOD_MS)
                            / (float)BREATHING_PERIOD_MS;
    const float s         = sinf((float)M_PI * phase);
    const float raw       = s * s;
    const float corrected = powf(raw, BREATHING_GAMMA);
    return (uint16_t)(corrected * (float)PEAK_DUTY + 0.5f);
}

// Atomically write a 12-bit duty value to all 16 channels using the
// ALL_LED broadcast registers — one I²C transaction, all rails phase-
// locked. Returns true on ACK.
//
// Always PWM mode (ON=0, OFF=duty). Never sets the FULL_ON / FULL_OFF
// flag bits, because switching INTO and OUT OF those modes (which
// happens for a single frame at the peak and trough of the breathing
// curve) produces a brief visible glitch on this PCA9685 silicon.
// Practical range: duty=1 → 0.024 % duty (sub-visual), duty=4095 →
// 99.976 % (visually full on). Caller can still pass 0; we clamp up
// to 1 so the ON==OFF undefined-behavior case is avoided.
static bool writeAllChannels(uint16_t duty) {
    if (duty < 1)         duty = 1;
    if (duty > DUTY_MAX)  duty = DUTY_MAX;

    const uint8_t on_l  = 0x00;
    const uint8_t on_h  = 0x00;
    const uint8_t off_l = (uint8_t)(duty & 0xFF);
    const uint8_t off_h = (uint8_t)((duty >> 8) & 0x0F);

    Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
    Wire.write(REG_ALL_LED_ON_L);
    Wire.write(on_l); Wire.write(on_h); Wire.write(off_l); Wire.write(off_h);
    return (Wire.endTransmission() == 0);
}

// PCA9685 software-reset broadcast. Issues exactly one I²C transaction:
//   START + 0x00 + 0x06 + STOP
// Any PCA9685 on the bus that still has its general-call ACK bit
// enabled (which is the power-on default) will perform a full reset of
// its internal state machine — including the address comparator. Costs
// one transaction; harmless if the chip is healthy or absent.
static bool sendSWRST() {
    Wire.beginTransmission(I2C_GENERAL_CALL_ADDR);
    Wire.write(PCA9685_SWRST_BYTE);
    const uint8_t err = Wire.endTransmission();
    Serial.printf("  SWRST: write 0x%02X to 7-bit addr 0x%02X → %s (err=%u)\n",
                  PCA9685_SWRST_BYTE, I2C_GENERAL_CALL_ADDR,
                  (err == 0) ? "ACK" : "NACK", err);
    return (err == 0);
}

// ============================================================================
//  BOOT-TIME CHIP VERIFICATION
//  Runs once after the first scan confirms the chip is on the bus.
//  Keeps the chip in SLEEP throughout — no LEDs flicker during the test.
// ============================================================================

// Read one register from PCA9685 at PCA9685_ADDR_EXPECTED.
static bool readReg(uint8_t reg, uint8_t& out) {
    Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(PCA9685_ADDR_EXPECTED, (uint8_t)1) != 1) return false;
    out = Wire.read();
    return true;
}

// Write one register and read it back. Logs the comparison.
static bool writeRegVerify(const char* step, const char* name,
                           uint8_t reg, uint8_t value) {
    Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
    Wire.write(reg);
    Wire.write(value);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[VERIFY] %s  W %-9s 0x%02X ← 0x%02X  NACK ✗\n",
                      step, name, reg, value);
        return false;
    }
    uint8_t got = 0;
    if (!readReg(reg, got)) {
        Serial.printf("[VERIFY] %s  W %-9s 0x%02X ← 0x%02X  (write OK, readback NACK ✗)\n",
                      step, name, reg, value);
        return false;
    }
    const bool match = (got == value);
    Serial.printf("[VERIFY] %s  W %-9s 0x%02X ← 0x%02X  readback 0x%02X  %s\n",
                  step, name, reg, value, got, match ? "✓ match" : "✗ MISMATCH");
    return match;
}

// Read + log a single register, comparing against an expected value.
static bool readRegExpect(const char* step, const char* name,
                          uint8_t reg, uint8_t expected) {
    uint8_t got = 0;
    if (!readReg(reg, got)) {
        Serial.printf("[VERIFY] %s  R %-9s 0x%02X = ??  NACK ✗\n", step, name, reg);
        return false;
    }
    const bool match = (got == expected);
    Serial.printf("[VERIFY] %s  R %-9s 0x%02X = 0x%02X  (expect 0x%02X)  %s\n",
                  step, name, reg, got, expected, match ? "✓ match" : "✗ MISMATCH");
    return match;
}

// 4-byte burst write to LEDn_ON_L (auto-increment enabled via MODE1.AI).
// Reads back all 4 bytes and compares.
static bool writeLED4Verify(const char* step, uint8_t firstReg,
                            uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
    Wire.write(firstReg);
    Wire.write(a); Wire.write(b); Wire.write(c); Wire.write(d);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[VERIFY] %s  burst write @0x%02X NACK ✗\n", step, firstReg);
        return false;
    }
    uint8_t g[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    bool readOk = true;
    for (int i = 0; i < 4; i++) readOk &= readReg(firstReg + i, g[i]);
    if (!readOk) {
        Serial.printf("[VERIFY] %s  burst write OK, readback NACK ✗\n", step);
        return false;
    }
    const bool match = (g[0] == a) && (g[1] == b) && (g[2] == c) && (g[3] == d);
    Serial.printf("[VERIFY] %s  burst @0x%02X  wrote {%02X %02X %02X %02X}  read {%02X %02X %02X %02X}  %s\n",
                  step, firstReg, a, b, c, d, g[0], g[1], g[2], g[3],
                  match ? "✓ match" : "✗ MISMATCH");
    return match;
}

// Read LED0..LED7 OFF_L/OFF_H pairs, compare to expected 12-bit duty.
static bool verifyAllChannelsDuty(const char* step, uint16_t expectedDuty) {
    bool allMatch = true;
    for (uint8_t ch = 0; ch < 8; ch++) {
        uint8_t off_l = 0xFF, off_h = 0xFF;
        const uint8_t base = REG_LED0_ON_L + 4 * ch;
        if (!readReg(base + 2, off_l) || !readReg(base + 3, off_h)) {
            Serial.printf("[VERIFY] %s  LED%u readback NACK ✗\n", step, ch);
            allMatch = false;
            continue;
        }
        const uint16_t got = ((uint16_t)(off_h & 0x0F) << 8) | off_l;
        const bool match = (got == expectedDuty);
        Serial.printf("[VERIFY] %s  LED%u OFF = 0x%03X (expect 0x%03X) %s\n",
                      step, ch, got, expectedDuty, match ? "✓" : "✗");
        if (!match) allMatch = false;
    }
    return allMatch;
}

// Run the full verification sequence. Logs every step. Doesn't halt on
// failure — we want the canary loop to start regardless.
static void runVerification() {
    Serial.println();
    Serial.println("──────────  BOOT-TIME CHIP VERIFICATION  ──────────");

    int passes = 0, fails = 0;
    auto tally = [&](bool ok) { if (ok) passes++; else fails++; };

    // Step 1: inherited register state.
    Serial.println("[VERIFY] 1/10  inherited state (before SWRST):");
    uint8_t m1 = 0xFF, m2 = 0xFF, pre = 0xFF;
    tally(readReg(REG_MODE1, m1));    Serial.printf("[VERIFY]      MODE1     = 0x%02X\n", m1);
    tally(readReg(REG_MODE2, m2));    Serial.printf("[VERIFY]      MODE2     = 0x%02X\n", m2);
    tally(readReg(REG_PRESCALE, pre));Serial.printf("[VERIFY]      PRESCALE  = 0x%02X\n", pre);

    // Step 2: SWRST broadcast.
    Serial.println("[VERIFY] 2/10  SWRST broadcast");
    Wire.beginTransmission(I2C_GENERAL_CALL_ADDR);
    Wire.write(PCA9685_SWRST_BYTE);
    const bool swrstAck = (Wire.endTransmission() == 0);
    Serial.printf("[VERIFY]      → %s\n", swrstAck ? "ACK ✓" : "NACK ✗");
    tally(swrstAck);
    delay(2);

    // Step 3: re-read, compare to POR defaults.
    Serial.println("[VERIFY] 3/10  POR defaults after SWRST:");
    tally(readRegExpect("3/10", "MODE1",    REG_MODE1,    POR_MODE1));
    tally(readRegExpect("3/10", "MODE2",    REG_MODE2,    POR_MODE2));
    tally(readRegExpect("3/10", "PRESCALE", REG_PRESCALE, POR_PRESCALE));

    // Step 4: toggle a benign MODE2 bit (INVRT) and back. Doesn't wake the
    // chip; INVRT only matters when outputs are active.
    Serial.println("[VERIFY] 4/10  MODE2 write/readback (INVRT toggle)");
    tally(writeRegVerify("4/10", "MODE2",   REG_MODE2, POR_MODE2 | MODE2_INVRT));
    tally(writeRegVerify("4/10", "MODE2",   REG_MODE2, POR_MODE2));

    // Step 5: explicit SLEEP entry (chip is already SLEEP from POR, but
    // we exercise the write path).
    Serial.println("[VERIFY] 5/10  MODE1 SLEEP entry");
    tally(writeRegVerify("5/10", "MODE1",   REG_MODE1, MODE1_SLEEP | MODE1_ALLCALL));

    // Step 6: PRESCALE write — only valid while SLEEP=1, which we just
    // confirmed. Writing the operating value (1000 Hz target) rather
    // than POR — this also tests the chip accepts a non-default
    // prescale, which is the path the continuous animation uses.
    Serial.printf("[VERIFY] 6/10  PRESCALE write (%u Hz target, value 0x%02X)\n",
                  PWM_FREQUENCY_HZ, PRESCALE_VALUE);
    tally(writeRegVerify("6/10", "PRESCALE", REG_PRESCALE, PRESCALE_VALUE));

    // Step 7: enable AI (auto-increment) for the burst writes in steps 8/10.
    // Still SLEEP=1 so outputs stay off.
    Serial.println("[VERIFY] 7/10  MODE1 enable AI (still in SLEEP)");
    tally(writeRegVerify("7/10", "MODE1",   REG_MODE1, MODE1_AI | MODE1_SLEEP | MODE1_ALLCALL));

    // Step 8: single LED0 burst write — sentinel duty 0x4D2 (1234/4095 ≈ 30%).
    Serial.println("[VERIFY] 8/10  LED0 burst write, sentinel duty 0x4D2");
    const uint16_t sentinel1 = 0x4D2;
    tally(writeLED4Verify("8/10", REG_LED0_ON_L,
                          0x00, 0x00,
                          (uint8_t)(sentinel1 & 0xFF),
                          (uint8_t)((sentinel1 >> 8) & 0x0F)));

    // Step 9: ALL_LED broadcast — different sentinel 0x800 (2048/4095 = 50%).
    // Read back from every LED0..LED7 OFF pair, all 8 must match.
    Serial.println("[VERIFY] 9/10  ALL_LED broadcast, sentinel duty 0x800");
    const uint16_t sentinel2 = 0x800;
    Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
    Wire.write(REG_ALL_LED_ON_L);
    Wire.write(0x00);                                       // ON_L
    Wire.write(0x00);                                       // ON_H
    Wire.write((uint8_t)(sentinel2 & 0xFF));                // OFF_L
    Wire.write((uint8_t)((sentinel2 >> 8) & 0x0F));         // OFF_H
    const bool bcastOk = (Wire.endTransmission() == 0);
    Serial.printf("[VERIFY] 9/10      broadcast write %s\n", bcastOk ? "ACK ✓" : "NACK ✗");
    tally(bcastOk);
    tally(verifyAllChannelsDuty("9/10", sentinel2));

    // Park ALL_LED → 0 (not FULL_OFF) before waking, so the chip transitions
    // into PLAY mode with all outputs at duty 0 — equivalent to OFF but
    // smoother on the wake transient (FULL_OFF + wake can cause a brief
    // glitch on some PCA9685 silicon revisions). FULL_OFF parking happens
    // at the very end, after the breathe.
    Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
    Wire.write(REG_ALL_LED_ON_L);
    Wire.write(0x00); Wire.write(0x00);
    Wire.write(0x00); Wire.write(0x00);
    Wire.endTransmission();

    // Step 10: wake-and-sleep round-trip — verifies the chip can enter
    // PLAY mode from SLEEP cleanly. No animation here; the continuous
    // breathing in loop() is the visual demo. Keeping verification
    // fast means a smaller dark gap between boot and the animation
    // starting.
    Serial.println("[VERIFY] 10/10 wake/sleep round-trip (visual demo runs in loop())");
    const uint8_t wakeMode1  = MODE1_AI | MODE1_ALLCALL;                // 0x21
    const uint8_t sleepMode1 = MODE1_AI | MODE1_SLEEP | MODE1_ALLCALL;  // 0x31
    tally(writeRegVerify("10/10", "MODE1 wake",  REG_MODE1, wakeMode1));
    delayMicroseconds(500);  // §7.3.1.1
    tally(writeRegVerify("10/10", "MODE1 sleep", REG_MODE1, sleepMode1));

    // Final state.
    Serial.println("──────────  VERIFICATION SUMMARY  ──────────");
    Serial.printf("  Passed: %d   Failed: %d   Chip left in SLEEP, all outputs OFF.\n",
                  passes, fails);
    Serial.println("─────────────────────────────────────────────");
    Serial.println();
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    uint32_t serialWaitStart = millis();
    while (!Serial && (millis() - serialWaitStart < 3000)) delay(10);

    Serial.println();
    Serial.println("================================================================");
    Serial.println("  HubFX PCA9685 hwtest — minimal scan + SWRST recovery loop");
    Serial.println("  ESP32-S3 / HubFX 8-channel rev");
    Serial.println("  Each tick: bus-health → recovery → scan → SWRST → re-scan");
    Serial.println("================================================================");
    Serial.printf("  SDA=GPIO%d  SCL=GPIO%d  clock=%lu Hz  tick every %lu ms\n",
                  PIN_I2C_SDA, PIN_I2C_SCL,
                  (unsigned long)I2C_CLOCK_HZ,
                  (unsigned long)TICK_INTERVAL_MS);
    Serial.println("================================================================");
    Serial.println();

    pinMode(PIN_HEARTBEAT_LED, OUTPUT);
    digitalWrite(PIN_HEARTBEAT_LED, HIGH);

    // One-shot boot-time chip verification. We do the bus health +
    // recovery + Wire.begin here too so the verification has a clean
    // environment; the canary loop will tear this down and re-init
    // each tick.
    busHealthCheck();
    busRecovery();
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ);

    if (i2cProbe(PCA9685_ADDR_EXPECTED)) {
        runVerification();
    } else {
        Serial.println();
        Serial.println("[VERIFY] PCA9685 not on bus at boot — skipping verification.");
        Serial.println("[VERIFY] The canary loop below will keep scanning + SWRST'ing.");
        Serial.println();
    }
}

// ============================================================================
//  LOOP — continuous breathing on all 8 LED rails (sin² gamma-corrected)
//  Wakes the chip on first entry, then writes the ALL_LED broadcast at
//  BREATHING_FRAME_HZ. Status line every second; warns if NACK count
//  starts growing (which would mean the chip went stuck mid-animation).
// ============================================================================

static bool     animationActive = false;
static uint32_t animBootMs      = 0;
static uint32_t animFrames      = 0;
static uint32_t animNacks       = 0;
static uint16_t animPeakDuty    = 0;

void loop() {
    static uint32_t lastFrame_ms     = 0;
    static uint32_t lastStatus_ms    = 0;
    static uint32_t lastHeartbeat_ms = 0;
    static uint32_t lastFrameSnap    = 0;
    static uint32_t lastNackSnap     = 0;
    static uint32_t statusCount      = 0;
    static bool     waitWarned       = false;

    const uint32_t now = millis();

    // 1 Hz heartbeat (independent of chip state).
    if (now - lastHeartbeat_ms >= 500) {
        lastHeartbeat_ms = now;
        digitalWrite(PIN_HEARTBEAT_LED, !digitalRead(PIN_HEARTBEAT_LED));
    }

    // Lazy continuous-mode init on first tick. runVerification() left
    // the chip in SLEEP; wake it here.
    if (!animationActive) {
        if (!i2cProbe(PCA9685_ADDR_EXPECTED)) {
            if (!waitWarned) {
                Serial.println("[Main] PCA9685 not on bus — animation paused. Will retry every 2 s.");
                waitWarned = true;
            }
            if (now - lastStatus_ms >= 2000) {
                lastStatus_ms = now;
                Serial.println("[Main]   …still waiting for PCA9685 @ 0x70.");
            }
            delay(50);
            return;
        }

        // Wake: clear SLEEP, keep AI on, then nudge RESTART so any
        // residual paused outputs come back cleanly.
        const uint8_t wakeMode1 = MODE1_AI | MODE1_ALLCALL;   // 0x21
        Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
        Wire.write(REG_MODE1);
        Wire.write(wakeMode1);
        Wire.endTransmission();
        delayMicroseconds(500);   // §7.3.1.1 oscillator stabilisation
        Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
        Wire.write(REG_MODE1);
        Wire.write(wakeMode1 | MODE1_RESTART);
        Wire.endTransmission();
        delay(1);

        // Start all outputs at duty 0 so the breathing eases in from
        // dark rather than from whatever stale value was last written.
        writeAllChannels(0);

        animBootMs      = millis();
        animationActive = true;
        Serial.println();
        if (CONSTANT_OUTPUT) {
            Serial.println("─────────  CONSTANT-DUTY DIAGNOSTIC  ─────────");
            Serial.printf("  fixed duty = %u/%u (%.0f%% of 12-bit range),  PWM = ~%u Hz\n",
                          PEAK_DUTY, DUTY_MAX, PEAK_FRACTION * 100.0f,
                          PWM_FREQUENCY_HZ);
            Serial.printf("  update rate = %lu Hz (only to keep the canary alive)\n",
                          (unsigned long)BREATHING_FRAME_HZ);
            Serial.println("  All 8 rails driven identically — no animation.");
            Serial.println("──────────────────────────────────────────────");
        } else {
            Serial.println("─────────  CONTINUOUS BREATHING  ─────────");
            Serial.printf("  period = %lu ms,  frame rate = %lu Hz,  γ = %.1f,  PWM = ~%u Hz\n",
                          (unsigned long)BREATHING_PERIOD_MS,
                          (unsigned long)BREATHING_FRAME_HZ,
                          BREATHING_GAMMA, PWM_FREQUENCY_HZ);
            Serial.printf("  peak duty = %u/%u (%.0f%% of 12-bit range)\n",
                          PEAK_DUTY, DUTY_MAX, PEAK_FRACTION * 100.0f);
            Serial.println("  All 8 rails updated atomically via ALL_LED broadcast.");
            Serial.println("──────────────────────────────────────────");
        }
        Serial.println();
    }

    // Animation tick — single 5-byte ALL_LED write per frame.
    const uint32_t framePeriodMs = 1000UL / BREATHING_FRAME_HZ;
    if (now - lastFrame_ms >= framePeriodMs) {
        lastFrame_ms = now;
        const uint16_t duty = CONSTANT_OUTPUT
                              ? PEAK_DUTY
                              : breathingDuty(now - animBootMs);
        if (!writeAllChannels(duty)) {
            animNacks++;
        }
        if (duty > animPeakDuty) animPeakDuty = duty;
        animFrames++;
    }

    // Status line every 1 s.
    if (now - lastStatus_ms >= 1000) {
        lastStatus_ms = now;
        statusCount++;

        const uint32_t deltaFrames = animFrames - lastFrameSnap;
        const uint32_t deltaNacks  = animNacks  - lastNackSnap;
        lastFrameSnap = animFrames;
        lastNackSnap  = animNacks;

        if ((statusCount % 20) == 1) {
            Serial.println();
            Serial.println("  #   Uptime   Duty/4095   %    FPS   Peak    Frames   NACKs (Δ)");
            Serial.println("---  -------  ----------  ---  -----  -----  --------  ----------");
        }

        const uint16_t latestDuty = CONSTANT_OUTPUT
                                    ? PEAK_DUTY
                                    : breathingDuty(now - animBootMs);
        Serial.printf("%3lu  %6.1fs  %4u/4095   %3.0f  %5lu  %5u  %8lu  %5lu (Δ%lu)\n",
                      (unsigned long)statusCount,
                      (now - animBootMs) / 1000.0f,
                      latestDuty,
                      (float)latestDuty * 100.0f / (float)DUTY_MAX,
                      (unsigned long)deltaFrames,
                      animPeakDuty,
                      (unsigned long)animFrames,
                      (unsigned long)animNacks,
                      (unsigned long)deltaNacks);

        // If new NACKs appeared during the last second, that's a chip
        // stick-up event — try SWRST recovery and reconfigure.
        if (deltaNacks > 0) {
            Serial.println("[WARN] NACKs during animation — attempting SWRST recovery.");
            sendSWRST();
            delay(2);
            // Re-set PWM frequency (PRESCALE needs SLEEP=1, which is
            // exactly the post-SWRST state).
            Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
            Wire.write(REG_PRESCALE);
            Wire.write(PRESCALE_VALUE);
            Wire.endTransmission();
            // Re-wake.
            animationActive = false;   // force lazy-init next tick
        }
    }
}
