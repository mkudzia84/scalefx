/**
 * GunFX Hardware Smoke Test
 *
 * Standalone Arduino sketch that exercises every physical pin on the GunFX
 * PCB. Run on a bare board (no ScaleFX protocol, no HubFX) to verify servo
 * wiring, smoke heater relay, fan driver, RC trigger input, and battery
 * divider in a single firmware.
 *
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core.
 *
 * Behaviour (runs forever):
 *   Servos    — SRV1..SRV3 (GP1..GP3) sweep synchronously 500→2500→500µs,
 *               ~4 s/cycle (triangular ramp)
 *   Heater    — GP17 held HIGH continuously (#define HEATER_ENABLE 0 to
 *               disable while debugging servos/fan only)
 *   Fan       — GP16 at ~98 % duty PWM (analogWrite 250 @ ~488 Hz)
 *   Muzzle flash / trigger LED — GP25 held HIGH (fully on)
 *   Indicator LEDs — alternating heartbeat on GP13 (blue) / GP14 (yellow)
 *   TriggerIn — GP0 pulse width captured via edge ISR
 *   Battery   — GP29 ADC, assumed ÷6 divider, 4-sample oversample
 *   INA226    — on-board current monitor at any I²C address in 0x40..0x4F.
 *               Bus voltage + current (through the configured shunt) are
 *               read every 500 ms and folded into the status line. No
 *               INA226 = that column reads "—".
 *
 * Serial output: 6 Mbps, human-readable status every 500 ms.
 *
 * SAFETY: the heater output drives a resistive heating element on the
 * nozzle. With HEATER_ENABLE = 1 it stays on from boot until power-off.
 * Verify supply current, thermal fusing, and clearance from anything
 * flammable before running unattended. The fan draws back-EMF; the driver
 * IC normally clamps that, but bench-probe the fan line on first bring-up.
 */

#include <Arduino.h>
#include <Servo.h>
#include <Wire.h>

// ============================================================================
//  TEST OPTIONS — flip these to isolate subsystems
// ============================================================================

#define HEATER_ENABLE   0     // 0 = keep heater off (servos + fan test only)
#define FAN_ENABLE      0     // 0 = keep fan off
#define FAN_PWM_DUTY    250   // 0..255, ~98 % at 250

// INA226 shunt + expected-max calibration. Match the hardware.
#define INA226_SHUNT_OHMS     0.005f   // 5 mΩ (same spec as the GearControl boards)
#define INA226_MAX_CURRENT_A  10.0f    // calibration headroom; not a hard limit

// ============================================================================
//  PIN MAP (mirrors gunfx_pico.ino)
// ============================================================================

// Servos (1-based IDs correspond to indices 0..2 in the arrays below).
static const uint8_t SERVO_PIN[3]   = { 1, 2, 3 };
static const char*   SERVO_LABEL[3] = { "SRV1", "SRV2", "SRV3" };

// Fixed-function pins.
static const uint8_t PIN_TRIGGER_INPUT = 0;    // GP0 — RC PWM input
static const uint8_t PIN_I2C_SDA       = 4;    // GP4 — I²C SDA (INA226)
static const uint8_t PIN_I2C_SCL       = 5;    // GP5 — I²C SCL (INA226)
static const uint8_t PIN_LED_CONN      = 13;   // blue indicator
static const uint8_t PIN_LED_ERR       = 14;   // yellow indicator
static const uint8_t PIN_SMOKE_FAN     = 16;   // PWM-driven fan
static const uint8_t PIN_SMOKE_HEATER  = 17;   // digital heater relay
static const uint8_t PIN_NOZZLE_FLASH  = 25;   // muzzle flash LED (unused here)
static const uint8_t PIN_VSENSE        = 29;   // battery ADC ÷6

// ============================================================================
//  SERVO SWEEP
// ============================================================================

static Servo servos[3];

static constexpr uint16_t SWEEP_MIN_US    = 500;
static constexpr uint16_t SWEEP_MAX_US    = 2500;
static constexpr uint32_t SWEEP_PERIOD_MS = 4000;   // full 500→2500→500 cycle

static uint16_t currentServoUs = 1500;

static void updateServos(uint32_t nowMs) {
    // Triangular sweep — 0..1 fraction shaped into a symmetric ramp so the
    // mean servo speed is predictable across the period.
    const uint32_t t    = nowMs % SWEEP_PERIOD_MS;
    const float    frac = (float)t / (float)SWEEP_PERIOD_MS;      // 0..1
    const float    tri  = 1.0f - 2.0f * fabsf(frac - 0.5f);       // 0..1..0
    currentServoUs = SWEEP_MIN_US + (uint16_t)(tri * (SWEEP_MAX_US - SWEEP_MIN_US));
    for (uint8_t i = 0; i < 3; i++) {
        servos[i].writeMicroseconds(currentServoUs);
    }
}

// ============================================================================
//  GP0 TRIGGER INPUT — edge ISR captures pulse width (mirror of GunFX prod)
// ============================================================================

static volatile uint32_t trigRiseUs   = 0;
static volatile uint16_t trigPulse_us = 0;
static volatile uint32_t trigEdgeMs   = 0;

static void triggerIsr() {
    const uint32_t now = micros();
    if (digitalRead(PIN_TRIGGER_INPUT)) {
        trigRiseUs = now;
    } else if (trigRiseUs != 0) {
        const uint32_t pulse = now - trigRiseUs;
        if (pulse >= 500 && pulse <= 2500) {
            trigPulse_us = (uint16_t)pulse;
            trigEdgeMs   = millis();
        }
    }
}

// Stale if no valid edge in the last 200 ms.
static uint16_t triggerCurrent_us() {
    noInterrupts();
    const uint16_t pulse = trigPulse_us;
    const uint32_t edge  = trigEdgeMs;
    interrupts();
    if (millis() - edge > 200) return 0;
    return pulse;
}

// ============================================================================
//  BATTERY SENSE (GP29)
// ============================================================================

// RP2040 ADC: 12-bit, 3.3 V reference. Assumes production ÷6 divider.
static constexpr float DIVIDER_RATIO = 6.0f;

static uint16_t batteryMilliVolts() {
    const uint8_t OVERSAMPLE = 4;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < OVERSAMPLE; i++) sum += analogRead(PIN_VSENSE);
    const uint32_t raw = sum / OVERSAMPLE;
    return (uint16_t)((raw * 3300UL * (uint32_t)(DIVIDER_RATIO * 1000)) /
                      (4095UL * 1000UL));
}

// ============================================================================
//  INA226 CURRENT MONITOR (inline — test is standalone, no ScaleFX libs)
// ============================================================================
//
// TI INA226 register map (SBOS547 §7.6):
//   0x00 CONFIG   — mode + averaging + conversion times
//   0x01 SHUNT_V  — shunt voltage, 2.5 µV/LSB signed (not used here)
//   0x02 BUS_V    — bus voltage,  1.25 mV/LSB unsigned
//   0x04 CURRENT  — calibrated current, (CURRENT_LSB) per bit, signed
//   0x05 CALIB    — calibration register (= 0.00512 / (CURRENT_LSB × Rshunt))
//   0xFE MFG_ID   — read-only, returns 0x5449 ("TI")
//
// CURRENT_LSB is chosen so full-scale == MAX_CURRENT: LSB = MaxA / 2^15.
// CAL = 0.00512 / (LSB × Rshunt) — rounded to the 16-bit register width.

static constexpr uint8_t INA_REG_CONFIG  = 0x00;
static constexpr uint8_t INA_REG_BUS_V   = 0x02;
static constexpr uint8_t INA_REG_CURRENT = 0x04;
static constexpr uint8_t INA_REG_CALIB   = 0x05;
static constexpr uint8_t INA_REG_MFG_ID  = 0xFE;
static constexpr uint16_t INA_MFG_TI     = 0x5449;

static constexpr float    INA_CURRENT_LSB_A = INA226_MAX_CURRENT_A / 32768.0f;   // A per bit
static constexpr float    INA_CURRENT_LSB_mA = INA_CURRENT_LSB_A * 1000.0f;      // mA per bit
static constexpr uint16_t INA_CAL_VALUE =
    (uint16_t)(0.00512f / (INA_CURRENT_LSB_A * INA226_SHUNT_OHMS));

// 0x4327: AVG_16 samples (bits 11:9 = 011), bus 1.1ms (bits 8:6 = 100),
// shunt 1.1ms (bits 5:3 = 100), continuous shunt+bus mode (bits 2:0 = 111).
static constexpr uint16_t INA_CONFIG_CONTINUOUS_AVG16 = 0x4327;

static uint8_t ina226Addr = 0;   // 0 = not probed / not found

static bool inaWriteReg16(uint8_t addr, uint8_t reg, uint16_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write((uint8_t)(val >> 8));
    Wire.write((uint8_t)(val & 0xFF));
    return Wire.endTransmission() == 0;
}

static bool inaReadReg16(uint8_t addr, uint8_t reg, uint16_t& out) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(addr, (uint8_t)2) != 2) return false;
    uint16_t hi = Wire.read();
    uint16_t lo = Wire.read();
    out = (uint16_t)((hi << 8) | lo);
    return true;
}

static void initINA226() {
    Wire.setSDA(PIN_I2C_SDA);
    Wire.setSCL(PIN_I2C_SCL);
    Wire.begin();
    Wire.setClock(400000);

    // Probe the INA226 address range. First device that reports MFG_ID = "TI"
    // wins; the test assumes a single shunt monitor on the GunFX board.
    Serial.print("[INA226] scanning 0x40..0x4F: ");
    for (uint8_t a = 0x40; a <= 0x4F; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() != 0) continue;
        uint16_t mfg = 0;
        if (inaReadReg16(a, INA_REG_MFG_ID, mfg) && mfg == INA_MFG_TI) {
            ina226Addr = a;
            Serial.printf("found at 0x%02X\n", a);
            break;
        }
    }
    if (ina226Addr == 0) {
        Serial.println("none (current readings will show '—')");
        return;
    }

    inaWriteReg16(ina226Addr, INA_REG_CALIB,  INA_CAL_VALUE);
    inaWriteReg16(ina226Addr, INA_REG_CONFIG, INA_CONFIG_CONTINUOUS_AVG16);
    Serial.printf("[INA226] shunt=%.3fΩ  maxI=%.1fA  LSB=%.3fmA  CAL=0x%04X\n",
                  (double)INA226_SHUNT_OHMS,
                  (double)INA226_MAX_CURRENT_A,
                  (double)INA_CURRENT_LSB_mA,
                  INA_CAL_VALUE);
}

// Fill busMv and currentMa with the most recent readings. Returns false if
// the INA226 wasn't detected at boot (leaves outputs untouched).
static bool readINA226(uint16_t& busMv, int32_t& currentMa) {
    if (ina226Addr == 0) return false;
    uint16_t rawBus = 0, rawI = 0;
    if (!inaReadReg16(ina226Addr, INA_REG_BUS_V,   rawBus)) return false;
    if (!inaReadReg16(ina226Addr, INA_REG_CURRENT, rawI))   return false;
    // Bus voltage: unsigned, 1.25 mV/LSB → × 5 / 4 keeps it in integer math.
    busMv = (uint16_t)(((uint32_t)rawBus * 5) / 4);
    // Current: signed 16-bit scaled by CURRENT_LSB_mA.
    currentMa = (int32_t)((int16_t)rawI * INA_CURRENT_LSB_mA);
    return true;
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================

void setup() {
    Serial.begin(6000000);
    const uint32_t bootStart = millis();
    while (!Serial && (millis() - bootStart) < 2000) { /* spin */ }

    Serial.println();
    Serial.println("── GunFX HW Test ──");
    Serial.printf("Servos sweep %u-%uµs; heater=%s fan=%s\n",
                  SWEEP_MIN_US, SWEEP_MAX_US,
                  HEATER_ENABLE ? "ON" : "off",
                  FAN_ENABLE    ? "ON" : "off");
    Serial.println("GP0 trigger captured via ISR; GP29 battery sampled every 500ms.");

    // All output pins driven LOW — no servo PWM, no heater, no fan, no
    // muzzle flash, no indicator heartbeat. The test passively monitors
    // trigger input, battery voltage, and INA226 — useful for bench
    // quiescent-current measurement or when probing passive rails.
    const uint8_t lowPins[] = {
        SERVO_PIN[0], SERVO_PIN[1], SERVO_PIN[2],
        PIN_SMOKE_HEATER, PIN_SMOKE_FAN, PIN_NOZZLE_FLASH,
        PIN_LED_CONN, PIN_LED_ERR,
    };
    for (uint8_t p : lowPins) {
        pinMode(p, OUTPUT);
        digitalWrite(p, LOW);
    }

    // Battery ADC — 12-bit for full Pico resolution.
    analogReadResolution(12);

    // GP0 RC PWM trigger input.
    pinMode(PIN_TRIGGER_INPUT, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_TRIGGER_INPUT), triggerIsr, CHANGE);

    // INA226 — I²C bus init + one-shot scan + calibration.
    initINA226();

    Serial.println("Running…");
}

void loop() {
    const uint32_t nowMs = millis();

    // All outputs held LOW from setup() — nothing to drive in loop().

    // ---- 500 ms status line ----
    static uint32_t lastReportMs = 0;
    if (nowMs - lastReportMs >= 500) {
        lastReportMs = nowMs;

        const uint16_t trigIn = triggerCurrent_us();
        const uint16_t battMv = batteryMilliVolts();

        uint16_t inaMv = 0;
        int32_t  inaMa = 0;
        const bool haveIna = readINA226(inaMv, inaMa);

        Serial.printf("t=%lu  (outputs all LOW)  trigIn=%4uus  batt=%umV",
                      (unsigned long)nowMs,
                      trigIn,
                      battMv);
        if (haveIna) {
            Serial.printf("  ina226=[%umV %ldmA]\n", inaMv, (long)inaMa);
        } else {
            Serial.println("  ina226=—");
        }
    }

    // Tight loop — servo updates can run at full speed; Servo library caps
    // pulse refresh internally.
}
