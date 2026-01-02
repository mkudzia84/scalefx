/**
 * GunFX Pico Controller
 * 
 * Slave controller for gun effects - receives commands from main Pi over USB serial.
 * Controls: muzzle flash LED (PWM), smoke heater/fan, 3x gun servos with motion profiling.
 * 
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: GunFxSerialSlave @ 115200 baud, COBS framed, 90s connection timeout
 */

#include <Arduino.h>
#include <Servo.h>
#include <math.h>
#include <serial_gunfx.h>
#include <led_control.h>
#include <srv_control.h>
#include <pico/unique_id.h>

// ============================================================================
//  PIN CONFIGURATION
// ============================================================================

const uint8_t PIN_GUN_SRV_1    =  1;   // Gun servo 1
const uint8_t PIN_GUN_SRV_2    =  2;   // Gun servo 2
const uint8_t PIN_GUN_SRV_3    =  3;   // Gun servo 3
const uint8_t PIN_LED_BLUE     = 13;   // Status LED (connection/firing)
const uint8_t PIN_LED_YELLOW   = 14;   // Heater indicator LED
const uint8_t PIN_SMOKE_FAN    = 16;   // Smoke fan motor relay
const uint8_t PIN_SMOKE_HEATER = 17;   // Smoke heater relay
const uint8_t PIN_NOZZLE_FLASH = 25;   // Muzzle flash LED (PWM)

// ============================================================================
//  CONSTANTS
// ============================================================================

// Serial communication
const uint32_t SERIAL_BAUD         = 115200;
const uint32_t STATUS_INTERVAL_MS  = 1000;    // Status telemetry interval

// Muzzle flash timing
const uint8_t  FLASH_PWM_DUTY      = 255;     // Full brightness
const uint16_t FLASH_PULSE_MS      = 30;      // Duration at full brightness
const uint16_t FLASH_FADE_MS       = 80;      // Fade-out duration
const uint8_t  FLASH_FADE_STEPS    = 20;      // Steps in fade animation

// Servo defaults
const uint16_t SERVO_DEFAULT_US    = 1500;    // Center position
const int SERVO_DEFAULT_MAX_SPEED  = 4000;    // μs/sec
const int SERVO_DEFAULT_ACCEL      = 8000;    // μs/sec²
const int SERVO_DEFAULT_DECEL      = 8000;    // μs/sec²

// ============================================================================
//  STATE VARIABLES
// ============================================================================

// Serial communication
GunFxSerialSlave serialSlave;
uint32_t next_status_ms = 0;
char deviceName[24];  // "GunFX-XXXX" with 4-char unique suffix

// Connection watchdog
bool watchdog_triggered = false;

/**
 * @brief Get unique 4-character suffix from Pico's flash ID
 */
void buildDeviceName() {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    // Use last 2 bytes of 8-byte ID for 4-char hex suffix
    snprintf(deviceName, sizeof(deviceName), "GunFX-%02X%02X", 
             id.id[6], id.id[7]);
}
bool was_master_connected = false;

// Firing state
bool is_firing = false;
int rate_of_fire_rpm = 0;
uint32_t shot_interval_ms = 0;
uint32_t next_shot_time_ms = 0;

// Muzzle flash state
bool flash_active = false;
bool flash_fading = false;
uint32_t flash_off_time_ms = 0;
uint32_t fade_start_time_ms = 0;
uint8_t fade_step_duration_ms = 0;

// Smoke generator state
bool smoke_heater_on = false;
bool smoke_fan_on = false;
bool smoke_fan_pending_off = false;
uint32_t smoke_fan_off_time_ms = 0;
uint16_t smoke_fan_off_delay_ms = 0;

// Servos with motion profiling
ServoControl gun_servos[3];

struct RecoilJerkConfig { int jerk_us, variance_us; };
RecoilJerkConfig servo_jerk_configs[3] = {{0, 0}, {0, 0}, {0, 0}};

// Status LEDs
LedControl led_blue, led_yellow;
uint32_t blue_led_next_toggle_ms = 0;
const uint32_t BLUE_LED_WATCHDOG_ON_MS  = 1000;   // Blink on duration
const uint32_t BLUE_LED_WATCHDOG_OFF_MS = 2000;   // Blink off duration

// ============================================================================
//  CONNECTION MANAGEMENT
// ============================================================================

void checkConnectionStatus() {
  bool connected = serialSlave.isMasterConnected();
  
  // Detect connection loss
  if (was_master_connected && !connected) {
    Serial.println("WARN: Connection timeout - performing shutdown");
    performSafeShutdown();
    watchdog_triggered = true;
  }
  
  // Detect reconnection
  if (!was_master_connected && connected) {
    watchdog_triggered = false;
  }
  
  was_master_connected = connected;
}

void performSafeShutdown() {
  stopFiring(0);
  setSmokeHeater(false);
  setSmokeFan(false);
  setNozzleFlash(false);
}

void performSafeInit() {
  stopFiring(0);
  setSmokeHeater(false);
  watchdog_triggered = false; // Clear watchdog flag on new init
  // Reset servos to center
  for (int i = 0; i < 3; i++) {
    setServoPulse(i + 1, SERVO_DEFAULT_US);
  }
}

// ============================================================================
//  LED CONTROL
// ============================================================================

void updateYellowLED() {
  // Yellow LED: solid ON when heater is on
  led_yellow.set(smoke_heater_on);
}

void updateBlueLED(uint32_t now_ms) {
  // Blue LED behavior:
  // - OFF when all is OK (idle, no issues)
  // - Synced with muzzle flash when firing (same blink rate as nozzle)
  // - Blinking 1s on / 2s off when watchdog triggered (no signal from main board)
  
  if (watchdog_triggered) {
    // No signal pattern: 1s on, 2s off
    if (now_ms >= blue_led_next_toggle_ms) {
      led_blue.toggle();
      if (led_blue.isOn()) {
        blue_led_next_toggle_ms = now_ms + BLUE_LED_WATCHDOG_ON_MS;
      } else {
        blue_led_next_toggle_ms = now_ms + BLUE_LED_WATCHDOG_OFF_MS;
      }
    }
  } else if (is_firing && (flash_active || flash_fading)) {
    // Sync with muzzle flash - LED on when flash is active or fading
    led_blue.on();
  } else {
    // All OK or between shots: LED off
    led_blue.off();
  }
}

void updateLEDs(uint32_t now_ms) {
  updateYellowLED();
  updateBlueLED(now_ms);
}

// ============================================================================
//  SERVO CONTROL
// ============================================================================

void setServoPulse(uint8_t servo_id, int pulse_us) {
  if (servo_id == 0 || servo_id > 3) return;
  ServoControl* servo = &gun_servos[servo_id - 1];
  int target = constrain(pulse_us, servo->minLimit(), servo->maxLimit());
  servo->setTarget(target);
}

void applyRecoilJerk() {
  for (int i = 0; i < 3; i++) {
    RecoilJerkConfig* jerk = &servo_jerk_configs[i];
    if (jerk->jerk_us == 0) {
      gun_servos[i].clearJerk();
    } else {
      int direction = (random(2) == 0) ? 1 : -1;
      int variance = jerk->variance_us > 0 ? random(jerk->variance_us + 1) : 0;
      gun_servos[i].applyJerk(direction * (jerk->jerk_us + variance));
    }
  }
}

void clearRecoilJerk() {
  for (int i = 0; i < 3; i++) gun_servos[i].clearJerk();
}

// ============================================================================
//  HARDWARE OUTPUT CONTROL
// ============================================================================

void setNozzleFlash(bool on) {
  analogWrite(PIN_NOZZLE_FLASH, on ? FLASH_PWM_DUTY : 0);
}

void setSmokeHeater(bool on) {
  smoke_heater_on = on;
  digitalWrite(PIN_SMOKE_HEATER, on ? HIGH : LOW);
}

void setSmokeFan(bool on) {
  smoke_fan_on = on;
  smoke_fan_pending_off = false;
  digitalWrite(PIN_SMOKE_FAN, on ? HIGH : LOW);
}

void scheduleSmokeFanOff(uint16_t delay_ms) {
  if (delay_ms == 0) {
    setSmokeFan(false);
  } else {
    smoke_fan_pending_off = true;
    smoke_fan_off_delay_ms = delay_ms;
    smoke_fan_off_time_ms = millis() + delay_ms;
  }
}

void updateAllServos() {
  for (int i = 0; i < 3; i++) {
    gun_servos[i].update();
  }
}

// ============================================================================
//  FIRING CONTROL
// ============================================================================

void startFiring(int rpm) {
  if (rpm <= 0) return;
  is_firing = true;
  rate_of_fire_rpm = rpm;
  shot_interval_ms = 60000UL / rpm;
  next_shot_time_ms = millis();
  setSmokeFan(true);
}

void stopFiring(uint16_t fan_delay_ms) {
  is_firing = false;
  rate_of_fire_rpm = 0;
  setNozzleFlash(false);
  flash_active = false;
  scheduleSmokeFanOff(fan_delay_ms);
}

// ============================================================================
//  PERIODIC UPDATE FUNCTIONS
// ============================================================================

void updateMuzzleFlash() {
  if (!is_firing) {
    if (flash_active || flash_fading) {
      setNozzleFlash(false);
      flash_active = false;
      flash_fading = false;
      clearRecoilJerk();  // Clear recoil jerk when not firing
    }
    return;
  }
  
  uint32_t now = millis();
  
  // Handle fade-out
  if (flash_fading) {
    uint32_t fade_elapsed = now - fade_start_time_ms;
    if (fade_elapsed >= FLASH_FADE_MS) {
      // Fade complete - clear recoil jerk
      setNozzleFlash(false);
      flash_fading = false;
      clearRecoilJerk();
    } else {
      // Calculate current brightness (linear fade from 255 to 0)
      uint16_t brightness = map(fade_elapsed, 0, FLASH_FADE_MS, FLASH_PWM_DUTY, 0);
      analogWrite(PIN_NOZZLE_FLASH, (uint8_t)brightness);
    }
  }
  // Handle flash pulse
  else if (flash_active) {
    if (now >= flash_off_time_ms) {
      // Start fade-out
      flash_active = false;
      flash_fading = true;
      fade_start_time_ms = now;
    }
  }
  // Trigger new flash
  else if (!flash_fading && now >= next_shot_time_ms) {
    setNozzleFlash(true);
    flash_active = true;
    flash_off_time_ms = now + FLASH_PULSE_MS;
    next_shot_time_ms = now + shot_interval_ms;
    applyRecoilJerk();  // Apply recoil jerk on each shot
  }
}

void updateSmokeFan() {
  if (!smoke_fan_pending_off) return;
  
  uint32_t now = millis();
  if (now >= smoke_fan_off_time_ms) {
    setSmokeFan(false);
  }
}

GunFxStatus buildCurrentStatus(uint32_t now_ms) {
  GunFxStatus status;
  
  status.firing = is_firing;
  status.flashActive = flash_active;
  status.flashFading = flash_fading;
  status.heaterOn = smoke_heater_on;
  status.fanOn = smoke_fan_on;
  status.fanSpindown = smoke_fan_pending_off;

  // Fan off remaining time
  if (smoke_fan_pending_off && smoke_fan_off_time_ms > now_ms) {
    status.fanOffRemainingMs = (uint16_t)(smoke_fan_off_time_ms - now_ms);
  } else {
    status.fanOffRemainingMs = 0;
  }

  // Servo positions
  status.servoUs[0] = (uint16_t)constrain(gun_servos[0].position(), 0, 3000);
  status.servoUs[1] = (uint16_t)constrain(gun_servos[1].position(), 0, 3000);
  status.servoUs[2] = (uint16_t)constrain(gun_servos[2].position(), 0, 3000);

  // Rate of fire
  status.rateOfFireRpm = (uint16_t)rate_of_fire_rpm;

  return status;
}

void emitStatus(uint32_t now_ms) {
  GunFxStatus status = buildCurrentStatus(now_ms);
  serialSlave.sendStatus(status);
}

// ============================================================================
//  ARDUINO SETUP & LOOP
// ============================================================================

void setup() {
  Serial.begin(SERIAL_BAUD);
  while (!Serial && millis() < 3000) delay(10);
  
  // Initialize GPIO outputs
  pinMode(PIN_NOZZLE_FLASH, OUTPUT);
  pinMode(PIN_SMOKE_FAN, OUTPUT);
  pinMode(PIN_SMOKE_HEATER, OUTPUT);
  analogWrite(PIN_NOZZLE_FLASH, 0);
  digitalWrite(PIN_SMOKE_FAN, LOW);
  digitalWrite(PIN_SMOKE_HEATER, LOW);
  
  // Initialize status LEDs
  led_blue.begin(PIN_LED_BLUE);
  led_yellow.begin(PIN_LED_YELLOW);
  
  // Initialize servos
  const uint8_t servoPins[] = {PIN_GUN_SRV_1, PIN_GUN_SRV_2, PIN_GUN_SRV_3};
  for (int i = 0; i < 3; i++) {
    gun_servos[i].begin(servoPins[i], 500, 2500, SERVO_DEFAULT_US);
    gun_servos[i].setId(i + 1);
  }
  
  // Build unique device name from Pico flash ID
  buildDeviceName();
  
  // Initialize serial slave with callbacks
  serialSlave.begin(&Serial, deviceName);
  serialSlave.setConnectionTimeout(90000);
  serialSlave.onTriggerOn([](uint16_t rpm) { startFiring(rpm); });
  serialSlave.onTriggerOff([](uint16_t delay) { stopFiring(delay); });
  serialSlave.onServoSet([](uint8_t id, uint16_t us) { setServoPulse(id, us); });
  serialSlave.onServoSettings([](const GunFxServoConfig& cfg) {
    if (cfg.servoId == 0 || cfg.servoId > 3) return;
    uint8_t idx = cfg.servoId - 1;
    if (cfg.recoilJerkUs > 0 || cfg.recoilJerkVarianceUs > 0) {
      servo_jerk_configs[idx].jerk_us = cfg.recoilJerkUs;
      servo_jerk_configs[idx].variance_us = cfg.recoilJerkVarianceUs;
    } else {
      gun_servos[idx].setLimits(cfg.minUs, cfg.maxUs);
      gun_servos[idx].setMotionProfile(cfg.maxSpeedUsPerSec, cfg.maxAccelUsPerSec2, cfg.maxDecelUsPerSec2);
    }
  });
  serialSlave.onSmokeHeat(setSmokeHeater);
  serialSlave.onInit([]() { Serial.println("INFO: INIT"); performSafeInit(); });
  serialSlave.onShutdown([]() { Serial.println("INFO: SHUTDOWN"); performSafeShutdown(); });
  
  Serial.print("GunFX Pico Ready: ");
  Serial.println(deviceName);
}

void loop() {
  // Process incoming commands via serial_gunfx
  serialSlave.process();
  
  // Update hardware states
  updateMuzzleFlash();
  updateSmokeFan();
  updateAllServos();

  uint32_t now = millis();
  
  // Update status LEDs
  updateLEDs(now);
  
  // Check connection status (handles watchdog timeout)
  checkConnectionStatus();
  
  // Send periodic status
  if (now >= next_status_ms) {
    emitStatus(now);
    next_status_ms = now + STATUS_INTERVAL_MS;
  }
  
  // Small delay to prevent tight loop
  delay(1);
}
