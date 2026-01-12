/**
 * GunFX Pico Controller v0.2.0
 * 
 * Slave controller for gun effects - receives commands from HubFX over USB serial.
 * Controls: muzzle flash LED (PWM), smoke heater/fan, 3x gun servos with motion profiling.
 * 
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: Protocol negotiation via SerialInitHandler, then binary COBS or text mode
 * 
 * Architecture:
 *   - SerialInitHandler: Handles INIT/INIT_READY handshake + system commands (REBOOT, BOOTSEL)
 *   - IGunFxSlave*: Protocol-agnostic interface for GunFX commands (binary or text)
 */

#include <Arduino.h>
#include <Servo.h>
#include <math.h>
#include <serial.h>
#include <led_control.h>
#include <srv_control.h>
#include <pico/unique_id.h>

// Firmware version - used in INIT_READY response (no "v" prefix)
#define FIRMWARE_VERSION "0.2.0"
#define BUILD_NUMBER 10  // Increment this with each build

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

// Smoke fan PWM (defaults - can be configured via SMOKE_SETTINGS command)
const uint8_t SMOKE_FAN_DEFAULT_SPEED = 255;  // Default fan speed (0-255)
const uint8_t SMOKE_FAN_DEFAULT_PULSE_HIGH = 255;  // Default fan speed during shot pulse
const uint8_t SMOKE_FAN_DEFAULT_PULSE_LOW = 80;    // Default fan speed between shots
const uint16_t SMOKE_FAN_DEFAULT_PULSE_MS = 50;    // Default pulse duration
const uint16_t SMOKE_FAN_DEFAULT_SPINDOWN_MS = 5000; // Default spindown delay

// ============================================================================
//  STATE VARIABLES
// ============================================================================

// Serial communication - CommandRouter + handlers (Chain of Responsibility)
CommandRouter commandRouter;
SerialInitHandler initHandler;
GunFxSerialSlave binarySlave;
GunFxSerialSlaveText textSlave;
IGunFxSlave* activeSlave = nullptr;  // Points to binary or text implementation

char deviceName[24];  // "GunFX-XXXX" with 4-char unique suffix

// Connection timeout - fallback if keepalive not negotiated (3x default keepalive)
const unsigned long DEFAULT_CONNECTION_TIMEOUT_MS = 15000;

// Connection watchdog
bool watchdog_triggered = false;
bool was_master_connected = false;

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

// Smoke generator state
bool smoke_heater_on = false;
bool smoke_fan_on = false;
uint8_t smoke_fan_speed = SMOKE_FAN_DEFAULT_SPEED;  // Current fan PWM duty (0-255)
bool smoke_fan_pending_off = false;
uint32_t smoke_fan_off_time_ms = 0;

// Session metrics (cumulative since boot)
uint32_t total_shots_fired = 0;
uint32_t heater_on_start_ms = 0;      // When heater was turned on (0 if off)
uint32_t total_heater_on_time_ms = 0; // Accumulated heater on-time

// Smoke fan pulsing mode
enum class SmokeFanMode : uint8_t {
  Constant = 0,   // Constant speed when firing
  Pulsing  = 1    // Speed varies with RPM (pulse high on each shot)
};
SmokeFanMode smoke_fan_mode = SmokeFanMode::Constant;
bool smoke_fan_pulse_active = false;
uint32_t smoke_fan_pulse_end_ms = 0;

// Smoke fan configuration (runtime-configurable via SMOKE_SETTINGS)
uint8_t smoke_fan_cfg_speed = SMOKE_FAN_DEFAULT_SPEED;
uint8_t smoke_fan_cfg_pulse_high = SMOKE_FAN_DEFAULT_PULSE_HIGH;
uint8_t smoke_fan_cfg_pulse_low = SMOKE_FAN_DEFAULT_PULSE_LOW;
uint16_t smoke_fan_cfg_pulse_ms = SMOKE_FAN_DEFAULT_PULSE_MS;
uint16_t smoke_fan_cfg_spindown_ms = SMOKE_FAN_DEFAULT_SPINDOWN_MS;

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
//  FORWARD DECLARATIONS
// ============================================================================

// Smoke control functions (defined later, needed by performSafeShutdown)
void setSmokeHeater(bool on);
void setSmokeFan(bool on, uint8_t speed);
void setNozzleFlash(bool on);
void stopFiring(uint16_t fanDelayMs);
void setServoPulse(uint8_t servo_id, int pulse_us);

// ============================================================================
//  CONNECTION MANAGEMENT
// ============================================================================

/**
 * @brief Get the effective connection timeout based on negotiated keepalive
 * 
 * If keepalive was negotiated in INIT, uses 1.5x the interval.
 * Otherwise falls back to DEFAULT_CONNECTION_TIMEOUT_MS.
 */
unsigned long getConnectionTimeoutMs() {
  unsigned long negotiated = initHandler.keepaliveTimeoutMs();
  return negotiated > 0 ? negotiated : DEFAULT_CONNECTION_TIMEOUT_MS;
}

void checkConnectionStatus() {
  // Check timeout via init handler - callback handles the connection loss
  initHandler.checkTimeout(getConnectionTimeoutMs());
  
  // Check if we have an active slave and it's connected
  bool connected = activeSlave && activeSlave->isMasterConnected();
  
  // Detect connection loss (USB disconnect, etc.)
  if (was_master_connected && !connected) {
    if (!watchdog_triggered) {
      performSafeShutdown();
      watchdog_triggered = true;
    }
  }
  
  // Detect reconnection (handled by initHandler via onInitComplete)
  was_master_connected = connected;
}

void performSafeShutdown() {
  stopFiring(0);
  setSmokeHeater(false);
  setSmokeFan(false, 0);  // Turn off fan completely
  setNozzleFlash(false);
  
  // Reset servos to neutral position
  for (int i = 0; i < 3; i++) {
    setServoPulse(i + 1, SERVO_DEFAULT_US);
  }
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

/**
 * @brief Set up callbacks on the active slave (called after protocol negotiation)
 * 
 * Callbacks return error codes:
 *   - SerialError::OK (0x00) on success -> slave sends ACK
 *   - GunFxError codes for domain errors -> slave sends NACK with code
 *   - SerialError codes for generic errors -> slave sends NACK with code
 */
void setupSlaveCallbacks() {
  if (!activeSlave) return;
  
  activeSlave->setConnectionTimeout(getConnectionTimeoutMs());
  
  // TRIGGER_ON: Start firing at specified RPM
  activeSlave->onTriggerOn([](uint16_t rpm) -> uint8_t { 
    // Validate RPM range (1-3000)
    if (rpm < 1 || rpm > 3000) {
      return GunFxError::INVALID_RPM;
    }
    startFiring(rpm);
    return SerialError::OK;
  });
  
  // TRIGGER_OFF: Stop firing with optional fan delay
  activeSlave->onTriggerOff([](uint16_t delay) -> uint8_t { 
    stopFiring(delay);
    return SerialError::OK;
  });
  
  // SERVO_SET: Set servo position
  activeSlave->onServoSet([](uint8_t id, uint16_t us) -> uint8_t { 
    // Validate servo ID (1-3)
    if (id < 1 || id > 3) {
      return GunFxError::SERVO_INVALID_ID;
    }
    // Validate pulse width (500-2500µs)
    if (us < 500 || us > 2500) {
      return GunFxError::SERVO_PULSE_RANGE;
    }
    setServoPulse(id, us);
    return SerialError::OK;
  });
  
  // SERVO_SETTINGS: Configure servo motion profile
  activeSlave->onServoSettings([](const GunFxServoConfig& cfg) -> uint8_t {
    // Validate servo ID (1-3)
    if (cfg.servoId < 1 || cfg.servoId > 3) {
      return GunFxError::SERVO_INVALID_ID;
    }
    uint8_t idx = cfg.servoId - 1;
    
    // Validate pulse limits if provided
    if (cfg.minUs > 0 && (cfg.minUs < 500 || cfg.minUs > 2500)) {
      return GunFxError::SERVO_PULSE_RANGE;
    }
    if (cfg.maxUs > 0 && (cfg.maxUs < 500 || cfg.maxUs > 2500)) {
      return GunFxError::SERVO_PULSE_RANGE;
    }
    if (cfg.minUs > 0 && cfg.maxUs > 0 && cfg.minUs >= cfg.maxUs) {
      return GunFxError::SERVO_PULSE_RANGE;
    }
    
    // Apply recoil jerk settings
    if (cfg.recoilJerkUs > 0 || cfg.recoilJerkVarianceUs > 0) {
      servo_jerk_configs[idx].jerk_us = cfg.recoilJerkUs;
      servo_jerk_configs[idx].variance_us = cfg.recoilJerkVarianceUs;
    }
    
    // Apply motion profile settings
    if (cfg.minUs > 0 || cfg.maxUs > 0) {
      gun_servos[idx].setLimits(cfg.minUs, cfg.maxUs);
    }
    if (cfg.maxSpeedUsPerSec > 0 || cfg.maxAccelUsPerSec2 > 0 || cfg.maxDecelUsPerSec2 > 0) {
      gun_servos[idx].setMotionProfile(cfg.maxSpeedUsPerSec, cfg.maxAccelUsPerSec2, cfg.maxDecelUsPerSec2);
    }
    return SerialError::OK;
  });
  
  // SMOKE_HEAT: Enable/disable smoke heater
  activeSlave->onSmokeHeat([](bool on) -> uint8_t {
    setSmokeHeater(on);
    return SerialError::OK;
  });
  
  // SMOKE_SETTINGS: Configure smoke fan parameters
  activeSlave->onSmokeSettings([](const GunFxSmokeConfig& cfg) -> uint8_t {
    // Validate fan speeds (0-255 PWM range)
    // Note: 0 is technically valid (fan off)
    
    // Validate pulse timing (sanity check - max 10 seconds)
    if (cfg.fanPulseMs > 10000) {
      return GunFxError::INVALID_FAN_SPEED;
    }
    if (cfg.fanSpindownMs > 60000) {
      return GunFxError::INVALID_FAN_SPEED;
    }
    
    // Apply smoke fan settings
    smoke_fan_mode = cfg.fanPulsing ? SmokeFanMode::Pulsing : SmokeFanMode::Constant;
    smoke_fan_cfg_speed = cfg.fanSpeed;
    smoke_fan_cfg_pulse_high = cfg.fanPulseHigh;
    smoke_fan_cfg_pulse_low = cfg.fanPulseLow;
    smoke_fan_cfg_pulse_ms = cfg.fanPulseMs;
    smoke_fan_cfg_spindown_ms = cfg.fanSpindownMs;
    
    // If currently firing, update fan speed for new mode
    if (is_firing && smoke_fan_on) {
      if (cfg.fanPulsing) {
        analogWrite(PIN_SMOKE_FAN, smoke_fan_cfg_pulse_low);
        smoke_fan_speed = smoke_fan_cfg_pulse_low;
      } else {
        analogWrite(PIN_SMOKE_FAN, smoke_fan_cfg_speed);
        smoke_fan_speed = smoke_fan_cfg_speed;
      }
    }
    return SerialError::OK;
  });
  
  // STATUS_REQ: Master requests current status/metrics
  activeSlave->onStatusRequest([]() -> GunFxStatus {
    return buildCurrentStatus(millis());
  });
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
  // Track heater on-time for metrics
  if (on && !smoke_heater_on) {
    // Heater turning on - start tracking
    heater_on_start_ms = millis();
  } else if (!on && smoke_heater_on && heater_on_start_ms > 0) {
    // Heater turning off - accumulate time
    total_heater_on_time_ms += millis() - heater_on_start_ms;
    heater_on_start_ms = 0;
  }
  
  smoke_heater_on = on;
  digitalWrite(PIN_SMOKE_HEATER, on ? HIGH : LOW);
}

void setSmokeFan(bool on, uint8_t speed = SMOKE_FAN_DEFAULT_SPEED) {
  smoke_fan_on = on;
  smoke_fan_speed = speed;
  smoke_fan_pending_off = false;
  smoke_fan_pulse_active = false;
  analogWrite(PIN_SMOKE_FAN, on ? speed : 0);
}

/**
 * @brief Set smoke fan operating mode
 * @param pulsing true for pulsing mode (varies with RPM), false for constant speed
 */
void setSmokeFanMode(bool pulsing) {
  smoke_fan_mode = pulsing ? SmokeFanMode::Pulsing : SmokeFanMode::Constant;
  
  // If currently firing, update fan speed immediately
  if (is_firing && smoke_fan_on) {
    if (pulsing) {
      // Switch to low speed (will pulse on shots)
      analogWrite(PIN_SMOKE_FAN, smoke_fan_cfg_pulse_low);
      smoke_fan_speed = smoke_fan_cfg_pulse_low;
    } else {
      // Switch to constant high speed
      analogWrite(PIN_SMOKE_FAN, smoke_fan_cfg_speed);
      smoke_fan_speed = smoke_fan_cfg_speed;
    }
  }
}

void scheduleSmokeFanOff(uint16_t delay_ms) {
  if (delay_ms == 0) {
    setSmokeFan(false);
  } else {
    smoke_fan_pending_off = true;
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
  
  // Start fan - in pulsing mode, start at low speed (will pulse high on shots)
  if (smoke_fan_mode == SmokeFanMode::Pulsing) {
    setSmokeFan(true, smoke_fan_cfg_pulse_low);
  } else {
    setSmokeFan(true, smoke_fan_cfg_speed);
  }
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
    triggerSmokeFanPulse();  // Pulse smoke fan on each shot (if in pulsing mode)
    total_shots_fired++;  // Increment shot counter for metrics
  }
}

void updateSmokeFan() {
  // Handle pending off (spindown delay after firing stops)
  if (smoke_fan_pending_off) {
    uint32_t now = millis();
    if (now >= smoke_fan_off_time_ms) {
      setSmokeFan(false);
    }
    return;
  }
  
  // Handle pulsing mode - return to low speed after pulse
  if (smoke_fan_mode == SmokeFanMode::Pulsing && smoke_fan_pulse_active) {
    uint32_t now = millis();
    if (now >= smoke_fan_pulse_end_ms) {
      smoke_fan_pulse_active = false;
      if (smoke_fan_on && is_firing) {
        analogWrite(PIN_SMOKE_FAN, smoke_fan_cfg_pulse_low);
        smoke_fan_speed = smoke_fan_cfg_pulse_low;
      }
    }
  }
}

/**
 * @brief Trigger a smoke fan pulse (called on each shot in pulsing mode)
 */
void triggerSmokeFanPulse() {
  if (smoke_fan_mode != SmokeFanMode::Pulsing || !smoke_fan_on) return;
  
  smoke_fan_pulse_active = true;
  smoke_fan_pulse_end_ms = millis() + smoke_fan_cfg_pulse_ms;
  analogWrite(PIN_SMOKE_FAN, smoke_fan_cfg_pulse_high);
  smoke_fan_speed = smoke_fan_cfg_pulse_high;
}

GunFxStatus buildCurrentStatus(uint32_t now_ms) {
  GunFxStatus status;
  
  // Operational state
  status.firing = is_firing;
  status.flashActive = flash_active;
  status.flashFading = flash_fading;
  status.heaterOn = smoke_heater_on;
  status.fanOn = smoke_fan_on;
  status.fanSpindown = smoke_fan_pending_off;
  status.fanSpeed = smoke_fan_speed;

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
  
  // Session metrics
  status.shotsFired = total_shots_fired;
  
  // Calculate total heater on-time (including current session if heater is on)
  status.heaterOnTimeMs = total_heater_on_time_ms;
  if (smoke_heater_on && heater_on_start_ms > 0) {
    status.heaterOnTimeMs += now_ms - heater_on_start_ms;
  }
  
  // System health
  status.uptimeMs = now_ms;
  status.freeRam = rp2040.getFreeHeap();

  return status;
}

// ============================================================================
//  ARDUINO SETUP & LOOP
// ============================================================================

void setup() {
  // Initialize USB serial for protocol communication
  Serial.begin(SERIAL_BAUD);
  while (!Serial && millis() < 3000) delay(10);
  
  // Initialize GPIO outputs
  pinMode(PIN_NOZZLE_FLASH, OUTPUT);
  pinMode(PIN_SMOKE_FAN, OUTPUT);
  pinMode(PIN_SMOKE_HEATER, OUTPUT);
  analogWrite(PIN_NOZZLE_FLASH, 0);
  analogWrite(PIN_SMOKE_FAN, 0);
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
  
  // ========================================================================
  // Initialize CommandRouter (Chain of Responsibility pattern)
  // ========================================================================
  // The router reads serial input and passes commands through handlers in order.
  // First handler to recognize a command processes it; others pass it along.
  // If no handler recognizes a command, the router sends NACK.
  
  commandRouter.begin(&Serial, [](uint8_t code, const char* cmd) {
    // Send NACK for unrecognized commands
    if (activeSlave) {
      activeSlave->sendNack(code, cmd);
    }
  });
  
  // ========================================================================
  // Initialize SerialInitHandler for system commands
  // ========================================================================
  initHandler.begin(&Serial, deviceName);
  initHandler.setBoardInfo(FIRMWARE_VERSION, BUILD_NUMBER, "RP2040", F_CPU / 1000000, rp2040.getFreeHeap());
  
  // Add to command chain (first - system commands take priority)
  commandRouter.addHandler(&initHandler);
  
  // Protocol negotiation complete - switch to appropriate protocol handler
  initHandler.onInitComplete([](ProtocolMode mode) {
    
    // Clean up previous slave if reconnecting
    if (activeSlave) {
      activeSlave->end();
      activeSlave = nullptr;
    }
    
    // Reset router handlers (keep initHandler first)
    commandRouter.clearHandlers();
    commandRouter.addHandler(&initHandler);
    
    // Switch router to negotiated protocol mode
    commandRouter.setMode(mode);
    
    // Initialize appropriate protocol handler and add to router
    if (mode == ProtocolMode::Binary) {
      binarySlave.begin(&Serial, deviceName);
      activeSlave = &binarySlave;
      commandRouter.addHandler(&binarySlave);
    } else {
      textSlave.begin(&Serial, deviceName);
      activeSlave = &textSlave;
      commandRouter.addHandler(&textSlave);
    }
    
    // Set up GunFX command callbacks
    setupSlaveCallbacks();
    
    // Perform safe initialization
    performSafeInit();
  });
  
  // Handle reconnection (new INIT received)
  initHandler.onInitReset([]() {
    performSafeShutdown();
    if (activeSlave) {
      activeSlave->end();
      activeSlave = nullptr;
    }
  });
  
  // System commands handled by SerialInitHandler
  initHandler.onShutdown([]() {
    performSafeShutdown();
  });
  
  initHandler.onReboot([]() {
    performSafeShutdown();
    delay(100);
    rp2040.reboot();
  });
  
  initHandler.onBootsel([]() {
    performSafeShutdown();
    delay(500);
    rp2040.rebootToBootloader();
  });

  // Connection loss detection (keepalive timeout)
  initHandler.onConnectionLoss([]() {
    // Custom connection loss handling
    // performSafeShutdown() is a reasonable default, but this callback
    // allows for custom behavior like LED indication, logging, etc.
    if (!watchdog_triggered) {
      performSafeShutdown();
      watchdog_triggered = true;
    }
  });
}

void loop() {
  // ========================================================================
  // Process commands via CommandRouter (Chain of Responsibility)
  // ========================================================================
  // Router handles both text and binary protocols based on current mode.
  // Handlers are tried in order:
  // 1. initHandler - INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE, STATUS_REQ
  // 2. activeSlave - TRIGGER_ON, SERVO_SET, etc.
  // If no handler processes a command, router sends NACK.
  
  commandRouter.process();
  
  // Update activity timestamp for timeout detection
  if (commandRouter.lastActivityMs() > initHandler.lastActivityMs()) {
    initHandler.updateActivity();
  }
  
  // Update hardware states
  updateMuzzleFlash();
  updateSmokeFan();
  updateAllServos();

  uint32_t now = millis();
  
  // Update status LEDs
  updateLEDs(now);
  
  // Check connection status (handles watchdog timeout)
  checkConnectionStatus();
  
  // Small delay to prevent tight loop
  delay(1);
}
