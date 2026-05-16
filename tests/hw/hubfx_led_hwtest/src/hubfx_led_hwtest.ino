/**
 * HubFX Local LED Hardware Test
 *
 * Bare-metal test firmware for the AW9523B I²C GPIO/LED-driver
 * expander on the HubFX ESP32-S3 board. Drives the 6 LED channels
 * wired to P0_0..P0_5 via N-MOSFETs with gate pull-ups, using the
 * AW9523B's built-in 8-bit / 430 Hz hardware PWM.
 *
 * Hardware topology
 * -----------------
 *
 *                                    +3.3 V
 *                                      │
 *                                    10 kΩ   (gate pull-up — populated on PCB)
 *                                      │
 *      +V_LED ── LED ── N-MOSFET drain │
 *                       N-MOSFET src ──┼── GND
 *                       N-MOSFET gate ─┴── AW9523B P0_x   (LED mode, sinks)
 *
 * Polarity (datasheet Rev 1.2 §7.2 — LED mode is a constant-current sink):
 *
 *   user brightness 0   →  DIM = 255  →  chip sinks 100 %   →  gate ≈ 0 V    →  MOSFET OFF (full off)
 *   user brightness 255 →  DIM = 0    →  chip sinks 0 %     →  gate ≈ 3.3 V  →  MOSFET ON  (full bright)
 *   user brightness B   →  DIM = 255−B → chip PWMs at (255−B)/255 duty @ 430 Hz
 *                                       → MOSFET switches at 430 Hz / (B/255) on-duty
 *
 * I²C bus:   SDA = GPIO 8, SCL = GPIO 9, 400 kHz
 * Expander:  Awinic AW9523B @ 0x58
 *
 * Serial commands (line-based — type and press Enter)
 *
 *   Brightness:
 *     <0..255>      set ALL channels to that brightness (e.g. "128")
 *     <ch>=<value>  set one channel (1..6 = <value>) — e.g. "3=200"
 *
 *   Diagnostics:
 *     r   re-run boot self-test
 *     s   state snapshot
 *     d   dump AW9523B registers
 *     b   I²C bus scan
 *     t   toggle [TRACE] mode (one line per I²C write)
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
static constexpr uint32_t I2C_FREQ_HZ = 400000;   // Fast Mode — AW9523B max

// ── AW9523B I²C address + registers ────────────────────────────────────

static constexpr uint8_t AW9523B_ADDR        = 0x58;
static constexpr uint8_t AW9523B_CHIP_ID_VAL = 0x23;

static constexpr uint8_t REG_INPUT_P0    = 0x00;
static constexpr uint8_t REG_INPUT_P1    = 0x01;
static constexpr uint8_t REG_OUTPUT_P0   = 0x02;
static constexpr uint8_t REG_OUTPUT_P1   = 0x03;
static constexpr uint8_t REG_CONFIG_P0   = 0x04;
static constexpr uint8_t REG_CONFIG_P1   = 0x05;
static constexpr uint8_t REG_CHIP_ID     = 0x10;
static constexpr uint8_t REG_GCR         = 0x11;
static constexpr uint8_t REG_LED_MODE_P0 = 0x12;
static constexpr uint8_t REG_LED_MODE_P1 = 0x13;
static constexpr uint8_t REG_DIM_P0_BASE = 0x24;   // P0_x DIM at 0x24 + x
static constexpr uint8_t REG_SOFT_RESET  = 0x7F;

static constexpr uint8_t GCR_P0_PUSH_PULL = 0x10;

// LED_MODE_P0 = 0xC0 → P0_0..P0_5 in LED-mode (bits 0..5 cleared);
// P0_6/P0_7 stay GPIO so the chip doesn't sink current into unconnected pins.
static constexpr uint8_t LED_MODE_P0_VALUE = 0xC0;

// CH_LED_n → P0_(n−1).
static constexpr uint8_t CHANNEL_COUNT = 6;
static constexpr uint8_t CHANNEL_PIN[CHANNEL_COUNT] = {0, 1, 2, 3, 4, 5};

static constexpr int PIN_BOARD_LED = 48;          // DevKitC-1 onboard LED

// ── Globals ────────────────────────────────────────────────────────────

static int      passCount = 0;
static int      failCount = 0;
static bool     traceI2C  = false;
static uint32_t i2cWrites = 0;
static uint32_t i2cReads  = 0;
static uint32_t i2cNacks  = 0;

// Mirror of last brightness set per channel (DIM register is write-only,
// so we keep our own copy for the snapshot/dump output).
static uint8_t  brightness[CHANNEL_COUNT] = {0, 0, 0, 0, 0, 0};

// Line input buffer for parsing numbers / `c=v` commands.
static constexpr size_t LINE_BUF_SIZE = 32;
static char     lineBuf[LINE_BUF_SIZE];
static size_t   lineLen = 0;

// ── I²C helpers ────────────────────────────────────────────────────────

static bool i2cWriteReg(uint8_t reg, uint8_t value) {
    uint32_t t0 = micros();
    Wire.beginTransmission(AW9523B_ADDR);
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

static bool i2cReadReg(uint8_t reg, uint8_t& out) {
    uint32_t t0 = micros();
    Wire.beginTransmission(AW9523B_ADDR);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(false);
    if (err != 0) {
        i2cReads++; i2cNacks++;
        if (traceI2C) Serial.printf("[TRACE]  %lu us  R reg=0x%02X NACK on addr-ptr err=%u\n",
                                    (unsigned long)(micros() - t0), reg, err);
        return false;
    }
    if (Wire.requestFrom(AW9523B_ADDR, (uint8_t)1) != 1) {
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

// ── Per-channel API (inverted polarity, see file header) ───────────────

static bool setChannel(uint8_t ch, uint8_t value) {
    if (ch >= CHANNEL_COUNT) return false;
    brightness[ch] = value;
    uint8_t dim = (uint8_t)(255 - value);
    return i2cWriteReg(REG_DIM_P0_BASE + CHANNEL_PIN[ch], dim);
}

static void setAllChannels(uint8_t value) {
    for (uint8_t ch = 0; ch < CHANNEL_COUNT; ch++) setChannel(ch, value);
}

// ── Chip configuration ─────────────────────────────────────────────────

static void configureChip() {
    i2cWriteReg(REG_GCR,         GCR_P0_PUSH_PULL);    // ISEL=00 (IMAX) + GPOMD bit
    i2cWriteReg(REG_LED_MODE_P0, LED_MODE_P0_VALUE);   // P0_0..5 LED-mode, P0_6/7 GPIO
    setAllChannels(0);                                  // start with everything OFF
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
    Serial.println("[INFO]   HubFX Local LED Hardware Self-Test");
    Serial.println("================================================================");
    passCount = 0; failCount = 0;

    Serial.println("[STEP 1/4] AW9523B probe");
    Serial.printf("[INFO]   I²C: SDA=GPIO%d SCL=GPIO%d freq=%lu Hz\n",
                  PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    if (!i2cProbe(AW9523B_ADDR)) {
        fail("AW9523B did NOT ACK at 0x%02X — check I²C wiring + power", AW9523B_ADDR);
        Serial.println("[DONE]   Self-test ABORTED — chip not reachable");
        return;
    }
    pass("AW9523B ACK at 0x%02X", AW9523B_ADDR);

    Serial.println("[STEP 2/4] Chip ID + soft reset");
    uint8_t id = 0;
    if (i2cReadReg(REG_CHIP_ID, id)) verifyEq("CHIP_ID (0x10)", id, AW9523B_CHIP_ID_VAL);
    else                             fail("CHIP_ID read failed");
    if (i2cWriteReg(REG_SOFT_RESET, 0x00)) pass("Soft reset accepted");
    else                                   fail("Soft reset write failed");
    delay(2);

    Serial.println("[STEP 3/4] LED-mode configuration");
    configureChip();
    uint8_t gcr = 0, mode = 0;
    if (i2cReadReg(REG_GCR,         gcr))  verifyEq("GCR (push-pull, IMAX)",       gcr,  GCR_P0_PUSH_PULL);
    if (i2cReadReg(REG_LED_MODE_P0, mode)) verifyEq("LED_MODE_P0 (LED on P0_0..5)", mode, LED_MODE_P0_VALUE);

    Serial.println("[STEP 4/4] Per-channel DIM write — all OFF (DIM=0xFF)");
    int writeFails = 0;
    for (uint8_t ch = 0; ch < CHANNEL_COUNT; ch++) {
        if (!setChannel(ch, 0)) {
            fail("CH%u (P0_%u, reg 0x%02X) write NACK",
                 ch + 1, CHANNEL_PIN[ch], REG_DIM_P0_BASE + CHANNEL_PIN[ch]);
            writeFails++;
        } else {
            Serial.printf("[VERIFY] CH%u (P0_%u, reg 0x%02X) DIM=0xFF (LED off) ACK ✓\n",
                          ch + 1, CHANNEL_PIN[ch], REG_DIM_P0_BASE + CHANNEL_PIN[ch]);
            passCount++;
        }
    }
    if (writeFails == 0) pass("All %u channels accepted DIM writes", CHANNEL_COUNT);

    Serial.println("================================================================");
    if (failCount == 0) Serial.printf("[DONE]   Self-test PASS (%d assertions)\n", passCount);
    else                Serial.printf("[DONE]   Self-test FAIL — %d assertions failed (%d passed)\n",
                                       failCount, passCount);
    Serial.println("================================================================");
    Serial.println();
}

// ── Diagnostics ────────────────────────────────────────────────────────

static const char* knownDeviceName(uint8_t addr) {
    if (addr >= 0x40 && addr <= 0x45) return "INA226 (power monitor)";
    if (addr == 0x4C || addr == 0x4D) return "TAS5825M/P (audio codec)";
    if (addr == 0x58)                 return "AW9523B (LED expander)";
    return "?";
}

static void busScan() {
    Serial.println("[INFO]   I²C bus scan 0x08..0x77");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[SCAN]   • 0x%02X — %s\n", addr, knownDeviceName(addr));
            found++;
        }
    }
    Serial.printf("[INFO]   bus scan complete — %d device(s) responded\n", found);
}

static void dumpRegisters() {
    Serial.println("[INFO]   AW9523B register dump");
    struct { uint8_t reg; const char* name; } regs[] = {
        {REG_INPUT_P0,    "INPUT_P0"},    {REG_INPUT_P1,    "INPUT_P1"},
        {REG_OUTPUT_P0,   "OUTPUT_P0"},   {REG_OUTPUT_P1,   "OUTPUT_P1"},
        {REG_CONFIG_P0,   "CONFIG_P0"},   {REG_CONFIG_P1,   "CONFIG_P1"},
        {REG_CHIP_ID,     "CHIP_ID"},     {REG_GCR,         "GCR"},
        {REG_LED_MODE_P0, "LED_MODE_P0"}, {REG_LED_MODE_P1, "LED_MODE_P1"},
    };
    for (const auto& r : regs) {
        uint8_t v = 0;
        if (i2cReadReg(r.reg, v)) Serial.printf("[INFO]     0x%02X %-12s = 0x%02X\n",
                                                r.reg, r.name, v);
        else                      Serial.printf("[INFO]     0x%02X %-12s = ERR\n",
                                                r.reg, r.name);
    }
    Serial.println("[INFO]   Per-channel brightness (DIM regs are write-only):");
    for (uint8_t ch = 0; ch < CHANNEL_COUNT; ch++) {
        uint8_t reg = REG_DIM_P0_BASE + CHANNEL_PIN[ch];
        uint8_t bri = brightness[ch];
        uint8_t dim = (uint8_t)(255 - bri);
        Serial.printf("[INFO]     CH%u (P0_%u, reg 0x%02X)  brightness=%3u/255  DIM=0x%02X\n",
                      ch + 1, CHANNEL_PIN[ch], reg, bri, dim);
    }
}

static void snapshot() {
    Serial.println("[SNAP]   ── State snapshot ────────────────────");
    Serial.printf("[SNAP]   uptime=%lu ms  trace=%s\n",
                  (unsigned long)millis(), traceI2C ? "ON" : "off");
    Serial.println("[SNAP]   drive: AW9523B LED-mode, hardware PWM 8-bit @ ~430 Hz");
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
    Serial.println("[INFO]   Brightness:");
    Serial.println("[INFO]     <0..255>      set ALL channels (e.g. \"128\")");
    Serial.println("[INFO]     <ch>=<value>  set one channel (e.g. \"3=200\", ch=1..6)");
    Serial.println("[INFO]   Diagnostics:");
    Serial.println("[INFO]     r   re-run self-test");
    Serial.println("[INFO]     s   state snapshot");
    Serial.println("[INFO]     d   dump AW9523B registers");
    Serial.println("[INFO]     b   I²C bus scan");
    Serial.println("[INFO]     t   toggle [TRACE]");
    Serial.println("[INFO]     m   this menu");
}

// ── Line parser ────────────────────────────────────────────────────────
//
// Three input shapes:
//   "<digits>"               → set all channels
//   "<digit>=<digits>"       → set one channel
//   "<single letter>"        → diagnostic command
// Whitespace at the edges is ignored. Any other shape is rejected.

static void trimInPlace(char* s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

static bool parseUInt(const char* s, uint16_t& out) {
    if (!*s) return false;
    uint16_t v = 0;
    for (const char* p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
        v = (uint16_t)(v * 10 + (*p - '0'));
        if (v > 65535) return false;
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
        case 'b': case 'B': busScan();       break;
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

    // Shape 1: single letter command.
    if (line[0] && !line[1] && !isdigit((unsigned char)line[0])) {
        handleSingleChar(line[0]);
        return;
    }

    // Shape 2: "<ch>=<value>" — exactly one '=' separator.
    char* eq = strchr(line, '=');
    if (eq) {
        *eq = 0;
        char* lhs = line;
        char* rhs = eq + 1;
        trimInPlace(lhs); trimInPlace(rhs);

        uint16_t chNum = 0, value = 0;
        if (!parseUInt(lhs, chNum) || !parseUInt(rhs, value)) {
            Serial.printf("[INFO]   bad format: '<channel>=<value>' (got '%s=%s')\n", lhs, rhs);
            return;
        }
        if (chNum < 1 || chNum > CHANNEL_COUNT) {
            Serial.printf("[INFO]   channel out of range: %u (must be 1..%u)\n",
                          chNum, CHANNEL_COUNT);
            return;
        }
        if (value > 255) {
            Serial.printf("[INFO]   value out of range: %u (must be 0..255)\n", value);
            return;
        }
        uint8_t ch  = (uint8_t)(chNum - 1);
        uint8_t bri = (uint8_t)value;
        setChannel(ch, bri);
        Serial.printf("[INFO]   CH%u (P0_%u) brightness=%u/255  DIM=0x%02X\n",
                      chNum, CHANNEL_PIN[ch], bri, (uint8_t)(255 - bri));
        return;
    }

    // Shape 3: pure number → set all channels.
    uint16_t value = 0;
    if (!parseUInt(line, value)) {
        Serial.printf("[INFO]   unrecognised input '%s' — type 'm' for menu\n", line);
        return;
    }
    if (value > 255) {
        Serial.printf("[INFO]   value out of range: %u (must be 0..255)\n", value);
        return;
    }
    uint8_t bri = (uint8_t)value;
    setAllChannels(bri);
    Serial.printf("[INFO]   ALL channels brightness=%u/255  DIM=0x%02X\n",
                  bri, (uint8_t)(255 - bri));
}

// ── Setup / loop ───────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    uint32_t serialWait = millis();
    while (!Serial && (millis() - serialWait < 3000)) delay(10);

    Serial.println();
    Serial.println("================================================================");
    Serial.println("[INFO]   HubFX Local LED Hardware Test");
    Serial.println("[INFO]   AW9523B LED-mode, hardware 8-bit PWM @ 430 Hz");
    Serial.println("[INFO]   topology: pull-up + N-MOSFET (inverted DIM polarity)");
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

    // Heartbeat LED on the dev board — confirms loop is alive without
    // polluting the serial log.
    static uint32_t ledToggle = 0;
    if (now - ledToggle > 500) {
        ledToggle = now;
        digitalWrite(PIN_BOARD_LED, !digitalRead(PIN_BOARD_LED));
    }

    // Accumulate serial input into a line buffer; dispatch on \r or \n.
    // Locally echo printable chars + handle backspace so the user can
    // see what they're typing — `pio device monitor` doesn't echo by
    // default, which made input look dead even when it was working.
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
            // Backspace / DEL — erase one character from the buffer
            // and from the visible terminal line.
            if (lineLen > 0) {
                lineLen--;
                Serial.print("\b \b");
            }
            continue;
        }

        // Ignore other control bytes — only printable ASCII goes in the buffer.
        if (c < 0x20 || c > 0x7E) continue;

        if (lineLen + 1 < LINE_BUF_SIZE) {
            lineBuf[lineLen++] = c;
            Serial.write(c);
        }
        // Silently drop overflow; a newline resets lineLen.
    }
}
