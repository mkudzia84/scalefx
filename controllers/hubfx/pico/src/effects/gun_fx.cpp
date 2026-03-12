/**
 * GunFX - Implementation
 * 
 * Gun effects controller using GunFxSerialMasterBinary and AudioMixer.
 */

#include "gun_fx.h"
#include "effects_config.h"
#include <audio/audio_mixer.h>

// Debug logging using centralized config
#define LOG(fmt, ...) EFFECTS_LOG("GunFX", fmt, ##__VA_ARGS__)

// ============================================================================
//  INITIALIZATION
// ============================================================================

bool GunFX::begin(UsbHost* usbHost, int deviceIndex, const GunFXSettings& settings) {
    if (!usbHost) return false;
    if (!settings.enabled) {
        LOG("Disabled in settings");
        return false;
    }
    
    _settings = settings;
    
    // Initialize serial master
    if (!_serial.begin(usbHost, deviceIndex)) {
        LOG("Failed to initialize serial");
        return false;
    }
    
    _serial.setKeepaliveInterval(GunFXConfig::KEEPALIVE_INTERVAL_MS);
    
    // Set compatible slave firmware versions
    static const char* compatibleVersions[] = {"v0.1.0"};
    _serial.setCompatibleVersions(compatibleVersions, 1);
    
    // Set up status callback
    _serial.onStatus([this](const GunFxStatus& status) {
        // Status received - can add handling here if needed
    });
    
    _serial.onReady([this](const char* name) {
        const GunFxBoardInfo& info = _serial.boardInfo();
        if (_serial.isVersionCompatible()) {
            LOG("Slave ready: %s (%s, %s) - COMPATIBLE", 
                info.deviceName, info.firmwareVersion, info.platform);
        } else {
            LOG("Slave ready: %s (%s, %s) - INCOMPATIBLE VERSION!", 
                info.deviceName, info.firmwareVersion, info.platform);
            LOG("Expected versions: 0.1.0");
        }
    });
    
    // Initialize PWM inputs
    if (settings.triggerChannel > 0) {
        _triggerInput.beginChannel(PwmInputType::Pwm, settings.triggerChannel);
    }
    if (settings.smoke.heaterToggleChannel > 0) {
        _heaterToggleInput.beginChannel(PwmInputType::Pwm, settings.smoke.heaterToggleChannel);
    }
    if (settings.pitch.inputChannel > 0) {
        _pitchInput.beginChannel(PwmInputType::Pwm, settings.pitch.inputChannel);
    }
    if (settings.yaw.inputChannel > 0) {
        _yawInput.beginChannel(PwmInputType::Pwm, settings.yaw.inputChannel);
    }
    
    // Initialize state
    _currentRateIndex = -1;
    _lastPitchOutputUs = -1;
    _lastYawOutputUs = -1;
    
    _initialized = true;
    
    LOG("Initialized (device %d, trigger ch=%d, rates=%d)",
        deviceIndex, settings.triggerChannel, settings.rateCount);
    
    return true;
}

void GunFX::end() {
    if (!_initialized) return;
    
    stop();
    _serial.end();
    _initialized = false;
    
    LOG("Ended");
}

void GunFX::setDevice(int deviceIndex) {
    _serial.setDevice(deviceIndex);
    LOG("Device changed to %d", deviceIndex);
}

// ============================================================================
//  START / STOP
// ============================================================================

bool GunFX::start() {
    if (!_initialized) return false;
    
    // Send INIT to GunFX Pico with keepalive interval
    // The slave will use 1.5x this interval for connection timeout
    _serial.sendInit(GunFXConfig::KEEPALIVE_INTERVAL_MS);
    busy_wait_ms(100);
    
    // Configure pitch servo
    if (_settings.pitch.servoId > 0) {
        GunFxServoConfig cfg;
        cfg.servoId = _settings.pitch.servoId;
        cfg.minUs = _settings.pitch.outputMinUs;
        cfg.maxUs = _settings.pitch.outputMaxUs;
        cfg.maxSpeedUsPerSec = _settings.pitch.maxSpeedUsPerSec;
        cfg.maxAccelUsPerSec2 = _settings.pitch.maxAccelUsPerSec2;
        cfg.maxDecelUsPerSec2 = _settings.pitch.maxDecelUsPerSec2;
        _serial.setServoConfig(cfg);
        
        if (_settings.pitch.recoilJerkUs > 0) {
            _serial.setRecoilJerk(_settings.pitch.servoId, 
                                  _settings.pitch.recoilJerkUs,
                                  _settings.pitch.recoilJerkVarianceUs);
        }
    }
    
    // Configure yaw servo
    if (_settings.yaw.servoId > 0) {
        GunFxServoConfig cfg;
        cfg.servoId = _settings.yaw.servoId;
        cfg.minUs = _settings.yaw.outputMinUs;
        cfg.maxUs = _settings.yaw.outputMaxUs;
        cfg.maxSpeedUsPerSec = _settings.yaw.maxSpeedUsPerSec;
        cfg.maxAccelUsPerSec2 = _settings.yaw.maxAccelUsPerSec2;
        cfg.maxDecelUsPerSec2 = _settings.yaw.maxDecelUsPerSec2;
        _serial.setServoConfig(cfg);
        
        if (_settings.yaw.recoilJerkUs > 0) {
            _serial.setRecoilJerk(_settings.yaw.servoId,
                                  _settings.yaw.recoilJerkUs,
                                  _settings.yaw.recoilJerkVarianceUs);
        }
    }
    
    LOG("Started");
    return true;
}

void GunFX::stop() {
    if (!_initialized) return;
    
    if (_isFiring) {
        ceaseFire();
    }
    
    _serial.sendShutdown();
    LOG("Stopped");
}

// ============================================================================
//  PROCESSING
// ============================================================================

void GunFX::process() {
    if (!_initialized) return;
    
    // Process serial communication
    _serial.process();
    _serial.processKeepalive();
    
    // Skip input processing if not connected or incompatible version
    if (!_serial.isConnected() || !_serial.isVersionCompatible()) return;
    
    // Update all PWM inputs
    _triggerInput.update();
    _heaterToggleInput.update();
    _pitchInput.update();
    _yawInput.update();
    
    // Process trigger input
    if (_triggerInput.isEnabled()) {
        int triggerPwm = _triggerInput.average();
        int newRate = selectRateOfFire(triggerPwm);
        
        if (newRate != _currentRateIndex) {
            if (newRate >= 0) {
                // Start or change rate
                int newRpm = _settings.ratesOfFire[newRate].rpm;
                _serial.triggerOn(newRpm);
                _isFiring = true;
                _currentRpm = newRpm;
                _currentRateIndex = newRate;
                playFiringSound(newRate);
                LOG("Firing: rate %d, RPM %d", newRate, newRpm);
            } else {
                // Stop firing
                ceaseFire();
            }
        }
    }
    
    // Process heater toggle
    if (_heaterToggleInput.isEnabled()) {
        bool heaterOn = _heaterToggleInput.aboveThreshold(
            _settings.smoke.heaterThresholdUs, GunFXConfig::RATE_HYSTERESIS_US);
        
        if (heaterOn != _smokeHeaterOn) {
            _smokeHeaterOn = heaterOn;
            _serial.setSmokeHeater(heaterOn);
            LOG("Smoke heater: %s", heaterOn ? "ON" : "OFF");
        }
    }
    
    // Process pitch servo
    if (_pitchInput.isEnabled() && _settings.pitch.servoId > 0) {
        int inputUs = _pitchInput.average();
        int outputUs = mapServoInput(_settings.pitch, inputUs);
        
        if (abs(outputUs - _lastPitchOutputUs) > GunFXConfig::SERVO_DEADBAND_US) {
            _serial.setServoPosition(_settings.pitch.servoId, outputUs);
            _lastPitchOutputUs = outputUs;
        }
    }
    
    // Process yaw servo
    if (_yawInput.isEnabled() && _settings.yaw.servoId > 0) {
        int inputUs = _yawInput.average();
        int outputUs = mapServoInput(_settings.yaw, inputUs);
        
        if (abs(outputUs - _lastYawOutputUs) > GunFXConfig::SERVO_DEADBAND_US) {
            _serial.setServoPosition(_settings.yaw.servoId, outputUs);
            _lastYawOutputUs = outputUs;
        }
    }
    
    // Periodic debug output
    uint32_t now = millis();
    if (now - _lastDebugTimeMs >= GunFXConfig::DEBUG_INTERVAL_MS) {
        _lastDebugTimeMs = now;
        LOG("Trigger=%d us, Heater=%s, Firing=%s (%d RPM)",
            _triggerInput.average(),
            _smokeHeaterOn ? "ON" : "OFF",
            _isFiring ? "YES" : "NO",
            _currentRpm);
    }
}

// ============================================================================
//  RATE SELECTION
// ============================================================================

int GunFX::selectRateOfFire(int pwmUs) {
    if (_settings.rateCount <= 0) return -1;
    
    // Build thresholds array
    int thresholds[GunFXConfig::MAX_RATES_OF_FIRE];
    for (int i = 0; i < _settings.rateCount; i++) {
        thresholds[i] = _settings.ratesOfFire[i].pwmThresholdUs;
    }
    
    return PwmInput::bandMatch(pwmUs, thresholds, _settings.rateCount,
                               _currentRateIndex, GunFXConfig::RATE_HYSTERESIS_US);
}

int GunFX::mapServoInput(const ServoInputConfig& cfg, int inputUs) {
    // Clamp input
    inputUs = constrain(inputUs, cfg.inputMinUs, cfg.inputMaxUs);
    
    // Linear mapping
    float inputRange = static_cast<float>(cfg.inputMaxUs - cfg.inputMinUs);
    float outputRange = static_cast<float>(cfg.outputMaxUs - cfg.outputMinUs);
    
    if (inputRange <= 0.0f) return cfg.outputMinUs;
    
    float normalized = static_cast<float>(inputUs - cfg.inputMinUs) / inputRange;
    int outputUs = cfg.outputMinUs + static_cast<int>(normalized * outputRange);
    
    return constrain(outputUs, cfg.outputMinUs, cfg.outputMaxUs);
}

// ============================================================================
//  AUDIO
// ============================================================================

void GunFX::playFiringSound(int rateIndex) {
    if (rateIndex < 0 || rateIndex >= _settings.rateCount) return;
    
    const RateOfFireConfig& rate = _settings.ratesOfFire[rateIndex];
    if (!rate.soundFile) return;
    
    AudioPlaybackOptions opts;
    opts.loop = true;
    opts.volume = rate.soundVolume;
    opts.output = AudioOutput::Stereo;
    
    mixer().playAsync(_settings.audioChannel, rate.soundFile, opts);
    LOG("Playing sound: %s", rate.soundFile);
}

void GunFX::stopFiringSound() {
    mixer().stopAsync(_settings.audioChannel, AudioStopMode::Immediate);
}

// ============================================================================
//  MANUAL CONTROL
// ============================================================================

void GunFX::trigger(int rpm) {
    if (!_initialized || !_serial.isVersionCompatible()) return;
    
    if (rpm > 0) {
        _serial.triggerOn(rpm);
        _isFiring = true;
        _currentRpm = rpm;
    } else {
        ceaseFire();
    }
}

void GunFX::ceaseFire() {
    if (!_initialized || !_serial.isVersionCompatible()) return;
    
    _isFiring = false;
    _currentRpm = 0;
    _currentRateIndex = -1;
    _serial.triggerOff(_settings.smoke.fanOffDelayMs);
    stopFiringSound();
    
    LOG("Cease fire");
}

void GunFX::setSmokeHeater(bool on) {
    if (!_initialized || !_serial.isVersionCompatible()) return;
    
    _smokeHeaterOn = on;
    _serial.setSmokeHeater(on);
}

void GunFX::setServo(int servoId, int pulseUs) {
    if (!_initialized || !_serial.isVersionCompatible()) return;
    _serial.setServoPosition(servoId, pulseUs);
}

// ============================================================================
//  DEBUG
// ============================================================================

#if EFFECTS_DEBUG
void GunFX::printStatus() {
    GUNFX_LOG("Status:");
    GUNFX_LOG("  Connected: %s", isConnected() ? "yes" : "no");
    
    const GunFxBoardInfo& info = _serial.boardInfo();
    if (isSlaveReady()) {
        GUNFX_LOG("  Slave: %s (%s, %s)", 
                      info.deviceName, info.firmwareVersion, info.platform);
        GUNFX_LOG("  Version compatible: %s", 
                      _serial.isVersionCompatible() ? "YES" : "NO - INCOMPATIBLE!");
        if (!_serial.isVersionCompatible()) {
            GUNFX_LOG("  WARNING: Slave not activated due to version mismatch");
        }
    } else {
        GUNFX_LOG("  Slave ready: no");
    }
    
    GUNFX_LOG("  Firing: %s (%d RPM, rate %d)", 
                  _isFiring ? "yes" : "no", _currentRpm, _currentRateIndex);
    GUNFX_LOG("  Heater: %s", _smokeHeaterOn ? "ON" : "OFF");
    GUNFX_LOG("  Trigger PWM: %d us", triggerPwm());
    GUNFX_LOG("  Rates configured: %d", _settings.rateCount);
    
    const GunFxStatus& status = slaveStatus();
    GUNFX_LOG("  Slave status: firing=%d, flash=%d, heater=%d, fan=%d",
                  status.firing, status.flashActive, 
                  status.heaterOn, status.fanOn);
    GUNFX_LOG("  Servos: %d, %d, %d us",
                  status.servoUs[0], status.servoUs[1], status.servoUs[2]);
    
    const CoreStats& stats = _serial.stats();
    GUNFX_LOG("  Packets: TX=%lu, RX=%lu, CRC err=%lu",
                  stats.packets_sent, stats.packets_received, stats.crc_errors);
}
#endif
