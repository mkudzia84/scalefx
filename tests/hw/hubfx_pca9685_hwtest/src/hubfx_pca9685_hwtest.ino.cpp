# 1 "C:\\Users\\marti\\AppData\\Local\\Temp\\tmptffijsjj"
#include <Arduino.h>
# 1 "C:/data/code/scalefx/tests/hw/hubfx_pca9685_hwtest/src/hubfx_pca9685_hwtest.ino"
# 30 "C:/data/code/scalefx/tests/hw/hubfx_pca9685_hwtest/src/hubfx_pca9685_hwtest.ino"
#include <Arduino.h>
#include <Wire.h>





static constexpr int PIN_I2C_SDA = 8;
static constexpr int PIN_I2C_SCL = 9;
static constexpr uint32_t I2C_CLOCK_HZ = 100000;

static constexpr int PIN_HEARTBEAT_LED = 48;

static constexpr uint8_t PCA9685_ADDR_EXPECTED = 0x70;






static constexpr uint8_t I2C_GENERAL_CALL_ADDR = 0x00;
static constexpr uint8_t PCA9685_SWRST_BYTE = 0x06;

static constexpr uint32_t TICK_INTERVAL_MS = 2000;


static constexpr uint8_t REG_MODE1 = 0x00;
static constexpr uint8_t REG_MODE2 = 0x01;
static constexpr uint8_t REG_LED0_ON_L = 0x06;
static constexpr uint8_t REG_ALL_LED_ON_L = 0xFA;
static constexpr uint8_t REG_PRESCALE = 0xFE;


static constexpr uint8_t MODE1_RESTART = 0x80;
static constexpr uint8_t MODE1_AI = 0x20;
static constexpr uint8_t MODE1_SLEEP = 0x10;
static constexpr uint8_t MODE1_ALLCALL = 0x01;


static constexpr uint8_t MODE2_INVRT = 0x10;
static constexpr uint8_t MODE2_OUTDRV = 0x04;


static constexpr uint8_t LED_FULL = 0x10;


static constexpr uint8_t POR_MODE1 = 0x11;
static constexpr uint8_t POR_MODE2 = 0x04;
static constexpr uint8_t POR_PRESCALE = 0x1E;
# 88 "C:/data/code/scalefx/tests/hw/hubfx_pca9685_hwtest/src/hubfx_pca9685_hwtest.ino"
static constexpr uint16_t PWM_FREQUENCY_HZ = 1526;
static constexpr uint8_t PRESCALE_VALUE = 0x03;


static constexpr uint16_t DUTY_MAX = 4095;






static constexpr uint32_t OFF_DURATION_MS = 10000;
static constexpr uint32_t ON_DURATION_MS = 5000;
static constexpr uint16_t ON_DUTY = DUTY_MAX;
static constexpr uint16_t OFF_DUTY = 0;



static constexpr uint32_t PING_INTERVAL_MS = 2000;
# 115 "C:/data/code/scalefx/tests/hw/hubfx_pca9685_hwtest/src/hubfx_pca9685_hwtest.ino"
enum class Phase : uint8_t { OFF, ON };

static bool cycleActive = false;
static Phase cyclePhase = Phase::OFF;
static uint32_t phaseStartMs = 0;
static uint32_t cycleNacks = 0;
static uint32_t cycleCount = 0;


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
static uint32_t phaseDuration(Phase p);
static uint16_t phaseDuty(Phase p);
static void busHealthCheck();
static void busRecovery();
static int scanBus(bool foundExpected[EXPECTED_COUNT], const char* label);
static bool writeAllChannels(uint16_t duty);
static bool sendSWRST();
static bool readReg(uint8_t reg, uint8_t& out);
static bool writeRegVerify(const char* step, const char* name,
                           uint8_t reg, uint8_t value);
static bool readRegExpect(const char* step, const char* name,
                          uint8_t reg, uint8_t expected);
static bool writeLED4Verify(const char* step, uint8_t firstReg,
                            uint8_t a, uint8_t b, uint8_t c, uint8_t d);
static bool verifyAllChannelsDuty(const char* step, uint16_t expectedDuty);
static void runVerification();
void setup();
static void enterPhase(Phase p, uint32_t now);
void loop();
#line 146 "C:/data/code/scalefx/tests/hw/hubfx_pca9685_hwtest/src/hubfx_pca9685_hwtest.ino"
static bool i2cProbe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}




static const char* phaseName(Phase p) {
    return (p == Phase::OFF) ? "OFF" : "ON ";
}
static uint32_t phaseDuration(Phase p) {
    return (p == Phase::OFF) ? OFF_DURATION_MS : ON_DURATION_MS;
}
static uint16_t phaseDuty(Phase p) {
    return (p == Phase::OFF) ? OFF_DUTY : ON_DUTY;
}

static const char* expectedRoleFor(uint8_t addr) {
    for (size_t i = 0; i < EXPECTED_COUNT; i++) {
        if (EXPECTED[i].addr == addr) return EXPECTED[i].role;
    }
    return nullptr;
}
# 180 "C:/data/code/scalefx/tests/hw/hubfx_pca9685_hwtest/src/hubfx_pca9685_hwtest.ino"
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
            break;
        }
    }


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
# 235 "C:/data/code/scalefx/tests/hw/hubfx_pca9685_hwtest/src/hubfx_pca9685_hwtest.ino"
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
# 267 "C:/data/code/scalefx/tests/hw/hubfx_pca9685_hwtest/src/hubfx_pca9685_hwtest.ino"
static bool writeAllChannels(uint16_t duty) {
    if (duty < 1) duty = 1;
    if (duty > DUTY_MAX) duty = DUTY_MAX;

    const uint8_t on_l = 0x00;
    const uint8_t on_h = 0x00;
    const uint8_t off_l = (uint8_t)(duty & 0xFF);
    const uint8_t off_h = (uint8_t)((duty >> 8) & 0x0F);

    Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
    Wire.write(REG_ALL_LED_ON_L);
    Wire.write(on_l); Wire.write(on_h); Wire.write(off_l); Wire.write(off_h);
    return (Wire.endTransmission() == 0);
}







static bool sendSWRST() {
    Wire.beginTransmission(I2C_GENERAL_CALL_ADDR);
    Wire.write(PCA9685_SWRST_BYTE);
    const uint8_t err = Wire.endTransmission();
    Serial.printf("  SWRST: write 0x%02X to 7-bit addr 0x%02X → %s (err=%u)\n",
                  PCA9685_SWRST_BYTE, I2C_GENERAL_CALL_ADDR,
                  (err == 0) ? "ACK" : "NACK", err);
    return (err == 0);
}
# 305 "C:/data/code/scalefx/tests/hw/hubfx_pca9685_hwtest/src/hubfx_pca9685_hwtest.ino"
static bool readReg(uint8_t reg, uint8_t& out) {
    Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(PCA9685_ADDR_EXPECTED, (uint8_t)1) != 1) return false;
    out = Wire.read();
    return true;
}


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



static void runVerification() {
    Serial.println();
    Serial.println("──────────  BOOT-TIME CHIP VERIFICATION  ──────────");

    int passes = 0, fails = 0;
    auto tally = [&](bool ok) { if (ok) passes++; else fails++; };


    Serial.println("[VERIFY] 1/10  inherited state (before SWRST):");
    uint8_t m1 = 0xFF, m2 = 0xFF, pre = 0xFF;
    tally(readReg(REG_MODE1, m1)); Serial.printf("[VERIFY]      MODE1     = 0x%02X\n", m1);
    tally(readReg(REG_MODE2, m2)); Serial.printf("[VERIFY]      MODE2     = 0x%02X\n", m2);
    tally(readReg(REG_PRESCALE, pre));Serial.printf("[VERIFY]      PRESCALE  = 0x%02X\n", pre);


    Serial.println("[VERIFY] 2/10  SWRST broadcast");
    Wire.beginTransmission(I2C_GENERAL_CALL_ADDR);
    Wire.write(PCA9685_SWRST_BYTE);
    const bool swrstAck = (Wire.endTransmission() == 0);
    Serial.printf("[VERIFY]      → %s\n", swrstAck ? "ACK ✓" : "NACK ✗");
    tally(swrstAck);
    delay(2);


    Serial.println("[VERIFY] 3/10  POR defaults after SWRST:");
    tally(readRegExpect("3/10", "MODE1", REG_MODE1, POR_MODE1));
    tally(readRegExpect("3/10", "MODE2", REG_MODE2, POR_MODE2));
    tally(readRegExpect("3/10", "PRESCALE", REG_PRESCALE, POR_PRESCALE));



    Serial.println("[VERIFY] 4/10  MODE2 write/readback (INVRT toggle)");
    tally(writeRegVerify("4/10", "MODE2", REG_MODE2, POR_MODE2 | MODE2_INVRT));
    tally(writeRegVerify("4/10", "MODE2", REG_MODE2, POR_MODE2));



    Serial.println("[VERIFY] 5/10  MODE1 SLEEP entry");
    tally(writeRegVerify("5/10", "MODE1", REG_MODE1, MODE1_SLEEP | MODE1_ALLCALL));





    Serial.printf("[VERIFY] 6/10  PRESCALE write (%u Hz target, value 0x%02X)\n",
                  PWM_FREQUENCY_HZ, PRESCALE_VALUE);
    tally(writeRegVerify("6/10", "PRESCALE", REG_PRESCALE, PRESCALE_VALUE));



    Serial.println("[VERIFY] 7/10  MODE1 enable AI (still in SLEEP)");
    tally(writeRegVerify("7/10", "MODE1", REG_MODE1, MODE1_AI | MODE1_SLEEP | MODE1_ALLCALL));


    Serial.println("[VERIFY] 8/10  LED0 burst write, sentinel duty 0x4D2");
    const uint16_t sentinel1 = 0x4D2;
    tally(writeLED4Verify("8/10", REG_LED0_ON_L,
                          0x00, 0x00,
                          (uint8_t)(sentinel1 & 0xFF),
                          (uint8_t)((sentinel1 >> 8) & 0x0F)));



    Serial.println("[VERIFY] 9/10  ALL_LED broadcast, sentinel duty 0x800");
    const uint16_t sentinel2 = 0x800;
    Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
    Wire.write(REG_ALL_LED_ON_L);
    Wire.write(0x00);
    Wire.write(0x00);
    Wire.write((uint8_t)(sentinel2 & 0xFF));
    Wire.write((uint8_t)((sentinel2 >> 8) & 0x0F));
    const bool bcastOk = (Wire.endTransmission() == 0);
    Serial.printf("[VERIFY] 9/10      broadcast write %s\n", bcastOk ? "ACK ✓" : "NACK ✗");
    tally(bcastOk);
    tally(verifyAllChannelsDuty("9/10", sentinel2));






    Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
    Wire.write(REG_ALL_LED_ON_L);
    Wire.write(0x00); Wire.write(0x00);
    Wire.write(0x00); Wire.write(0x00);
    Wire.endTransmission();






    Serial.println("[VERIFY] 10/10 wake/sleep round-trip (visual demo runs in loop())");
    const uint8_t wakeMode1 = MODE1_AI | MODE1_ALLCALL;
    const uint8_t sleepMode1 = MODE1_AI | MODE1_SLEEP | MODE1_ALLCALL;
    tally(writeRegVerify("10/10", "MODE1 wake", REG_MODE1, wakeMode1));
    delayMicroseconds(500);
    tally(writeRegVerify("10/10", "MODE1 sleep", REG_MODE1, sleepMode1));


    Serial.println("──────────  VERIFICATION SUMMARY  ──────────");
    Serial.printf("  Passed: %d   Failed: %d   Chip left in SLEEP, all outputs OFF.\n",
                  passes, fails);
    Serial.println("─────────────────────────────────────────────");
    Serial.println();
}





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
# 560 "C:/data/code/scalefx/tests/hw/hubfx_pca9685_hwtest/src/hubfx_pca9685_hwtest.ino"
static void enterPhase(Phase p, uint32_t now) {
    cyclePhase = p;
    phaseStartMs = now;
    const uint16_t duty = phaseDuty(p);
    if (!writeAllChannels(duty)) cycleNacks++;
    Serial.printf("[Cycle %lu] -> %s  duty=%u/%u  for %lu ms\n",
                  (unsigned long)cycleCount, phaseName(p), duty, DUTY_MAX,
                  (unsigned long)phaseDuration(p));
}

void loop() {
    static uint32_t lastHeartbeat_ms = 0;
    static uint32_t lastPing_ms = 0;
    static bool waitWarned = false;

    const uint32_t now = millis();


    if (now - lastHeartbeat_ms >= 500) {
        lastHeartbeat_ms = now;
        digitalWrite(PIN_HEARTBEAT_LED, !digitalRead(PIN_HEARTBEAT_LED));
    }


    if (!cycleActive) {
        if (!i2cProbe(PCA9685_ADDR_EXPECTED)) {
            if (!waitWarned) {
                Serial.println("[Main] PCA9685 not on bus — cycle paused. Will retry every 2 s.");
                waitWarned = true;
            }
            if (now - lastPing_ms >= PING_INTERVAL_MS) {
                lastPing_ms = now;
                Serial.println("[Main]   …still waiting for PCA9685 @ 0x70.");
            }
            delay(50);
            return;
        }


        const uint8_t wakeMode1 = MODE1_AI | MODE1_ALLCALL;
        Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
        Wire.write(REG_MODE1);
        Wire.write(wakeMode1);
        Wire.endTransmission();
        delayMicroseconds(500);
        Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
        Wire.write(REG_MODE1);
        Wire.write(wakeMode1 | MODE1_RESTART);
        Wire.endTransmission();
        delay(1);

        Serial.println();
        Serial.println("─────────  ON/OFF CYCLE  ─────────");
        Serial.printf("  OFF window = %lu ms,  ON window = %lu ms,  PWM = ~%u Hz\n",
                      (unsigned long)OFF_DURATION_MS,
                      (unsigned long)ON_DURATION_MS,
                      PWM_FREQUENCY_HZ);
        Serial.printf("  ON duty = %u/%u (full brightness)\n", ON_DUTY, DUTY_MAX);
        Serial.println("  All 8 rails updated atomically via ALL_LED broadcast.");
        Serial.println("──────────────────────────────────");
        Serial.println();

        cycleActive = true;
        cycleCount = 1;
        enterPhase(Phase::OFF, now);
        lastPing_ms = now;
    }


    if (now - phaseStartMs >= phaseDuration(cyclePhase)) {
        if (cyclePhase == Phase::OFF) {
            enterPhase(Phase::ON, now);
        } else {
            cycleCount++;
            enterPhase(Phase::OFF, now);
        }
    }



    if (now - lastPing_ms >= PING_INTERVAL_MS) {
        lastPing_ms = now;
        const uint32_t elapsed = now - phaseStartMs;
        const uint32_t remain = phaseDuration(cyclePhase) -
                                 (elapsed <= phaseDuration(cyclePhase) ? elapsed : phaseDuration(cyclePhase));
        Serial.printf("[Cycle %lu] %s phase: %.1fs in, %.1fs left   NACKs=%lu\n",
                      (unsigned long)cycleCount, phaseName(cyclePhase),
                      elapsed / 1000.0f, remain / 1000.0f,
                      (unsigned long)cycleNacks);

        if (!i2cProbe(PCA9685_ADDR_EXPECTED)) {
            cycleNacks++;
            Serial.println("[WARN] PCA9685 stopped ACKing — running SWRST recovery.");
            sendSWRST();
            delay(2);


            Wire.beginTransmission(PCA9685_ADDR_EXPECTED);
            Wire.write(REG_PRESCALE);
            Wire.write(PRESCALE_VALUE);
            Wire.endTransmission();
            cycleActive = false;
        }
    }
}