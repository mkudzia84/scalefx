/**
 * GunFX - Gun Effects Controller
 * 
 * Communication wrapper for GunFX slave boards connected over USB.
 * Monitors PWM inputs for trigger, heater toggle, and turret servos,
 * then sends commands to the GunFX Pico via GunFxSerialMaster.
 * 
 * GunFX Pico handles: nozzle flash, smoke heater/fan, 3x servos with recoil
 * This module handles: PWM input monitoring, rate selection, sound playback
 */

#ifndef GUN_FX_H
#define GUN_FX_H

#include <Arduino.h>
#include <serial_gunfx.h>
#include <pwm_control.h>

// Forward declarations
class AudioMixer;
class UsbHost;

// ============================================================================
//  CONSTANTS
// ============================================================================

namespace GunFXConfig {
    constexpr int RATE_HYSTERESIS_US   = 50;
    constexpr int KEEPALIVE_INTERVAL_MS = 30000;
    constexpr int DEBUG_INTERVAL_MS    = 10000;
    constexpr int MAX_RATES_OF_FIRE    = 8;
    constexpr int SERVO_DEADBAND_US    = 5;
}

// ============================================================================
//  TYPES
// ============================================================================

struct RateOfFireConfig {
    int pwmThresholdUs = 0;
    int rpm = 0;
    const char* soundFile = nullptr;
    float soundVolume = 1.0f;
};

struct ServoInputConfig {
    int inputChannel = -1;
    int servoId = 0;
    int inputMinUs = 1000;
    int inputMaxUs = 2000;
    int outputMinUs = 1000;
    int outputMaxUs = 2000;
    int maxSpeedUsPerSec = 0;
    int maxAccelUsPerSec2 = 0;
    int maxDecelUsPerSec2 = 0;
    int recoilJerkUs = 0;
    int recoilJerkVarianceUs = 0;
};

struct SmokeConfig {
    int heaterToggleChannel = -1;
    int heaterThresholdUs = 1500;
    int fanOffDelayMs = 2000;
};

struct GunFXSettings {
    bool enabled = false;
    
    // Trigger input
    int triggerChannel = -1;
    
    // Rates of fire
    RateOfFireConfig ratesOfFire[GunFXConfig::MAX_RATES_OF_FIRE];
    int rateCount = 0;
    
    // Turret servos
    ServoInputConfig pitch;
    ServoInputConfig yaw;
    
    // Smoke
    SmokeConfig smoke;
    
    // Audio channel for gun sounds
    int audioChannel = 0;
};

// ============================================================================
//  GUN FX CLASS
// ============================================================================

class GunFX {
public:
    GunFX() = default;
    ~GunFX() = default;
    
    // Non-copyable
    GunFX(const GunFX&) = delete;
    GunFX& operator=(const GunFX&) = delete;

    // ---- Initialization ----
    bool begin(UsbHost* usbHost, int deviceIndex, 
               AudioMixer* mixer, const GunFXSettings& settings);
    void end();
    
    // ---- Processing ----
    void process();
    bool start();
    void stop();
    
    // ---- Device Management ----
    void setDevice(int deviceIndex);
    
    // ---- Manual Control ----
    void trigger(int rpm);
    void ceaseFire();
    void setSmokeHeater(bool on);
    void setServo(int servoId, int pulseUs);
    
    // ---- Status ----
    bool isFiring() const { return _isFiring; }
    int rpm() const { return _currentRpm; }
    int rateIndex() const { return _currentRateIndex; }
    bool isHeaterOn() const { return _smokeHeaterOn; }
    bool isConnected() const { return _serial.isConnected(); }
    bool isSlaveReady() const { return _serial.isSlaveReady(); }
    const GunFxStatus& slaveStatus() const { return _serial.lastStatus(); }
    
    // ---- PWM Readings ----
    int triggerPwm() const { return _triggerInput.average(); }
    int heaterTogglePwm() const { return _heaterToggleInput.average(); }
    int pitchPwm() const { return _pitchInput.average(); }
    int yawPwm() const { return _yawInput.average(); }
    
    // ---- Debug ----
    void printStatus();

private:
    // ---- Rate Selection ----
    int selectRateOfFire(int pwmUs);
    int mapServoInput(const ServoInputConfig& cfg, int inputUs);
    
    // ---- Audio ----
    void playFiringSound(int rateIndex);
    void stopFiringSound();

    // ---- State ----
    GunFXSettings _settings;
    GunFxSerialMaster _serial;
    AudioMixer* _mixer = nullptr;
    
    PwmInput _triggerInput;
    PwmInput _heaterToggleInput;
    PwmInput _pitchInput;
    PwmInput _yawInput;
    
    bool _isFiring = false;
    int _currentRateIndex = -1;
    int _currentRpm = 0;
    bool _smokeHeaterOn = false;
    
    int _lastPitchOutputUs = -1;
    int _lastYawOutputUs = -1;
    
    uint32_t _lastDebugTimeMs = 0;
    bool _initialized = false;
};

#endif // GUN_FX_H
