/**
 * GunFX - Implementation
 * 
 * Gun effects controller using GunFxSerialMaster and AudioMixer.
 */

#include "gun_fx.h"
#include "audio_mixer.h"

// ============================================================================
//  DEBUG LOGGING
// ============================================================================

#ifndef GUN_FX_DEBUG
#define GUN_FX_DEBUG 1
#endif

#if GUN_FX_DEBUG
#define LOG(fmt, ...) Serial.printf("[GunFX] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG(fmt, ...)
#endif

// ============================================================================
//  INITIALIZATION
// ============================================================================

bool GunFX::begin(UsbHost* usbHost, int deviceIndex, 
                  AudioMixer* mixer, const GunFXSettings& settings) {
    if (!usbHost) return false;
    if (!settings.enabled) {
        LOG("Disabled in settings");
        return false;
    }
    
    _settings = settings;
    _mixer = mixer;
    
    // Initialize serial master
    if (!_serial.begin(usbHost, deviceIndex)) {
        LOG("Failed to initialize serial");
        return false;
    }
    
    _serial.setKeepaliveInterval(GunFXConfig::KEEPALIVE_INTERVAL_MS);
    
    // Set up status callback
    _serial.onStatus([this](const GunFxStatus& status) {
        // Status received - can add handling here if needed
    });
    
    _serial.onReady([](const char* name) {
        LOG("Slave ready: %s", name);
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
    
    // Send INIT to GunFX Pico
    _serial.sendInit();
    delay(100);
    
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
    
    // Skip input processing if not connected
    if (!_serial.isConnected()) return;
    
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
    if (!_mixer) return;
    if (rateIndex < 0 || rateIndex >= _settings.rateCount) return;
    
    const RateOfFireConfig& rate = _settings.ratesOfFire[rateIndex];
    if (!rate.soundFile) return;
    
    AudioPlaybackOptions opts;
    opts.loop = true;
    opts.volume = rate.soundVolume;
    opts.output = AudioOutput::Stereo;
    
    _mixer->playAsync(_settings.audioChannel, rate.soundFile, opts);
    LOG("Playing sound: %s", rate.soundFile);
}

void GunFX::stopFiringSound() {
    if (_mixer) {
        _mixer->stopAsync(_settings.audioChannel, AudioStopMode::Immediate);
    }
}

// ============================================================================
//  MANUAL CONTROL
// ============================================================================

void GunFX::trigger(int rpm) {
    if (!_initialized) return;
    
    if (rpm > 0) {
        _serial.triggerOn(rpm);
        _isFiring = true;
        _currentRpm = rpm;
    } else {
        ceaseFire();
    }
}

void GunFX::ceaseFire() {
    if (!_initialized) return;
    
    _isFiring = false;
    _currentRpm = 0;
    _currentRateIndex = -1;
    _serial.triggerOff(_settings.smoke.fanOffDelayMs);
    stopFiringSound();
    
    LOG("Cease fire");
}

void GunFX::setSmokeHeater(bool on) {
    if (!_initialized) return;
    
    _smokeHeaterOn = on;
    _serial.setSmokeHeater(on);
}

void GunFX::setServo(int servoId, int pulseUs) {
    if (!_initialized) return;
    _serial.setServoPosition(servoId, pulseUs);
}

// ============================================================================
//  DEBUG
// ============================================================================

void GunFX::printStatus() {
    Serial.println("[GunFX] Status:");
    Serial.printf("  Connected: %s\n", isConnected() ? "yes" : "no");
    Serial.printf("  Slave ready: %s (%s)\n", 
                  isSlaveReady() ? "yes" : "no",
                  _serial.slaveName());
    Serial.printf("  Firing: %s (%d RPM, rate %d)\n", 
                  _isFiring ? "yes" : "no", _currentRpm, _currentRateIndex);
    Serial.printf("  Heater: %s\n", _smokeHeaterOn ? "ON" : "OFF");
    Serial.printf("  Trigger PWM: %d us\n", triggerPwm());
    Serial.printf("  Rates configured: %d\n", _settings.rateCount);
    
    const GunFxStatus& status = slaveStatus();
    Serial.printf("  Slave status: firing=%d, flash=%d, heater=%d, fan=%d\n",
                  status.firing, status.flashActive, 
                  status.heaterOn, status.fanOn);
    Serial.printf("  Servos: %d, %d, %d us\n",
                  status.servoUs[0], status.servoUs[1], status.servoUs[2]);
    
    const SerialBusStats& stats = _serial.stats();
    Serial.printf("  Packets: TX=%lu, RX=%lu, CRC err=%lu\n",
                  stats.packets_sent, stats.packets_received, stats.crc_errors);
}
