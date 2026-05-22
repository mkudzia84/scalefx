# 1 "C:\\Users\\marti\\AppData\\Local\\Temp\\tmp8oxvopsw"
#include <Arduino.h>
# 1 "C:/data/code/scalefx/tests/hw/hubfx_i2c_scan/src/hubfx_i2c_scan.ino"
# 41 "C:/data/code/scalefx/tests/hw/hubfx_i2c_scan/src/hubfx_i2c_scan.ino"
#include <Arduino.h>
#include <Wire.h>





static constexpr int PIN_I2C_SDA = 8;
static constexpr int PIN_I2C_SCL = 9;
static constexpr uint32_t I2C_CLOCK_HZ = 100000;

static constexpr int PIN_HEARTBEAT_LED = 48;


static constexpr uint32_t SCAN_INTERVAL_MS = 2000;


struct ExpectedDevice {
    uint8_t addr;
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
static bool i2cProbe(uint8_t addr);
static uint16_t readReg16(uint8_t addr, uint8_t reg, bool& ok);
static uint8_t readReg8(uint8_t addr, uint8_t reg, bool& ok);
static bool isInaSlot(uint8_t addr);
static void inspectAddr(uint8_t addr, const char* role);
static void busSwrst();
void setup();
void loop();
#line 82 "C:/data/code/scalefx/tests/hw/hubfx_i2c_scan/src/hubfx_i2c_scan.ino"
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


static constexpr uint16_t INA_MFG_TI = 0x5449;
static constexpr uint16_t INA_DIE_INA226 = 0x2260;


static bool isInaSlot(uint8_t addr) {
    return (addr >= 0x40 && addr <= 0x45) || addr == 0x4A || addr == 0x4F;
}
# 143 "C:/data/code/scalefx/tests/hw/hubfx_i2c_scan/src/hubfx_i2c_scan.ino"
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


        bool ok;
        const uint8_t mode1 = readReg8(addr, 0x00, ok);
        Serial.printf("  MODE1=0x%02X (POR 0x11, driver-init 0x21)\n",
                      ok ? mode1 : 0xFF);
        return;
    }


    Serial.println("  (no fingerprint — ACK only)");
}





static void busSwrst() {
    Wire.beginTransmission((uint8_t)0x00);
    Wire.write((uint8_t)0x06);
    Wire.endTransmission();
    delay(2);
}





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




    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ);




    Serial.println("[I2C] Bus recovery — broadcast SWRST (gen-call 0x00 / 0x06)");
    busSwrst();

    Serial.println("[I2C] Driver ready. First scan in 2 s.");
    Serial.println();
}





void loop() {
    static uint32_t lastScan_ms = 0;
    static uint32_t lastHeartbeat_ms = 0;
    static uint32_t scanCount = 0;

    const uint32_t now = millis();


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

    int foundCount = 0;
    bool foundExpected[EXPECTED_COUNT] = { false };
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






    Serial.println("  per-device fingerprint (raw, no driver):");
    for (size_t i = 0; i < EXPECTED_COUNT; i++) {
        if (!foundExpected[i]) continue;
        inspectAddr(EXPECTED[i].addr, EXPECTED[i].role);
    }
    Serial.println();
}