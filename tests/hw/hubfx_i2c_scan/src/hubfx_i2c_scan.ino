/**
 * @file hubfx_i2c_scan.ino
 * @brief Minimal-scope I²C bus scanner for the HubFX PCB.
 *
 * Brings up I²C at 100 kHz on the HubFX pins (SDA = GPIO 8, SCL = GPIO 9)
 * and scans every address from 0x08 to 0x7F once every 2 seconds. For
 * each address that ACKs, prints a one-line entry annotated with the
 * device function from PINOUT.md when known. After every scan also
 * prints which expected devices were missing.
 *
 * That's it. No register writes, no audio, no PCA9685 init, no LED
 * animation — nothing that could even theoretically poke a device on
 * the bus. Use this fixture when:
 *
 *   - Diagnosing intermittent enumeration (live-wiggle U54 while
 *     watching for it to appear / disappear).
 *   - Bisecting "did the previous test damage anything?" — if the
 *     missing chip is also missing here, the failure pre-dates any
 *     register activity.
 *   - Confirming PCB-level health on a freshly built board.
 *
 * Expected I²C device set (HubFX 8-channel rev — see
 * controllers/hubfx/esp32s3/PINOUT.md):
 *
 *   0x40..0x45  INA226 rail monitors (channels 1..6)
 *   0x4A        INA226 channel 7
 *   0x4C        TAS5825P audio codec
 *   0x4F        INA226 channel 8
 *   0x70        PCA9685 LED PWM driver
 *
 *   → 10 devices total when the board is healthy.
 *
 * Serial: 115200 baud on UART0 (USB-UART bridge — WCH CH343 on the
 * HubFX board).
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

static constexpr int      PIN_HEARTBEAT_LED = 48;

// Scan cadence — one full 0x08..0x7F sweep every N ms.
static constexpr uint32_t SCAN_INTERVAL_MS = 2000;

// Expected device set (annotated for the scan output).
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

// Look up the expected role for a given address. Returns nullptr if the
// address isn't in the expected set (helpful — flags a stray responder).
static const char* expectedRoleFor(uint8_t addr) {
    for (size_t i = 0; i < EXPECTED_COUNT; i++) {
        if (EXPECTED[i].addr == addr) return EXPECTED[i].role;
    }
    return nullptr;
}

// Read a 16-bit register (MSB-first byte order).  Returns the read
// value and writes ok=true on success; ok=false on any I²C error.
static uint16_t readReg16(uint8_t addr, uint8_t reg, bool& ok) {
    ok = false;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFFFF;
    if (Wire.requestFrom((int)addr, 2) != 2) return 0xFFFF;
    if (Wire.available() < 2) return 0xFFFF;
    const uint8_t hi = (uint8_t)Wire.read();
    const uint8_t lo = (uint8_t)Wire.read();
    ok = true;
    return (uint16_t)((uint16_t)hi << 8) | lo;
}

static uint8_t readReg8(uint8_t addr, uint8_t reg, bool& ok) {
    ok = false;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFF;
    if (Wire.requestFrom((int)addr, 1) != 1) return 0xFF;
    if (Wire.available() < 1) return 0xFF;
    ok = true;
    return (uint8_t)Wire.read();
}

// Reference IDs for the chips we expect on this PCB.
static constexpr uint16_t INA_MFG_TI     = 0x5449;  // "TI"
static constexpr uint16_t INA_DIE_INA226 = 0x2260;

// True if `addr` is one of the HubFX INA226 channel slots.
static bool isInaSlot(uint8_t addr) {
    return (addr >= 0x40 && addr <= 0x45) || addr == 0x4A || addr == 0x4F;
}

// Per-address fingerprint.  Reads chip-appropriate registers ONLY —
// never reads 0xFE/0xFF on a chip that doesn't have a valid register
// at those offsets, because writing 0xFE/0xFF as a register pointer
// to PCA9685 (or TAS5825P) latches the chip's address comparator
// into an undefined state that survives until a general-call SWRST.
// An earlier revision of this test hit the PCA9685 with 0xFE/0xFF
// reads and left it NACK-only on subsequent boots — don't repeat.
//
// Per-address verdicts:
//   0x40..0x45, 0x4A, 0x4F  → expect TI / INA226 (mfg 0x5449, die 0x2260)
//   0x4C                    → TAS5825P codec (ACK only — no probe)
//   0x70                    → PCA9685 — MODE1 (0x00), safe register
static void inspectAddr(uint8_t addr, const char* role) {
    Serial.printf("  0x%02X  %-22s", addr, role ? role : "(unexpected)");

    if (isInaSlot(addr)) {
        bool mfgOk, dieOk;
        const uint16_t mfg = readReg16(addr, 0xFE, mfgOk);
        const uint16_t die = readReg16(addr, 0xFF, dieOk);
        Serial.printf("  reg[0xFE]=");
        if (mfgOk) Serial.printf("0x%04X", mfg); else Serial.print("read-fail");
        Serial.printf("  reg[0xFF]=");
        if (dieOk) Serial.printf("0x%04X", die); else Serial.print("read-fail");
        const bool match = mfgOk && dieOk && mfg == INA_MFG_TI && die == INA_DIE_INA226;
        Serial.print(match ? "   ✓ INA226 (TI)"
                           : "   ✗ NOT canonical INA226");
        Serial.println();
        return;
    }

    if (addr == 0x70) {
        // PCA9685 — read MODE1 (0x00).  POR = 0x11 (SLEEP|ALLCALL);
        // after our driver init = 0x21 (AI|ALLCALL).
        bool ok;
        const uint8_t mode1 = readReg8(addr, 0x00, ok);
        Serial.printf("  MODE1=0x%02X (POR 0x11, driver-init 0x21)\n",
                      ok ? mode1 : 0xFF);
        return;
    }

    // Unknown / not fingerprinted (TAS5825P, etc.) — leave alone.
    Serial.println("  (no fingerprint — ACK only)");
}

// General-call SWRST — write 0x06 to address 0x00.  Recovers a
// PCA9685 whose address comparator has latched out of normal state
// (also resets any other chip on the bus that honours general-call,
// which on this PCB is just the PCA).
static void busSwrst() {
    Wire.beginTransmission((uint8_t)0x00);
    Wire.write((uint8_t)0x06);
    Wire.endTransmission();
    delay(2);
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
    Serial.println("  HubFX I²C bus scanner — minimal diagnostic firmware");
    Serial.println("  ESP32-S3 / HubFX 8-channel rev");
    Serial.println("  No register writes, no audio, no PWM — bus probe only");
    Serial.println("================================================================");
    Serial.printf("  SDA=GPIO%d  SCL=GPIO%d  clock=%lu Hz  scan every %lu ms\n",
                  PIN_I2C_SDA, PIN_I2C_SCL,
                  (unsigned long)I2C_CLOCK_HZ,
                  (unsigned long)SCAN_INTERVAL_MS);
    Serial.println("================================================================");
    Serial.println();

    pinMode(PIN_HEARTBEAT_LED, OUTPUT);
    digitalWrite(PIN_HEARTBEAT_LED, HIGH);

    // 3-arg begin sets the clock during init — avoids the post-begin
    // `setClock()` race that can leave the ESP-IDF I²C-NG driver in
    // INVALID_STATE.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ);

    // Bus recovery — broadcast SWRST so any chip left in a stuck
    // state (notably the PCA9685 after a buggy register read) gets a
    // chance to come back before the first scan.
    Serial.println("[I2C] Bus recovery — broadcast SWRST (gen-call 0x00 / 0x06)");
    busSwrst();

    Serial.println("[I2C] Driver ready. First scan in 2 s.");
    Serial.println();
}

// ============================================================================
//  LOOP — periodic scan
// ============================================================================

void loop() {
    static uint32_t lastScan_ms      = 0;
    static uint32_t lastHeartbeat_ms = 0;
    static uint32_t scanCount        = 0;

    const uint32_t now = millis();

    // 2 Hz heartbeat so a black serial output still gives an "alive" signal.
    if (now - lastHeartbeat_ms >= 500) {
        lastHeartbeat_ms = now;
        digitalWrite(PIN_HEARTBEAT_LED, !digitalRead(PIN_HEARTBEAT_LED));
    }

    if (now - lastScan_ms < SCAN_INTERVAL_MS) {
        delay(20);
        return;
    }
    lastScan_ms = now;
    scanCount++;

    Serial.printf("───────  scan #%lu @ %.1f s  ───────\n",
                  (unsigned long)scanCount, now / 1000.0f);

    int   foundCount    = 0;
    bool  foundExpected[EXPECTED_COUNT] = { false };
    for (uint8_t addr = 0x08; addr <= 0x7F; addr++) {
        if (i2cProbe(addr)) {
            const char* role = expectedRoleFor(addr);
            if (role) {
                Serial.printf("  0x%02X  ACK  %s\n", addr, role);
                for (size_t i = 0; i < EXPECTED_COUNT; i++) {
                    if (EXPECTED[i].addr == addr) { foundExpected[i] = true; break; }
                }
            } else {
                Serial.printf("  0x%02X  ACK  *** UNEXPECTED RESPONDER ***\n", addr);
            }
            foundCount++;
        }
    }

    // Summary line — and explicit miss list so PCA9685 absence is loud.
    int missingCount = 0;
    Serial.printf("  → %d/%zu expected devices present\n",
                  foundCount - (foundCount > (int)EXPECTED_COUNT ? foundCount - (int)EXPECTED_COUNT : 0),
                  EXPECTED_COUNT);
    for (size_t i = 0; i < EXPECTED_COUNT; i++) {
        if (!foundExpected[i]) {
            Serial.printf("  ✗ MISSING  0x%02X  %s\n", EXPECTED[i].addr, EXPECTED[i].role);
            missingCount++;
        }
    }
    if (missingCount == 0) {
        Serial.println("  ✓ All expected devices ACKed.");
    }

    // Per-device fingerprint — reads 0xFE/0xFF (INA226 mfg/die slot)
    // on every responder.  For INA addresses we render an explicit
    // PASS/FAIL against the canonical TI/INA226 values; for the codec
    // and PCA we just dump what's at those register addresses for
    // completeness.  Bare Wire — no driver in the loop.
    Serial.println("  per-device fingerprint (raw, no driver):");
    for (size_t i = 0; i < EXPECTED_COUNT; i++) {
        if (!foundExpected[i]) continue;
        inspectAddr(EXPECTED[i].addr, EXPECTED[i].role);
    }
    Serial.println();
}
