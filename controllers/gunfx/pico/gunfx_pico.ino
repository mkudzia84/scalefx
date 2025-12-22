/*
  GunFX Controller for Raspberry Pi Pico
  
  Slave controller that receives commands from main Raspberry Pi over USB serial
  and controls gun FX hardware:
  - Nozzle flash LED (PWM)
  - Smoke heater and fan
  - Gun servos (srv_1, srv_2, srv_3)
  
  Binary Serial Protocol (115200 baud, COBS framed, delimiter 0x00):
  Packet: [type:u8][len:u8][payload:len][crc8(0x07)]; frame encoded with COBS, terminated by 0x00
  Commands (Pi -> Pico):
    0x01 TRIGGER_ON    payload: rpm:u16le
    0x02 TRIGGER_OFF   payload: fan_delay_ms:u16le
    0x10 SRV_SET       payload: servo_id:u8, pulse_us:u16le
    0x11 SRV_SETTINGS  payload: servo_id:u8, min:u16le, max:u16le, max_speed:u16le, accel:u16le, decel:u16le
    0x12 SRV_RECOIL_JERK payload: servo_id:u8, jerk_us:u16le, variance_us:u16le
    0x20 SMOKE_HEAT    payload: on:u8 (0=off,1=on)
    0xF0 INIT          payload: none (daemon initialization - reset to safe state)
    0xF1 SHUTDOWN      payload: none (daemon shutdown - enter safe state)
    0xF2 KEEPALIVE     payload: none (periodic keepalive from daemon)
  Telemetry (Pico -> Pi):
    0xF3 INIT_READY    payload: module_name:string (sent in response to INIT)
    0xF4 STATUS        payload: flags:u8 (bit0=firing, bit1=flash_active, bit2=flash_fading, bit3=heater_on, bit4=fan_on, bit5=fan_spindown),
                                  fan_off_remaining_ms:u16le, servo_us[3]:u16le each, rate_of_fire_rpm:u16le
*/

#include <Arduino.h>
#include <Servo.h>
#include <math.h>

// ---------------------- Pin Configuration (Right Side of Board) ----------------------
// Raspberry Pi Pico Pinout - All outputs on right side, servos consecutive
// Right side reference: Pins 28 (GP19), 30 (GP20), 32 (GP21), 34 (GP22), 36 (GP23), 40 (GP25)
//
// Pin layout:     Left (1-20)         Right (21-40)
//                 ──────────          ───────────
//                 ...                 ...
//                 ...                 GP16 · GP17 (21/22)← Heater / Fan
//                 ...                 GND ·  GP19 (28)  ← PIN_GUN_SRV_1
//                 ...                 GND ·  GP20 (30)  ← PIN_GUN_SRV_2
//                 ...                 GND ·  GP21 (32)  ← PIN_GUN_SRV_3
//                 ...                 GND ·  GP22 (34)  ← PIN_NOZZLE_FLASH
//                 ...                 ...  ·  GP23 (36)
//                 ...                 GND ·  GP25 (40)  ← NC

// Gun Servo Outputs (PWM) - Consecutive on right side
const uint8_t PIN_GUN_SRV_1       = 19; // GP19 (Physical Pin 28)
const uint8_t PIN_GUN_SRV_2       = 20; // GP20 (Physical Pin 30)
const uint8_t PIN_GUN_SRV_3       = 21; // GP21 (Physical Pin 32)

// Smoke and Flash Outputs - Continuing on right side
const uint8_t PIN_NOZZLE_FLASH    = 22; // GP22 (Physical Pin 34) - PWM LED
const uint8_t PIN_SMOKE_HEATER    = 16; // GP16 (Physical Pin 21) - Digital out
const uint8_t PIN_SMOKE_FAN       = 17; // GP17 (Physical Pin 22) - Digital out

// ---------------------- Constants ----------------------
const uint32_t SERIAL_BAUD        = 115200;
const uint8_t FLASH_PWM_DUTY      = 255;      // Full brightness during flash
const uint16_t FLASH_PULSE_MS     = 30;       // Flash duration per shot
const uint16_t FLASH_FADE_MS      = 80;       // Fade-out duration after flash
const uint8_t FLASH_FADE_STEPS    = 20;       // Number of steps in fade-out
const uint16_t SERVO_DEFAULT_US   = 1500;     // Servo center position
const int SERVO_DEFAULT_MAX_SPEED = 4000;     // us per second
const int SERVO_DEFAULT_ACCEL     = 8000;     // us per second^2
const int SERVO_DEFAULT_DECEL     = 8000;     // us per second^2
const uint32_t STATUS_INTERVAL_MS = 1000;     // Periodic status interval

// ---------------------- Binary Protocol (COBS framed, delimiter 0x00) ----------------------
//  Packet format (before COBS): [type:u8][len:u8][payload:len bytes][crc:u8]
//  CRC-8 polynomial 0x07 over type+len+payload
//  Command types (Pi -> Pico)
//  0x01: TRIGGER_ON    payload: rpm:u16le
//  0x02: TRIGGER_OFF   payload: fan_delay_ms:u16le
//  0x10: SRV_SET       payload: servo_id:u8, pulse_us:u16le
//  0x11: SRV_SETTINGS  payload: servo_id:u8, min:u16le, max:u16le, max_speed:u16le, accel:u16le, decel:u16le
//  0x12: SRV_RECOIL_JERK payload: servo_id:u8, jerk_us:u16le, variance_us:u16le
//  0x20: SMOKE_HEAT    payload: on:u8 (0=off,1=on)
//  0xF0: INIT          payload: none (daemon initialization - reset to safe state)
//  0xF1: SHUTDOWN      payload: none (daemon shutdown - enter safe state)
//  0xF2: KEEPALIVE     payload: none (periodic keepalive from daemon)
//  Telemetry (Pico -> Pi):
//  0xF3: INIT_READY    payload: module_name:string (sent in response to INIT)
//  0xF4: STATUS        payload: flags:u8, fan_off_remaining_ms:u16le, servo_us[3]:u16le each, rate_of_fire_rpm:u16le

const uint8_t PKT_TRIGGER_ON      = 0x01;
const uint8_t PKT_TRIGGER_OFF     = 0x02;
const uint8_t PKT_SRV_SET         = 0x10;
const uint8_t PKT_SRV_SETTINGS    = 0x11;
const uint8_t PKT_SRV_RECOIL_JERK = 0x12;
const uint8_t PKT_SMOKE_HEAT      = 0x20;

// Universal protocol packets (high values, used across all modules)
const uint8_t PKT_INIT            = 0xF0;
const uint8_t PKT_SHUTDOWN        = 0xF1;
const uint8_t PKT_KEEPALIVE       = 0xF2;
const uint8_t PKT_INIT_READY      = 0xF3;
const uint8_t PKT_STATUS          = 0xF4;

const size_t  MAX_PAYLOAD_SIZE    = 32;
const size_t  MAX_PACKET_SIZE     = 2 /*type+len*/ + MAX_PAYLOAD_SIZE + 1 /*crc*/;
const size_t  COBS_BUFFER_SIZE    = MAX_PACKET_SIZE + MAX_PACKET_SIZE / 254 + 1; // worst-case COBS growth

// ---------------------- State Variables ----------------------
// Firing state
bool is_firing = false;
int rate_of_fire_rpm = 0;
uint32_t shot_interval_ms = 0;
uint32_t next_shot_time_ms = 0;

// Flash state
bool flash_active = false;
uint32_t flash_off_time_ms = 0;
bool flash_fading = false;
uint32_t fade_start_time_ms = 0;
uint8_t fade_step_duration_ms = 0;

// Smoke fan delayed off
bool smoke_fan_on = false;
bool smoke_fan_pending_off = false;
uint32_t smoke_fan_off_time_ms = 0;
uint16_t smoke_fan_off_delay_ms = 0;

// Smoke heater
bool smoke_heater_on = false;

// Gun Servos
Servo gun_srv_1;
Servo gun_srv_2;
Servo gun_srv_3;

struct ServoConfig {
  Servo* servo;
  uint8_t id;
  int min_us;
  int max_us;
  int max_speed;      // Stored for future motion profiling
  int acceleration;   // Stored for future motion profiling
  int deceleration;   // Stored for future motion profiling
  int recoil_jerk_us; // Recoil jerk offset per shot (0=disabled)
  int recoil_jerk_variance_us; // Random variance for recoil jerk
};

ServoConfig servo_configs[] = {
  { &gun_srv_1, 1, 500, 2500, 0, 0, 0, 0, 0 },
  { &gun_srv_2, 2, 500, 2500, 0, 0, 0, 0, 0 },
  { &gun_srv_3, 3, 500, 2500, 0, 0, 0, 0, 0 }
};

struct ServoMotionState {
  float position_us;
  float target_us;
  float velocity_us_per_s;
  uint32_t last_update_ms;
  float recoil_jerk_offset;  // Active recoil jerk offset (applied during shot)
};

ServoMotionState servo_motion[] = {
  { (float)SERVO_DEFAULT_US, (float)SERVO_DEFAULT_US, 0.0f, 0, 0.0f },
  { (float)SERVO_DEFAULT_US, (float)SERVO_DEFAULT_US, 0.0f, 0, 0.0f },
  { (float)SERVO_DEFAULT_US, (float)SERVO_DEFAULT_US, 0.0f, 0, 0.0f }
};

uint32_t next_status_ms = 0;

// Watchdog state
const uint32_t WATCHDOG_TIMEOUT_MS = 90000; // 90 seconds
uint32_t last_keepalive_ms = 0;
bool watchdog_active = false;

// RX buffer for binary protocol
uint8_t rx_buffer[COBS_BUFFER_SIZE];
size_t rx_index = 0;

// ---------------------- Protocol Helpers ----------------------

uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// COBS decode: returns decoded length, 0 on error
size_t cobsDecode(const uint8_t *input, size_t length, uint8_t *output) {
  size_t read_index = 0;
  size_t write_index = 0;

  while (read_index < length) {
    uint8_t code = input[read_index++];
    if (code == 0 || read_index + code - 1 > length) {
      return 0; // invalid
    }
    for (uint8_t i = 1; i < code; i++) {
      if (write_index >= MAX_PACKET_SIZE) return 0;
      if (read_index >= length) return 0;
      output[write_index++] = input[read_index++];
    }
    if (code < 0xFF && read_index < length) {
      if (write_index >= MAX_PACKET_SIZE) return 0;
      output[write_index++] = 0x00;
    }
  }
  return write_index;
}

// COBS encode: returns encoded length
size_t cobsEncode(const uint8_t *input, size_t length, uint8_t *output) {
  size_t read_index = 0;
  size_t write_index = 1; // first code byte reserved
  size_t code_index = 0;
  uint8_t code = 1;

  while (read_index < length) {
    if (input[read_index] == 0) {
      output[code_index] = code;
      code_index = write_index++;
      code = 1;
      read_index++;
    } else {
      output[write_index++] = input[read_index++];
      code++;
      if (code == 0xFF) {
        output[code_index] = code;
        code_index = write_index++;
        code = 1;
      }
    }
  }
  output[code_index] = code;
  return write_index;
}

// ---------------------- Protocol Packet Helpers ----------------------
// Modular packet building and sending functions to reduce code duplication

void sendPacket(uint8_t type, const uint8_t* payload_data, size_t payload_len) {
  uint8_t packet[MAX_PACKET_SIZE];
  size_t idx = 0;
  packet[idx++] = type;
  packet[idx++] = (uint8_t)payload_len;
  if (payload_len > 0 && payload_data != nullptr) {
    memcpy(&packet[idx], payload_data, payload_len);
    idx += payload_len;
  }
  packet[idx++] = crc8(packet, idx);
  
  uint8_t encoded[COBS_BUFFER_SIZE];
  size_t enc_len = cobsEncode(packet, idx, encoded);
  encoded[enc_len++] = 0x00;
  Serial.write(encoded, enc_len);
}

void sendInitReady() {
  const char* module_name = "MSB GunFX";
  sendPacket(PKT_INIT_READY, (const uint8_t*)module_name, strlen(module_name));
}

// ---------------------- Watchdog Management ----------------------

void watchdogReset() {
  last_keepalive_ms = millis();
  watchdog_active = true;
}

void watchdogDisable() {
  watchdog_active = false;
}

void watchdogCheck(uint32_t now_ms) {
  if (!watchdog_active) return;
  
  if (now_ms - last_keepalive_ms > WATCHDOG_TIMEOUT_MS) {
    Serial.println("WARN: Watchdog timeout - performing shutdown");
    performSafeShutdown();
    watchdogDisable();
  }
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
  // Reset servos to center
  for (int i = 0; i < 3; i++) {
    setServoPulse(i + 1, SERVO_DEFAULT_US);
  }
}

// ---------------------- Servo Helper Functions ----------------------

ServoConfig* getServoConfig(uint8_t servo_id) {
  if (servo_id == 0 || servo_id > (sizeof(servo_configs) / sizeof(ServoConfig))) {
    return nullptr;
  }
  return &servo_configs[servo_id - 1];
}

ServoMotionState* getServoMotion(uint8_t servo_id) {
  if (servo_id == 0 || servo_id > (sizeof(servo_motion) / sizeof(ServoMotionState))) {
    return nullptr;
  }
  return &servo_motion[servo_id - 1];
}

int effectiveMaxSpeed(const ServoConfig* cfg) {
  return (cfg->max_speed > 0) ? cfg->max_speed : SERVO_DEFAULT_MAX_SPEED;
}

int effectiveAccel(const ServoConfig* cfg) {
  return (cfg->acceleration > 0) ? cfg->acceleration : SERVO_DEFAULT_ACCEL;
}

int effectiveDecel(const ServoConfig* cfg) {
  return (cfg->deceleration > 0) ? cfg->deceleration : SERVO_DEFAULT_DECEL;
}

float approachZero(float value, float delta) {
  if (value > 0.0f) {
    value -= delta;
    if (value < 0.0f) value = 0.0f;
  } else if (value < 0.0f) {
    value += delta;
    if (value > 0.0f) value = 0.0f;
  }
  return value;
}

// ---------------------- Servo Control ----------------------

void setServoPulse(uint8_t servo_id, int pulse_us) {
  ServoConfig* cfg = getServoConfig(servo_id);
  ServoMotionState* motion = getServoMotion(servo_id);
  if (!cfg || !motion) {
    Serial.println("ERROR: Invalid servo id");
    return;
  }

  if (pulse_us < 300 || pulse_us > 2700) {
    Serial.println("ERROR: Invalid pulse width");
    return;
  }

  int target = constrain(pulse_us, cfg->min_us, cfg->max_us);
  motion->target_us = (float)target;

  Serial.print("Servo ");
  Serial.print(servo_id);
  Serial.print(" target=");
  Serial.print(pulse_us);
  Serial.print("us clamped=");
  Serial.print(target);
  Serial.print(" limits[");
  Serial.print(cfg->min_us);
  Serial.print(",");
  Serial.print(cfg->max_us);
  Serial.println("]");
}

void setServoSettings(uint8_t servo_id, int min_limit, int max_limit, int max_speed, int acceleration, int deceleration) {
  ServoConfig* cfg = getServoConfig(servo_id);
  ServoMotionState* motion = getServoMotion(servo_id);
  if (!cfg) {
    Serial.println("ERROR: Invalid servo id");
    return;
  }

  if (min_limit < 300 || max_limit > 2700 || min_limit >= max_limit) {
    Serial.println("ERROR: Invalid servo limits");
    return;
  }

  if (max_speed <= 0 || acceleration <= 0 || deceleration <= 0) {
    Serial.println("ERROR: Speed/accel/decel must be > 0");
    return;
  }

  cfg->min_us = min_limit;
  cfg->max_us = max_limit;
  cfg->max_speed = max_speed;
  cfg->acceleration = acceleration;
  cfg->deceleration = deceleration;

  if (motion) {
    motion->target_us = constrain(motion->target_us, (float)cfg->min_us, (float)cfg->max_us);
    motion->position_us = constrain(motion->position_us, (float)cfg->min_us, (float)cfg->max_us);
    cfg->servo->writeMicroseconds((int)motion->position_us);
  }

  Serial.print("Servo ");
  Serial.print(servo_id);
  Serial.print(" settings -> limits[");
  Serial.print(cfg->min_us);
  Serial.print(",");
  Serial.print(cfg->max_us);
  Serial.print("] max_speed=");
  Serial.print(cfg->max_speed);
  Serial.print(" accel=");
  Serial.print(cfg->acceleration);
  Serial.print(" decel=");
  Serial.print(cfg->deceleration);
  Serial.print(" recoil_jerk=");
  Serial.print(cfg->recoil_jerk_us);
  Serial.print(" +/-");
  Serial.println(cfg->recoil_jerk_variance_us);
}

void setServoRecoilJerk(uint8_t servo_id, int jerk_us, int variance_us) {
  ServoConfig* cfg = getServoConfig(servo_id);
  if (!cfg) {
    Serial.println("ERROR: Invalid servo id");
    return;
  }

  cfg->recoil_jerk_us = jerk_us;
  cfg->recoil_jerk_variance_us = variance_us;

  Serial.print("Servo ");
  Serial.print(servo_id);
  Serial.print(" recoil jerk -> ");
  Serial.print(jerk_us);
  Serial.print(" +/-");
  Serial.println(variance_us);
}

// Apply recoil jerk to all servos (called on each shot)
void applyRecoilJerk() {
  for (size_t i = 0; i < (sizeof(servo_configs) / sizeof(ServoConfig)); i++) {
    ServoConfig* cfg = &servo_configs[i];
    ServoMotionState* motion = &servo_motion[i];
    
    if (cfg->recoil_jerk_us == 0) {
      motion->recoil_jerk_offset = 0.0f;
      continue;
    }
    
    // Calculate random direction (+/-)
    int direction = (random(2) == 0) ? 1 : -1;
    
    // Calculate random variance (0 to variance_us)
    int variance = 0;
    if (cfg->recoil_jerk_variance_us > 0) {
      variance = random(cfg->recoil_jerk_variance_us + 1);
    }
    
    // Total jerk = direction * (base_jerk + random_variance)
    motion->recoil_jerk_offset = (float)(direction * (cfg->recoil_jerk_us + variance));
    
    Serial.print("Servo ");
    Serial.print(i + 1);
    Serial.print(" recoil jerk applied: ");
    Serial.println(motion->recoil_jerk_offset);
  }
}

// Clear recoil jerk offsets (called after flash ends)
void clearRecoilJerk() {
  for (size_t i = 0; i < (sizeof(servo_motion) / sizeof(ServoMotionState)); i++) {
    servo_motion[i].recoil_jerk_offset = 0.0f;
  }
}

// ---------------------- Hardware Output Control ----------------------

void setNozzleFlash(bool on) {
  if (on) {
    analogWrite(PIN_NOZZLE_FLASH, FLASH_PWM_DUTY);
  } else {
    analogWrite(PIN_NOZZLE_FLASH, 0);
  }
}

void setSmokeHeater(bool on) {
  smoke_heater_on = on;
  digitalWrite(PIN_SMOKE_HEATER, on ? HIGH : LOW);
  Serial.print("Smoke heater: ");
  Serial.println(on ? "ON" : "OFF");
}

void setSmokeFan(bool on) {
  smoke_fan_on = on;
  smoke_fan_pending_off = false;
  digitalWrite(PIN_SMOKE_FAN, on ? HIGH : LOW);
  Serial.print("Smoke fan: ");
  Serial.println(on ? "ON" : "OFF");
}

void scheduleSmokeFanOff(uint16_t delay_ms) {
  if (delay_ms == 0) {
    setSmokeFan(false);
  } else {
    smoke_fan_pending_off = true;
    smoke_fan_off_delay_ms = delay_ms;
    smoke_fan_off_time_ms = millis() + delay_ms;
    Serial.print("Smoke fan scheduled OFF in ");
    Serial.print(delay_ms);
    Serial.println(" ms");
  }
}

void updateServoMotion(ServoConfig* cfg, ServoMotionState* motion, uint32_t now_ms) {
  if (!cfg || !motion) return;

  if (motion->last_update_ms == 0) {
    motion->last_update_ms = now_ms;
    return;
  }

  uint32_t dt_ms = now_ms - motion->last_update_ms;
  if (dt_ms == 0) return;
  motion->last_update_ms = now_ms;

  float dt_s = dt_ms / 1000.0f;
  float max_speed = (float)effectiveMaxSpeed(cfg);
  float accel = (float)effectiveAccel(cfg);
  float decel = (float)effectiveDecel(cfg);

  // Clamp target to limits continuously in case limits changed
  if (motion->target_us < cfg->min_us) motion->target_us = (float)cfg->min_us;
  if (motion->target_us > cfg->max_us) motion->target_us = (float)cfg->max_us;

  float dist = motion->target_us - motion->position_us;
  int dir = (dist > 0.5f) ? 1 : (dist < -0.5f ? -1 : 0);

  // If effectively at target and stopped, snap and exit
  if (dir == 0 && fabs(motion->velocity_us_per_s) < 1.0f) {
    motion->position_us = motion->target_us;
    motion->velocity_us_per_s = 0.0f;
    cfg->servo->writeMicroseconds((int)motion->position_us);
    return;
  }

  float stop_dist = (decel > 0.0f) ? ((motion->velocity_us_per_s * motion->velocity_us_per_s) / (2.0f * decel)) : 0.0f;
  bool moving_toward = (motion->velocity_us_per_s * dist) > 0.0f;

  if (motion->velocity_us_per_s == 0.0f) {
    // Start accelerating toward target
    motion->velocity_us_per_s += dir * accel * dt_s;
  } else if (moving_toward) {
    // If close enough that we must stop, decelerate; else accelerate up to max
    if (fabs(dist) <= stop_dist) {
      motion->velocity_us_per_s = approachZero(motion->velocity_us_per_s, decel * dt_s);
    } else {
      motion->velocity_us_per_s += dir * accel * dt_s;
    }
  } else {
    // Moving away from target: decelerate to zero first
    motion->velocity_us_per_s = approachZero(motion->velocity_us_per_s, decel * dt_s);
    // Acceleration in the new direction begins after velocity crosses zero (next iterations)
  }

  // Cap speed
  if (motion->velocity_us_per_s > max_speed) motion->velocity_us_per_s = max_speed;
  if (motion->velocity_us_per_s < -max_speed) motion->velocity_us_per_s = -max_speed;

  // Integrate position
  motion->position_us += motion->velocity_us_per_s * dt_s;

  // Prevent overshoot past target when moving toward it
  if (dir > 0 && motion->position_us > motion->target_us) {
    motion->position_us = motion->target_us;
    motion->velocity_us_per_s = 0.0f;
  } else if (dir < 0 && motion->position_us < motion->target_us) {
    motion->position_us = motion->target_us;
    motion->velocity_us_per_s = 0.0f;
  }

  // Enforce limits
  if (motion->position_us < cfg->min_us) {
    motion->position_us = (float)cfg->min_us;
    motion->velocity_us_per_s = 0.0f;
  }
  if (motion->position_us > cfg->max_us) {
    motion->position_us = (float)cfg->max_us;
    motion->velocity_us_per_s = 0.0f;
  }

  // Apply recoil jerk offset to final servo output (only during shot)
  float output_us = motion->position_us + motion->recoil_jerk_offset;
  
  // Clamp output to servo limits
  if (output_us < cfg->min_us) output_us = (float)cfg->min_us;
  if (output_us > cfg->max_us) output_us = (float)cfg->max_us;

  cfg->servo->writeMicroseconds((int)output_us);
}

void updateAllServos() {
  uint32_t now_ms = millis();
  for (size_t i = 0; i < (sizeof(servo_configs) / sizeof(ServoConfig)); i++) {
    updateServoMotion(&servo_configs[i], &servo_motion[i], now_ms);
  }
}

// ---------------------- Firing Control ----------------------

void startFiring(int rpm) {
  if (rpm <= 0) {
    Serial.println("ERROR: Invalid RPM");
    return;
  }
  
  is_firing = true;
  rate_of_fire_rpm = rpm;
  shot_interval_ms = (60000UL / rpm);
  next_shot_time_ms = millis();
  
  // Turn on smoke fan immediately
  setSmokeFan(true);
  
  Serial.print("Firing started: ");
  Serial.print(rpm);
  Serial.print(" RPM (interval: ");
  Serial.print(shot_interval_ms);
  Serial.println(" ms)");
}

void stopFiring(uint16_t fan_delay_ms) {
  is_firing = false;
  rate_of_fire_rpm = 0;
  
  // Turn off flash immediately
  setNozzleFlash(false);
  flash_active = false;
  
  // Schedule fan off with delay
  scheduleSmokeFanOff(fan_delay_ms);
  
  Serial.print("Firing stopped (fan delay: ");
  Serial.print(fan_delay_ms);
  Serial.println(" ms)");
}

// ---------------------- Packet Processing ----------------------

void processPacket(const uint8_t *payload, size_t len) {
  if (len < 2) return; // need at least type + len
  uint8_t type = payload[0];
  uint8_t plen = payload[1];
  if (plen + 2 != len) {
    Serial.println("ERROR: Length mismatch");
    return;
  }

  const uint8_t *body = payload + 2;

  switch (type) {
    case PKT_TRIGGER_ON: {
      if (plen != 2) { Serial.println("ERROR: TRIGGER_ON len"); break; }
      uint16_t rpm = (uint16_t)(body[0] | (body[1] << 8));
      startFiring((int)rpm);
      break;
    }
    case PKT_TRIGGER_OFF: {
      if (plen != 2) { Serial.println("ERROR: TRIGGER_OFF len"); break; }
      uint16_t delay_ms = (uint16_t)(body[0] | (body[1] << 8));
      stopFiring(delay_ms);
      break;
    }
    case PKT_SRV_SET: {
      if (plen != 3) { Serial.println("ERROR: SRV_SET len"); break; }
      uint8_t sid = body[0];
      uint16_t pulse = (uint16_t)(body[1] | (body[2] << 8));
      setServoPulse(sid, (int)pulse);
      break;
    }
    case PKT_SRV_SETTINGS: {
      if (plen != 11) { Serial.println("ERROR: SRV_SETTINGS len"); break; }
      uint8_t sid = body[0];
      uint16_t minv = (uint16_t)(body[1] | (body[2] << 8));
      uint16_t maxv = (uint16_t)(body[3] | (body[4] << 8));
      uint16_t max_speed = (uint16_t)(body[5] | (body[6] << 8));
      uint16_t accel = (uint16_t)(body[7] | (body[8] << 8));
      uint16_t decel = (uint16_t)(body[9] | (body[10] << 8));
      setServoSettings(sid, (int)minv, (int)maxv, (int)max_speed, (int)accel, (int)decel);
      break;
    }
    case PKT_SRV_RECOIL_JERK: {
      if (plen != 5) { Serial.println("ERROR: SRV_RECOIL_JERK len"); break; }
      uint8_t sid = body[0];
      uint16_t jerk_us = (uint16_t)(body[1] | (body[2] << 8));
      uint16_t variance_us = (uint16_t)(body[3] | (body[4] << 8));
      setServoRecoilJerk(sid, (int)jerk_us, (int)variance_us);
      break;
    }
    case PKT_SMOKE_HEAT: {
      if (plen != 1) { Serial.println("ERROR: SMOKE_HEAT len"); break; }
      setSmokeHeater(body[0] != 0);
      break;
    }
    case PKT_INIT: {
      // Main daemon initialization - reset to safe state
      Serial.println("INFO: Received INIT");
      performSafeInit();
      sendInitReady();
      watchdogReset();
      break;
    }
    case PKT_SHUTDOWN: {
      // Main daemon shutdown - enter safe state
      Serial.println("INFO: Received SHUTDOWN");
      performSafeShutdown();
      watchdogDisable();
      break;
    }
    case PKT_KEEPALIVE: {
      // Keepalive from main daemon - update watchdog timer
      watchdogReset();
      break;
    }
    default:
      Serial.print("ERROR: Unknown pkt type ");
      Serial.println(type, HEX);
      break;
  }
}

// ---------------------- Periodic Update Functions ----------------------

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

size_t buildStatusPayload(uint8_t* payload, uint32_t now_ms) {
  size_t idx = 0;
  
  // Build status flags
  uint8_t flags = 0;
  if (is_firing) flags |= 0x01;
  if (flash_active) flags |= 0x02;
  if (flash_fading) flags |= 0x04;
  if (smoke_heater_on) flags |= 0x08;
  if (smoke_fan_on) flags |= 0x10;
  if (smoke_fan_pending_off) flags |= 0x20;
  payload[idx++] = flags;

  // Fan off remaining time
  uint16_t fan_remain = 0;
  if (smoke_fan_pending_off && smoke_fan_off_time_ms > now_ms) {
    fan_remain = (uint16_t)(smoke_fan_off_time_ms - now_ms);
  }
  payload[idx++] = (uint8_t)(fan_remain & 0xFF);
  payload[idx++] = (uint8_t)(fan_remain >> 8);

  // Servo positions
  uint16_t s1 = (uint16_t)constrain((int)servo_motion[0].position_us, 0, 3000);
  uint16_t s2 = (uint16_t)constrain((int)servo_motion[1].position_us, 0, 3000);
  uint16_t s3 = (uint16_t)constrain((int)servo_motion[2].position_us, 0, 3000);
  payload[idx++] = (uint8_t)(s1 & 0xFF);
  payload[idx++] = (uint8_t)(s1 >> 8);
  payload[idx++] = (uint8_t)(s2 & 0xFF);
  payload[idx++] = (uint8_t)(s2 >> 8);
  payload[idx++] = (uint8_t)(s3 & 0xFF);
  payload[idx++] = (uint8_t)(s3 >> 8);

  // Rate of fire
  uint16_t rpm = (uint16_t)rate_of_fire_rpm;
  payload[idx++] = (uint8_t)(rpm & 0xFF);
  payload[idx++] = (uint8_t)(rpm >> 8);

  return idx;
}

void emitStatus(uint32_t now_ms) {
  uint8_t payload[MAX_PACKET_SIZE];
  size_t payload_len = buildStatusPayload(payload, now_ms);
  sendPacket(PKT_STATUS, payload, payload_len);
}

// ---------------------- Arduino Setup & Loop ----------------------

void setup() {
  // USB Serial (CDC - Communication Device Class)
  // The Pico advertises as: VID=0x2e8a (Raspberry Pi Foundation), PID=0x0180 (gunfx_pico)
  // This appears as /dev/ttyACM0 on Linux
  Serial.begin(SERIAL_BAUD);
  
  // Wait for USB serial connection (optional, remove if you want immediate start)
  while (!Serial && millis() < 3000) {
    delay(10);
  }
  
  // Initialize outputs
  pinMode(PIN_NOZZLE_FLASH, OUTPUT);
  pinMode(PIN_SMOKE_FAN, OUTPUT);
  pinMode(PIN_SMOKE_HEATER, OUTPUT);
  
  analogWrite(PIN_NOZZLE_FLASH, 0);
  digitalWrite(PIN_SMOKE_FAN, LOW);
  digitalWrite(PIN_SMOKE_HEATER, LOW);
  
  // Initialize gun servos
  gun_srv_1.attach(PIN_GUN_SRV_1);
  gun_srv_2.attach(PIN_GUN_SRV_2);
  gun_srv_3.attach(PIN_GUN_SRV_3);
  
  gun_srv_1.writeMicroseconds(SERVO_DEFAULT_US);
  gun_srv_2.writeMicroseconds(SERVO_DEFAULT_US);
  gun_srv_3.writeMicroseconds(SERVO_DEFAULT_US);
  uint32_t now = millis();
  servo_motion[0].last_update_ms = now;
  servo_motion[1].last_update_ms = now;
  servo_motion[2].last_update_ms = now;
  
  Serial.println("GunFX Pico Controller Ready (binary protocol, COBS framed, delimiter 0x00)");
  Serial.println("Packets: type,len,payload...,crc8(0x07)");
}

void loop() {
  // Process incoming binary packets (COBS framed, delimited by 0x00)
  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();
    if (b == 0x00) {
      if (rx_index > 0) {
        uint8_t decoded[MAX_PACKET_SIZE];
        size_t decoded_len = cobsDecode(rx_buffer, rx_index, decoded);
        rx_index = 0;
        if (decoded_len >= 3) { // type+len+crc at least
          uint8_t crc = decoded[decoded_len - 1];
          if (crc8(decoded, decoded_len - 1) == crc) {
            processPacket(decoded, decoded_len - 1); // exclude crc byte
          } else {
            Serial.println("ERROR: CRC");
          }
        }
      }
    } else {
      if (rx_index < sizeof(rx_buffer)) {
        rx_buffer[rx_index++] = b;
      } else {
        rx_index = 0; // overflow, reset
      }
    }
  }
  
  // Update hardware states
  updateMuzzleFlash();
  updateSmokeFan();
  updateAllServos();

  uint32_t now = millis();
  
  // Watchdog check - shutdown if no keepalive for 90 seconds
  watchdogCheck(now);
  
  if (now >= next_status_ms) {
    emitStatus(now);
    next_status_ms = now + STATUS_INTERVAL_MS;
  }
  
  // Small delay to prevent tight loop
  delay(1);
}
