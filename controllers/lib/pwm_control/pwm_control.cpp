/*
 * PWM Control - Implementation
 * 
 * Object-oriented PWM input monitoring with averaging and hysteresis.
 * Supports both synchronous (blocking) and async (interrupt-driven) modes.
 */

#include "pwm_control.h"
#include <string.h>

// ============================================================================
// PwmInputMapping Static Members
// ============================================================================

int PwmInputMapping::_channelPins[PwmInputConfig::MAX_CHANNELS] = {0};
int PwmInputMapping::_maxChannel = PwmInputConfig::MAX_CHANNELS;
bool PwmInputMapping::_useCustomMapping = false;
int PwmInputMapping::_offset = PwmInputConfig::DEFAULT_CHANNEL_PIN_OFFSET;

void PwmInputMapping::setOffset(int offset, int maxChannel) {
    _offset = offset;
    _maxChannel = (maxChannel > PwmInputConfig::MAX_CHANNELS) ? 
                  PwmInputConfig::MAX_CHANNELS : maxChannel;
    _useCustomMapping = false;
}

void PwmInputMapping::setMapping(const int* channelPins, int count) {
    if (!channelPins || count <= 0) return;
    
    _maxChannel = (count > PwmInputConfig::MAX_CHANNELS) ? 
                  PwmInputConfig::MAX_CHANNELS : count;
    
    for (int i = 0; i < _maxChannel; i++) {
        _channelPins[i] = channelPins[i];
    }
    _useCustomMapping = true;
}

int PwmInputMapping::channelToPin(int channel) {
    if (channel < 1 || channel > _maxChannel) {
        return -1;
    }
    
    if (_useCustomMapping) {
        return _channelPins[channel - 1];
    }
    return _offset + channel;
}

int PwmInputMapping::pinToChannel(int pin) {
    if (_useCustomMapping) {
        for (int i = 0; i < _maxChannel; i++) {
            if (_channelPins[i] == pin) {
                return i + 1;
            }
        }
        return -1;
    }
    
    int channel = pin - _offset;
    if (channel < 1 || channel > _maxChannel) {
        return -1;
    }
    return channel;
}

void PwmInputMapping::reset() {
    _useCustomMapping = false;
    _offset = PwmInputConfig::DEFAULT_CHANNEL_PIN_OFFSET;
    _maxChannel = PwmInputConfig::MAX_CHANNELS;
}

// ============================================================================
// Static Utilities
// ============================================================================

int PwmInput::bandMatch(int pwmUs, const int* thresholds, int count,
                        int currentIndex, int hysteresisUs) {
    if (!thresholds || count <= 0 || pwmUs <= 0) {
        return -1;
    }
    
    int bestMatch = -1;
    int bestThreshold = -1;
    
    for (int i = 0; i < count; i++) {
        int threshold = thresholds[i];
        
        // Apply hysteresis advantage to current selection
        if (i == currentIndex) {
            threshold -= hysteresisUs;
        }
        
        // Check if PWM exceeds this threshold
        if (pwmUs >= threshold && threshold > bestThreshold) {
            bestMatch = i;
            bestThreshold = threshold;
        }
    }
    
    return bestMatch;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

PwmInput::~PwmInput() {
    end();
}

// ============================================================================
// Initialization
// ============================================================================

void PwmInput::begin(PwmInputType type, int pin) {
    // End any previous configuration
    if (_asyncMode && _pin >= 0) {
        detachInterrupt(digitalPinToInterrupt(_pin));
    }
    
    _type = type;
    _pin = pin;
    _asyncMode = false;
    _thresholdState = false;
    _lastThreshold = 0;
    _lastCallbackValue = 0;
    _pulseStartUs = 0;
    
    // Clear sample buffer
    memset((void*)_samples, 0, sizeof(_samples));
    _sampleIndex = 0;
    _sampleCount = 0;
    _latestUs = 0;
    _lastUpdateUs = 0;
    
    // Configure pin if valid
    if (pin >= 0 && type != PwmInputType::None && type != PwmInputType::Serial) {
        if (type == PwmInputType::Analog) {
            // ADC pins don't need pinMode on RP2040
        } else {
            pinMode(pin, INPUT);
        }
    }
}

void PwmInput::beginChannel(PwmInputType type, int channel) {
    int pin = channelToPin(channel);
    begin(type, pin);
}

bool PwmInput::beginAsync(PwmInputType type, int pin) {
    // Only PWM type supports async mode
    if (type != PwmInputType::Pwm || pin < 0) {
        return false;
    }
    
    // Initialize base state
    begin(type, pin);
    
    // Setup interrupt
    _asyncMode = true;
    _pulseStartUs = 0;
    
    // Attach interrupt on both edges
    attachInterruptParam(digitalPinToInterrupt(pin), isrHandler, CHANGE, this);
    
    return true;
}

bool PwmInput::beginAsyncChannel(PwmInputType type, int channel) {
    int pin = channelToPin(channel);
    return beginAsync(type, pin);
}

void PwmInput::end() {
    if (_asyncMode && _pin >= 0) {
        detachInterrupt(digitalPinToInterrupt(_pin));
    }
    _asyncMode = false;
    _type = PwmInputType::None;
    _pin = -1;
    _valueCallback = nullptr;
    _thresholdCallback = nullptr;
    reset();
}

void PwmInput::reset() {
    noInterrupts();
    memset((void*)_samples, 0, sizeof(_samples));
    _sampleIndex = 0;
    _sampleCount = 0;
    _latestUs = 0;
    _thresholdState = false;
    _lastCallbackValue = 0;
    interrupts();
}

void PwmInput::setThreshold(int thresholdUs, int hysteresisUs) {
    _configuredThreshold = thresholdUs;
    _configuredHysteresis = hysteresisUs;
}

// ============================================================================
// Update
// ============================================================================

void PwmInput::addSample(int value) {
    _samples[_sampleIndex] = value;
    _sampleIndex = (_sampleIndex + 1) % PwmInputConfig::SAMPLE_COUNT;
    if (_sampleCount < PwmInputConfig::SAMPLE_COUNT) {
        _sampleCount++;
    }
    _latestUs = value;
    _lastUpdateUs = micros();
}

void PwmInput::checkCallbacks(int oldAvg, int newAvg) {
    // Check value change callback
    if (_valueCallback) {
        int delta = newAvg - _lastCallbackValue;
        if (delta < 0) delta = -delta;
        
        if (delta >= PwmInputConfig::VALUE_CHANGE_THRESHOLD_US) {
            _lastCallbackValue = newAvg;
            _valueCallback(*this, newAvg);
        }
    }
    
    // Check threshold crossing callback
    if (_thresholdCallback && _configuredThreshold > 0) {
        bool wasAbove = _thresholdState;
        
        // Apply hysteresis
        if (_thresholdState) {
            // Currently above - need to drop below (threshold - hysteresis)
            if (newAvg < (_configuredThreshold - _configuredHysteresis)) {
                _thresholdState = false;
            }
        } else {
            // Currently below - need to rise above (threshold + hysteresis)
            if (newAvg > (_configuredThreshold + _configuredHysteresis)) {
                _thresholdState = true;
            }
        }
        
        // Fire callback if state changed
        if (_thresholdState != wasAbove) {
            _thresholdCallback(*this, _thresholdState);
        }
    }
}

int PwmInput::update() {
    // In async mode, just process callbacks - no blocking read needed
    if (_asyncMode) {
        int currentAvg = average();
        checkCallbacks(_lastCallbackValue, currentAvg);
        return currentAvg;
    }
    
    if (_pin < 0) return 0;
    
    int oldAvg = average();
    int value = 0;
    
    switch (_type) {
        case PwmInputType::Pwm:
            // Read PWM pulse width using pulseIn (blocking)
            value = pulseIn(_pin, HIGH, PwmInputConfig::TIMEOUT_US);
            break;
            
        case PwmInputType::Analog:
            // Read ADC and convert to PWM-equivalent (1000-2000us range)
            {
                int adcValue = analogRead(_pin);
                // Map 0-4095 to 1000-2000us
                value = 1000 + (adcValue * 1000 / 4095);
            }
            break;
            
        case PwmInputType::Serial:
            // Value is set externally via setValue()
            return _latestUs;
            
        case PwmInputType::None:
        default:
            return 0;
    }
    
    // Only add valid readings to sample buffer
    if (value > 0) {
        addSample(value);
        checkCallbacks(oldAvg, average());
    }
    
    return value;
}

void PwmInput::setValue(int valueUs) {
    if (valueUs > 0) {
        int oldAvg = average();
        addSample(valueUs);
        checkCallbacks(oldAvg, average());
    }
}

// ============================================================================
// Async ISR Handling
// ============================================================================

void IRAM_ATTR PwmInput::isrHandler(void* arg) {
    PwmInput* self = static_cast<PwmInput*>(arg);
    if (self) {
        self->handleInterrupt();
    }
}

void PwmInput::handleInterrupt() {
    unsigned long now = micros();
    
    if (digitalRead(_pin) == HIGH) {
        // Rising edge - start timing
        _pulseStartUs = now;
    } else {
        // Falling edge - calculate pulse width
        if (_pulseStartUs > 0) {
            unsigned long pulseWidth = now - _pulseStartUs;
            
            // Validate pulse width
            if (pulseWidth >= PwmInputConfig::MIN_PULSE_US && 
                pulseWidth <= PwmInputConfig::MAX_PULSE_US) {
                
                int oldAvg = average();
                addSample((int)pulseWidth);
                
                // Check callbacks (note: runs in ISR context!)
                checkCallbacks(oldAvg, average());
            }
            _pulseStartUs = 0;
        }
    }
}

// ============================================================================
// Getters
// ============================================================================

int PwmInput::average() const {
    if (_sampleCount == 0) return 0;
    
    int sum = 0;
    for (int i = 0; i < _sampleCount; i++) {
        sum += _samples[i];
    }
    return sum / _sampleCount;
}

bool PwmInput::aboveThreshold(int thresholdUs, int hysteresisUs) {
    int value = average();
    if (value == 0) {
        // No valid reading - maintain last state
        return _thresholdState;
    }
    
    // Check if threshold changed (for state tracking)
    if (thresholdUs != _lastThreshold) {
        _lastThreshold = thresholdUs;
    }
    
    // Apply hysteresis
    if (_thresholdState) {
        // Currently above - need to drop below (threshold - hysteresis) to go below
        if (value < (thresholdUs - hysteresisUs)) {
            _thresholdState = false;
        }
    } else {
        // Currently below - need to rise above (threshold + hysteresis) to go above
        if (value > (thresholdUs + hysteresisUs)) {
            _thresholdState = true;
        }
    }
    
    return _thresholdState;
}

unsigned long PwmInput::timeSinceUpdate() const {
    if (_lastUpdateUs == 0) {
        return ULONG_MAX;
    }
    return micros() - _lastUpdateUs;
}
