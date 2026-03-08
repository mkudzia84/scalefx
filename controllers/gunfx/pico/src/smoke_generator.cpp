/*
 * Smoke Generator Module - Implementation
 *
 * Heater relay + PWM fan with constant and pulsing modes.
 * Includes disconnect detection and overcurrent protection.
 */

#include "smoke_generator.h"

using namespace SmokeGeneratorConfig;

// ============================================================================
// Initialization
// ============================================================================

void SmokeGenerator::begin(uint8_t heaterPin, uint8_t fanPin) {
    _heaterPin = heaterPin;
    _fanPin = fanPin;

    pinMode(_heaterPin, OUTPUT);
    pinMode(_fanPin, OUTPUT);
    _setHeaterPin(false);
    _setFanPwm(0);

    // Initialize config defaults
    _cfg.fanPulsing   = false;
    _cfg.fanSpeed     = DEFAULT_SPEED;
    _cfg.fanPulseHigh = DEFAULT_PULSE_HIGH;
    _cfg.fanPulseLow  = DEFAULT_PULSE_LOW;
    _cfg.fanPulseMs   = 0;  // Auto-calculate from RPM
    _cfg.fanSpindownMs = DEFAULT_SPINDOWN_MS;
}

// ============================================================================
// Heater Control
// ============================================================================

void SmokeGenerator::setHeater(bool on) {
    if (on && !_heaterOn) {
        _heaterOnStart_ms = millis();
    } else if (!on && _heaterOn && _heaterOnStart_ms > 0) {
        _totalHeaterOn_ms += millis() - _heaterOnStart_ms;
        _heaterOnStart_ms = 0;
    }

    _heaterOn = on;
    _setHeaterPin(on);
}

uint32_t SmokeGenerator::heaterOnTime_ms() const {
    uint32_t total = _totalHeaterOn_ms;
    if (_heaterOn && _heaterOnStart_ms > 0) {
        total += millis() - _heaterOnStart_ms;
    }
    return total;
}

// ============================================================================
// Fan Control
// ============================================================================

void SmokeGenerator::startFan(uint16_t rpm) {
    _rpm = rpm;
    _pendingOff = false;
    _pulseActive = false;
    _firingActive = true;

    if (_fanMode == SmokeFanMode::Pulsing) {
        _fanSpeed = _cfg.fanPulseLow;
    } else {
        _fanSpeed = _cfg.fanSpeed;
    }

    _fanOn = true;
    _setFanPwm(_fanSpeed);
}

void SmokeGenerator::stopFan(uint16_t delay_ms) {
    _firingActive = false;

    if (delay_ms == 0) {
        _fanOn = false;
        _fanSpeed = 0;
        _pendingOff = false;
        _pulseActive = false;
        _setFanPwm(0);
    } else {
        _pendingOff = true;
        _fanOffTime_ms = millis() + delay_ms;
    }
}

void SmokeGenerator::triggerPulse() {
    if (_fanMode != SmokeFanMode::Pulsing || !_fanOn) return;

    _pulseActive = true;
    _pulseEnd_ms = millis() + _effectivePulseMs();
    _fanSpeed = _cfg.fanPulseHigh;
    _setFanPwm(_fanSpeed);
}

void SmokeGenerator::forceOff() {
    _fanOn = false;
    _fanSpeed = 0;
    _pendingOff = false;
    _pulseActive = false;
    _firingActive = false;
    _setFanPwm(0);
}

uint16_t SmokeGenerator::spindownRemaining_ms() const {
    if (!_pendingOff) return 0;
    uint32_t now = millis();
    if (_fanOffTime_ms > now) {
        return (uint16_t)(_fanOffTime_ms - now);
    }
    return 0;
}

// ============================================================================
// Configuration
// ============================================================================

void SmokeGenerator::configure(const GunFxSmokeConfig& cfg) {
    _cfg = cfg;
    _fanMode = cfg.fanPulsing ? SmokeFanMode::Pulsing : SmokeFanMode::Constant;

    // If currently firing, apply new config immediately
    if (_firingActive && _fanOn) {
        if (_fanMode == SmokeFanMode::Pulsing) {
            _fanSpeed = _cfg.fanPulseLow;
        } else {
            _fanSpeed = _cfg.fanSpeed;
        }
        _setFanPwm(_fanSpeed);
    }
}

// ============================================================================
// Update
// ============================================================================

void SmokeGenerator::update() {
    // Handle spindown timeout
    if (_pendingOff) {
        if (millis() >= _fanOffTime_ms) {
            _fanOn = false;
            _fanSpeed = 0;
            _pendingOff = false;
            _pulseActive = false;
            _setFanPwm(0);
        }
        // Note: fall through — still run protection during spindown
    }

    // Handle pulse timeout (Pulsing mode)
    if (_fanMode == SmokeFanMode::Pulsing && _pulseActive) {
        if (millis() >= _pulseEnd_ms) {
            _pulseActive = false;
            if (_fanOn && _firingActive) {
                _fanSpeed = _cfg.fanPulseLow;
                _setFanPwm(_fanSpeed);
            }
        }
    }

    // Run current protection for attached monitors
    for (auto ch : { SmokeChannel::Heater, SmokeChannel::Fan }) {
        if (_currentMonitor[(uint8_t)ch]) {
            _updateProtection(ch);
        }
    }
}

// ============================================================================
// Shutdown / Reset
// ============================================================================

void SmokeGenerator::shutdown() {
    setHeater(false);
    forceOff();
    _rpm = 0;
}

// ============================================================================
// Internal Helpers
// ============================================================================

void SmokeGenerator::_setFanPwm(uint8_t duty) {
    // Enforce overcurrent cap — single pin writer, no racing
    uint8_t actual = min(duty, _protection[(uint8_t)SmokeChannel::Fan].cappedDuty);
    analogWrite(_fanPin, actual);
}

void SmokeGenerator::_setHeaterPin(bool on) {
    digitalWrite(_heaterPin, on ? HIGH : LOW);
}

uint16_t SmokeGenerator::_effectivePulseMs() const {
    // Explicit override — use config value directly
    if (_cfg.fanPulseMs > 0) return _cfg.fanPulseMs;

    // Auto-calculate from RPM: 50% duty cycle of shot interval
    if (_rpm > 0) {
        uint16_t halfInterval = 30000 / _rpm;   // ms
        return constrain(halfInterval, AUTO_PULSE_MIN_MS, AUTO_PULSE_MAX_MS);
    }

    // Fallback when RPM unknown
    return DEFAULT_PULSE_MS;
}

// ============================================================================
// Current Monitoring
// ============================================================================

void SmokeGenerator::attachCurrentMonitor(SmokeChannel channel, INA226* monitor) {
    if ((uint8_t)channel >= (uint8_t)SmokeChannel::COUNT) return;
    _currentMonitor[(uint8_t)channel] = monitor;
}

uint16_t SmokeGenerator::readCurrent_mA(SmokeChannel channel) const {
    uint8_t ch = (uint8_t)channel;
    if (ch >= (uint8_t)SmokeChannel::COUNT || !_currentMonitor[ch]) return 0;
    float raw = _currentMonitor[ch]->current_mA();
    return (uint16_t)(raw < 0.0f ? -raw : raw);
}

// ============================================================================
// Current Protection (internal)
// ============================================================================

void SmokeGenerator::_updateProtection(SmokeChannel channel) {
    using namespace SmokeProtection;

    uint8_t ch = (uint8_t)channel;
    bool isActive = (channel == SmokeChannel::Heater) ? _heaterOn : _fanOn;
    auto& p = _protection[ch];
    uint32_t now = millis();

    // ── Detect off→on transition ──
    if (isActive && !p.active) {
        p = SmokeChannelProtection();
        p.active = true;
        p.activeStart_ms = now;
        _errorReason[ch] = SmokeErrorReason::NONE;
    }
    // ── Detect on→off transition ──
    else if (!isActive && p.active) {
        p.active = false;
        p.disconnected = false;
        p.zeroDetected = false;
        p.overCurrent = false;
        p.cappedDuty = 255;
        p.shutoff = false;
        // Error reason preserved for diagnostics until off→on or resetErrors()
    }

    // Only check while active and past startup ignore period
    if (!p.active) return;
    if (p.shutoff) return;  // Already shut off — nothing to do
    if (now - p.activeStart_ms < STARTUP_IGNORE_ms) return;

    uint16_t absCurrent_mA = readCurrent_mA(channel);

    // ── Disconnect Detection ──
    if (absCurrent_mA <= ZERO_CURRENT_THRESH_mA) {
        if (!p.zeroDetected) {
            p.zeroDetected = true;
            p.zeroStart_ms = now;
        } else if (now - p.zeroStart_ms >= ZERO_CURRENT_TIMEOUT_ms) {
            p.disconnected = true;
            _errorReason[ch] = (channel == SmokeChannel::Heater)
                ? SmokeErrorReason::HEATER_DISCONNECTED
                : SmokeErrorReason::FAN_DISCONNECTED;
        }
    } else {
        p.zeroDetected = false;
        if (p.disconnected) {
            p.disconnected = false;
            _errorReason[ch] = SmokeErrorReason::NONE;
        }
    }

    // ── Overcurrent Protection ──
    uint16_t limit = _currentLimit_mA[ch];
    if (limit == 0) return;  // Protection disabled

    if (absCurrent_mA > limit) {
        // Current exceeds limit — start/continue debounce
        if (!p.overCurrent) {
            p.overCurrent = true;
            p.overStart_ms = now;
            p.lastStep_ms = now;
        } else if (now - p.overStart_ms >= OVERCURRENT_DEBOUNCE_ms) {
            // Heater is a relay — no PWM stepping, immediate shutoff
            if (channel == SmokeChannel::Heater) {
                p.shutoff = true;
                _errorReason[ch] = SmokeErrorReason::HEATER_OVERCURRENT;
                setHeater(false);
                return;
            }

            // Fan — step PWM down gradually
            if (now - p.lastStep_ms >= PWM_STEP_INTERVAL_ms) {
                if (p.cappedDuty > PWM_STEP_SIZE) {
                    p.cappedDuty -= PWM_STEP_SIZE;
                } else {
                    p.cappedDuty = 0;
                }
                p.lastStep_ms = now;

                // Re-apply current fan speed with new cap (enforced in _setFanPwm)
                _setFanPwm(_fanSpeed);

                // If cap bottomed out — shut off the channel
                if (p.cappedDuty == 0) {
                    p.shutoff = true;
                    _errorReason[ch] = SmokeErrorReason::FAN_OVERCURRENT;
                    forceOff();
                }
            }
        }
    } else {
        // Current within limits
        p.overCurrent = false;

        // If previously throttled, maintain the cap (don't auto-increase)
        // The cap is already enforced inside _setFanPwm, so no extra write needed
    }
}

void SmokeGenerator::setCurrentLimit(SmokeChannel channel, uint16_t limit_mA) {
    uint8_t ch = (uint8_t)channel;
    if (ch >= (uint8_t)SmokeChannel::COUNT) return;
    _currentLimit_mA[ch] = limit_mA;
    // Reset throttle state when limit changes
    _protection[ch].cappedDuty = 255;
    _protection[ch].overCurrent = false;
    _protection[ch].shutoff = false;
}

void SmokeGenerator::resetErrors() {
    for (auto ch : { SmokeChannel::Heater, SmokeChannel::Fan }) {
        uint8_t i = (uint8_t)ch;
        _protection[i].disconnected = false;
        _protection[i].zeroDetected = false;
        _protection[i].overCurrent = false;
        _protection[i].cappedDuty = 255;
        _protection[i].shutoff = false;
        _errorReason[i] = SmokeErrorReason::NONE;
    }
}

uint8_t SmokeGenerator::errorReason(SmokeChannel channel) const {
    uint8_t ch = (uint8_t)channel;
    return (ch < (uint8_t)SmokeChannel::COUNT) ? _errorReason[ch] : SmokeErrorReason::NONE;
}

uint8_t SmokeGenerator::cappedDuty(SmokeChannel channel) const {
    uint8_t ch = (uint8_t)channel;
    return (ch < (uint8_t)SmokeChannel::COUNT) ? _protection[ch].cappedDuty : 255;
}

bool SmokeGenerator::hasError() const {
    return _errorReason[(uint8_t)SmokeChannel::Heater] != SmokeErrorReason::NONE
        || _errorReason[(uint8_t)SmokeChannel::Fan]    != SmokeErrorReason::NONE;
}
