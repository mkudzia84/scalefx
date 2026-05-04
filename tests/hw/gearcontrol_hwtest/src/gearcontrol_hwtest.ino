/**
 * GearControl Hardware Smoke Test
 *
 * Standalone Arduino sketch that exercises every physical pin on the
 * GearControl PCB. Run on a bare board (no ScaleFX protocol, no HubFX) to
 * verify soldering, servo wiring, motor driver outputs, RC input capture,
 * battery-divider calibration, and I²C presence in a single firmware.
 *
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core.
 *
 * Behaviour (runs forever):
 *   Servos    — all 7 slots sweep synchronously 500→2500→500µs, ~4 s/cycle
 *   Motors    — all 3 H-bridges cycle through (cw,ccw) = 0,0 → 0,1 → 1,0 →
 *               1,1 with 1 s dwell per phase (4 s full cycle). Exercises
 *               each output leg alone and both together.
 *   Status LEDs — mirror the matching motor leg so the active phase is
 *                 readable at a glance
 *   Indicator LEDs — alternating heartbeat on GP13 (green) / GP14 (red)
 *   GearInput — GP0 pulse width captured via edge ISR
 *   Battery   — GP29 ADC, assumed ÷6 divider, 4-sample oversample
 *   I²C       — one-shot bus scan at boot, results printed to serial
 *
 * Serial output: 6 Mbps, human-readable status every 500 ms.
 *
 * Safety: phase 3 (1,1) briefly shorts both H-bridge legs high — brake /
 * shoot-through depending on the driver IC. Run with the motor unwired (or
 * the motor-output fuses pulled) unless you know your driver handles it.
 */

#include <Arduino.h>
#include <Servo.h>
#include <Wire.h>

// ============================================================================
//  TYPES — declared up front so the Arduino preprocessor's auto-generated
//  function prototypes (inserted at the top of the .ino) see them.
// ============================================================================

struct MotorPair { uint8_t cw; uint8_t ccw; };

// ============================================================================
//  PIN MAP (mirrors gearcontrol_pico.ino)
// ============================================================================

// User-mappable slots (pin1..pin7 on the board silkscreen).
static const uint8_t SERVO_PIN[7]   = { 1, 2, 3, 6, 7, 8, 9 };
static const char*   SERVO_LABEL[7] = { "pin1", "pin2", "pin3", "pin4", "pin5", "pin6", "pin7" };

// Fixed-function pins.
static const uint8_t PIN_GEAR_INPUT = 0;
static const uint8_t PIN_SDA        = 4;
static const uint8_t PIN_SCL        = 5;
static const uint8_t PIN_LED_CONN   = 13;    // "green" indicator
static const uint8_t PIN_LED_ERR    = 14;    // "red" indicator
static const uint8_t PIN_VSENSE     = 29;

// Motor H-bridge CW/CCW pairs (3 gears).
static const MotorPair MOTOR[3] = {
    { 16, 15 },   // motor 0: CW=GP16, CCW=GP15 (swapped vs. original schematic)
    { 17, 18 },
    { 19, 20 },
};

// Status LEDs (CW / CCW per gear) — lit alongside the matching motor phase.
static const MotorPair STATUS_LED[3] = {
    { 21, 22 },
    { 23, 24 },
    { 25, 26 },
};

// ============================================================================
//  SERVO SWEEP
// ============================================================================

static Servo servos[7];

// Sweep endpoints — the absolute PWM range the firmware allows. Narrow down
// if your mechanical servos physically bind before this range.
static constexpr uint16_t SWEEP_MIN_US    = 500;
static constexpr uint16_t SWEEP_MAX_US    = 2500;
static constexpr uint32_t SWEEP_PERIOD_MS = 4000;    // full 500→2500→500 cycle

static uint16_t currentServoUs = 1500;

static void updateServos(uint32_t nowMs) {
    // Triangular sweep — phase ∈ [0, 2π) over SWEEP_PERIOD_MS, then shaped
    // into a symmetric ramp via abs(0.5 - fraction). Keeps the servo moving
    // at a predictable mean speed regardless of the period.
    const uint32_t t = nowMs % SWEEP_PERIOD_MS;
    const float frac = (float)t / (float)SWEEP_PERIOD_MS;           // 0..1
    const float tri  = 1.0f - 2.0f * fabsf(frac - 0.5f);            // 0..1..0
    currentServoUs = SWEEP_MIN_US + (uint16_t)(tri * (SWEEP_MAX_US - SWEEP_MIN_US));
    for (uint8_t i = 0; i < 7; i++) {
        servos[i].writeMicroseconds(currentServoUs);
    }
}

// ============================================================================
//  MOTOR DRIVE (4-phase walking pattern, 1 s/phase)
// ============================================================================

// Walk every H-bridge through all four (cw, ccw) input combinations with
// equal dwell time on each state, so each output leg is exercised both
// alone and together with its partner:
//
//   phase 0 = (0, 0)  both legs off (coast / hi-Z)
//   phase 1 = (0, 1)  CCW leg high, CW low
//   phase 2 = (1, 0)  CW leg high,  CCW low
//   phase 3 = (1, 1)  both legs high (brake / supply-rail probe)
//
// Full cycle = 4 s. Encoding: phase bit 1 → CW, phase bit 0 → CCW, mirrored
// on the matching status LED pair so you can see which state is active
// without a scope.
static constexpr uint32_t MOTOR_PHASE_MS = 1000;   // dwell per state

static void driveMotorPhase(uint8_t gearIdx, uint8_t phase) {
    const MotorPair& m  = MOTOR[gearIdx];
    const MotorPair& sl = STATUS_LED[gearIdx];
    const bool cwHigh  = (phase >> 1) & 0x01;
    const bool ccwHigh =  phase       & 0x01;
    digitalWrite(m.cw,   cwHigh  ? HIGH : LOW);
    digitalWrite(m.ccw,  ccwHigh ? HIGH : LOW);
    digitalWrite(sl.cw,  cwHigh  ? HIGH : LOW);
    digitalWrite(sl.ccw, ccwHigh ? HIGH : LOW);
}

static uint8_t motorPhaseFor(uint32_t nowMs) {
    return (uint8_t)((nowMs / MOTOR_PHASE_MS) & 0x03);
}

// ============================================================================
//  GEAR-INPUT CAPTURE (GP0)
// ============================================================================

// ISR state — volatile because read from loop(), written from interrupt.
static volatile uint32_t gearInputRiseUs   = 0;
static volatile uint16_t gearInputPulse_us = 0;
static volatile uint32_t gearInputEdgeMs   = 0;

static void gearInputIsr() {
    const uint32_t now = micros();
    if (digitalRead(PIN_GEAR_INPUT)) {
        gearInputRiseUs = now;
    } else if (gearInputRiseUs != 0) {
        const uint32_t pulse = now - gearInputRiseUs;
        if (pulse >= 500 && pulse <= 2500) {
            gearInputPulse_us = (uint16_t)pulse;
            gearInputEdgeMs   = millis();
        }
    }
}

// Stale if we haven't seen a valid pulse in the last 200 ms.
static uint16_t gearInputCurrent_us() {
    noInterrupts();
    const uint16_t pulse = gearInputPulse_us;
    const uint32_t edge  = gearInputEdgeMs;
    interrupts();
    if (millis() - edge > 200) return 0;
    return pulse;
}

// ============================================================================
//  BATTERY SENSE (GP29)
// ============================================================================

// RP2040 ADC: 12-bit, 3.3 V reference. Assumed divider ratio matches the
// production firmware's ÷6 (50 k / 10 k). Adjust if this test is run on a
// board with a different divider — the printed mV scales linearly.
static constexpr float DIVIDER_RATIO = 6.0f;

static uint16_t batteryMilliVolts() {
    const uint8_t OVERSAMPLE = 4;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < OVERSAMPLE; i++) sum += analogRead(PIN_VSENSE);
    const uint32_t raw = sum / OVERSAMPLE;
    // mV = raw * 3300 / 4095 * DIVIDER_RATIO
    return (uint16_t)((raw * 3300UL * (uint32_t)(DIVIDER_RATIO * 1000)) / (4095UL * 1000UL));
}

// ============================================================================
//  I²C BUS SCAN (one-shot at boot)
// ============================================================================

static void scanI2C() {
    Wire.setSDA(PIN_SDA);
    Wire.setSCL(PIN_SCL);
    Wire.begin();
    Wire.setClock(400000);

    Serial.print("I2C scan: ");
    uint8_t found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            if (found > 0) Serial.print(", ");
            Serial.printf("0x%02X", addr);
            found++;
        }
    }
    if (found == 0) Serial.print("(no devices)");
    Serial.println();
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================

void setup() {
    Serial.begin(6000000);
    // Wait up to 2 s for the host to open the port, but don't block forever —
    // bench-only operation should still run headless.
    const uint32_t bootStart = millis();
    while (!Serial && (millis() - bootStart) < 2000) { /* spin */ }

    Serial.println();
    Serial.println("── GearControl HW Test ──");
    Serial.println("Servos sweep 500-2500µs; motors cycle CW/idle/CCW/idle;");
    Serial.println("GP0 input captured via ISR; GP29 battery sampled every 500ms.");

    // Servo channels — attach each to its GPIO, send to centre position.
    for (uint8_t i = 0; i < 7; i++) {
        servos[i].attach(SERVO_PIN[i], SWEEP_MIN_US, SWEEP_MAX_US);
        servos[i].writeMicroseconds(1500);
    }

    // Motor H-bridges + status LEDs — digital outputs. Start every pair at
    // phase 0 (both legs LOW); loop() advances the phase on a 1 s tick.
    for (uint8_t i = 0; i < 3; i++) {
        pinMode(MOTOR[i].cw,       OUTPUT);
        pinMode(MOTOR[i].ccw,      OUTPUT);
        pinMode(STATUS_LED[i].cw,  OUTPUT);
        pinMode(STATUS_LED[i].ccw, OUTPUT);
        driveMotorPhase(i, 0);
    }

    // Indicator LEDs.
    pinMode(PIN_LED_CONN, OUTPUT); digitalWrite(PIN_LED_CONN, LOW);
    pinMode(PIN_LED_ERR,  OUTPUT); digitalWrite(PIN_LED_ERR,  LOW);

    // Battery ADC — 12-bit for full Pico resolution.
    analogReadResolution(12);

    // Gear-input RC PWM capture.
    pinMode(PIN_GEAR_INPUT, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_GEAR_INPUT), gearInputIsr, CHANGE);

    // I²C scan — single pass, results logged. Leaves Wire initialised so
    // follow-up debugging can be bolted on without re-init.
    scanI2C();

    Serial.println("Running…");
}

void loop() {
    const uint32_t nowMs = millis();

    // ---- Continuous actuation ----
    updateServos(nowMs);

    // Motor phase walk — only re-apply to the pins when the phase index
    // actually changes, so we're not re-writing the same value every
    // iteration of this tight loop.
    static uint8_t lastPhase = 0xFF;
    const uint8_t phase = motorPhaseFor(nowMs);
    if (phase != lastPhase) {
        lastPhase = phase;
        for (uint8_t i = 0; i < 3; i++) driveMotorPhase(i, phase);
    }

    // Heartbeat — alternate indicator LEDs every 500 ms as a "firmware alive"
    // signal the user can see even without the serial monitor.
    const bool heartbeat = (nowMs / 500) & 1;
    digitalWrite(PIN_LED_CONN, heartbeat ? HIGH : LOW);
    digitalWrite(PIN_LED_ERR,  heartbeat ? LOW  : HIGH);

    // ---- 500 ms status line ----
    static uint32_t lastReportMs = 0;
    if (nowMs - lastReportMs >= 500) {
        lastReportMs = nowMs;

        const uint16_t gearIn = gearInputCurrent_us();
        const uint16_t battMv = batteryMilliVolts();
        const uint8_t cwBit  = (phase >> 1) & 0x01;
        const uint8_t ccwBit =  phase       & 0x01;

        Serial.printf("t=%lu  servo=%4uus  gearIn=%4uus  batt=%umV  motor=(cw=%u ccw=%u)\n",
                      (unsigned long)nowMs,
                      currentServoUs,
                      gearIn,
                      battMv,
                      cwBit, ccwBit);
    }

    // Tight loop — servo updates can run at full speed; Servo library caps
    // pulse refresh internally.
}
