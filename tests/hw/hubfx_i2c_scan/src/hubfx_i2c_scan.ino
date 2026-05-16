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
    Serial.println();
}
