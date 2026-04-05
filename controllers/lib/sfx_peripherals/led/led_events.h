/*
 * LED Events Library - Header
 * 
 * Event-based LED behavior animations.
 * 
 * Events:
 *   - LedOn: Constant on (with optional power saving PWM)
 *   - LedOff: Constant off for a duration
 *   - LedFlashing: On/Off at specified frequency
 *   - LedFadeIn: Fade from off to on
 *   - LedFadeOut: Fade from on to off
 *   - LedFading: Sinusoidal breathing/pulsing effect
 *   - LedBeacon: Rotating beacon (brief flash, extended off)
 * 
 * Usage:
 *   LedControl led;
 *   led.begin(13, false, true);  // PWM enabled
 *   led.startEvent(new LedFlashing(500, 2000));  // 500ms interval for 2 seconds
 *   // In loop:
 *   led.update();
 */

#ifndef LED_EVENTS_H
#define LED_EVENTS_H

#include <Arduino.h>
#include <cmath>

// ============================================================================
// ILedEvent Interface
// ============================================================================

/**
 * @brief Abstract base class for LED behavior events
 * 
 * Implement this interface to create custom LED animations.
 * The update() method is called repeatedly and should return
 * the desired LED intensity (0-100) or -1 when the event is complete.
 */
class ILedEvent {
public:
    virtual ~ILedEvent() = default;
    
    /**
     * @brief Start the event
     * @param now Current time in milliseconds (from millis())
     */
    virtual void start(uint32_t now) = 0;
    
    /**
     * @brief Update the event and get current LED intensity
     * @param now Current time in milliseconds
     * @return LED intensity 0-100, or -1 if event is complete
     */
    virtual int16_t update(uint32_t now) = 0;
    
    /**
     * @brief Check if the event is complete
     * @return true if event has finished
     */
    virtual bool isComplete() const = 0;
    
    /**
     * @brief Get event duration in milliseconds
     */
    virtual uint32_t duration() const = 0;
    
    /**
     * @brief Get event name for debugging
     */
    virtual const char* name() const = 0;

    /**
     * @brief Get numeric event type ID (matches LightFxEventType protocol constants)
     *
     * Defined independently of lightfx.h to avoid coupling the animation
     * library to the serial protocol layer.  Values: ON=0, OFF=1, FLASH=2,
     * FADE_IN=3, FADE_OUT=4, FADING=5, BEACON=6.
     */
    virtual uint8_t typeId() const = 0;
};

// ============================================================================
// LedOn Event - Constant on or high-frequency PWM for power saving
// ============================================================================

/**
 * @brief LED constantly on, optionally with power-saving PWM
 * 
 * When powerSaving is enabled, the LED pulses at high frequency (1kHz+)
 * which appears constant to human eyes but reduces power consumption.
 */
class LedOn : public ILedEvent {
public:
    /**
     * @brief Create an LED on event
     * @param durationMs Duration in milliseconds (0 = infinite)
     * @param brightness LED brightness 0-100 (default 100 = full)
     * @param powerSaving If true, use PWM even at full brightness for power saving
     * @param pwmDuty Power saving duty 0-100 when power saving (default 78%)
     */
    explicit LedOn(uint32_t durationMs = 0, uint8_t brightness = 100, 
                   bool powerSaving = false, uint8_t pwmDuty = 78)
        : _durationMs(durationMs), _brightness(brightness), 
          _powerSaving(powerSaving), _pwmDuty(pwmDuty) {}
    
    void start(uint32_t now) override { 
        _startTime = now; 
        _complete = false;
    }
    
    int16_t update(uint32_t now) override {
        // Check duration
        if (_durationMs > 0 && (now - _startTime) >= _durationMs) {
            _complete = true;
            return -1;
        }
        
        if (_powerSaving && _brightness == 100) {
            return _pwmDuty;  // Power-saving mode
        }
        return _brightness;
    }
    
    bool isComplete() const override { return _complete; }
    uint32_t duration() const override { return _durationMs; }
    const char* name() const override { return "LedOn"; }
    uint8_t typeId() const override { return 0; }  // ON

private:
    uint32_t _startTime = 0;
    uint32_t _durationMs;
    uint8_t _brightness;
    bool _powerSaving;
    uint8_t _pwmDuty;
    bool _complete = false;
};

// ============================================================================
// LedOff Event - Constant off for a duration
// ============================================================================

/**
 * @brief LED constantly off for a specified duration
 * 
 * Useful for creating patterns with explicit off periods.
 */
class LedOff : public ILedEvent {
public:
    /**
     * @brief Create an LED off event
     * @param durationMs Duration in milliseconds (0 = infinite)
     */
    explicit LedOff(uint32_t durationMs = 0)
        : _durationMs(durationMs) {}
    
    void start(uint32_t now) override { 
        _startTime = now; 
        _complete = false;
    }
    
    int16_t update(uint32_t now) override {
        // Check duration
        if (_durationMs > 0 && (now - _startTime) >= _durationMs) {
            _complete = true;
            return -1;
        }
        return 0;  // Always off
    }
    
    bool isComplete() const override { return _complete; }
    uint32_t duration() const override { return _durationMs; }
    const char* name() const override { return "LedOff"; }
    uint8_t typeId() const override { return 1; }  // OFF

private:
    uint32_t _startTime = 0;
    uint32_t _durationMs;
    bool _complete = false;
};

// ============================================================================
// LedFlashing Event - On/Off at specified frequency
// ============================================================================

/**
 * @brief LED flashing on and off at a specified interval
 */
class LedFlashing : public ILedEvent {
public:
    /**
     * @brief Create a flashing LED event
     * @param intervalMs On/Off interval in milliseconds (full cycle = 2x interval)
     * @param durationMs Total duration in milliseconds (0 = infinite)
     * @param brightness LED brightness when on (0-100)
     * @param dutyPercent Duty cycle percentage (default 50 = equal on/off)
     */
    explicit LedFlashing(uint16_t intervalMs, uint32_t durationMs = 0,
                         uint8_t brightness = 100, uint8_t dutyPercent = 50)
        : _intervalMs(intervalMs), _durationMs(durationMs),
          _brightness(brightness), _dutyPercent(dutyPercent) {}
    
    void start(uint32_t now) override { 
        _startTime = now; 
        _complete = false;
    }
    
    int16_t update(uint32_t now) override {
        // Check duration
        if (_durationMs > 0 && (now - _startTime) >= _durationMs) {
            _complete = true;
            return -1;
        }
        
        uint32_t elapsed = now - _startTime;
        uint32_t cycleTime = elapsed % (_intervalMs * 2);
        uint32_t onTime = (_intervalMs * 2 * _dutyPercent) / 100;
        return (cycleTime < onTime) ? _brightness : 0;
    }
    
    bool isComplete() const override { return _complete; }
    uint32_t duration() const override { return _durationMs; }
    const char* name() const override { return "LedFlashing"; }
    uint8_t typeId() const override { return 2; }  // FLASH
    
    void setInterval(uint16_t intervalMs) { _intervalMs = intervalMs; }
    uint16_t interval() const { return _intervalMs; }

private:
    uint32_t _startTime = 0;
    uint16_t _intervalMs;
    uint32_t _durationMs;
    uint8_t _brightness;
    uint8_t _dutyPercent;
    bool _complete = false;
};

// ============================================================================
// LedFadeIn Event - Fade from off to on over time
// ============================================================================

/**
 * @brief LED fades in from off to target brightness
 */
class LedFadeIn : public ILedEvent {
public:
    /**
     * @brief Create a fade-in event
     * @param durationMs Fade duration in milliseconds
     * @param targetBrightness Final brightness (0-100, default 100)
     */
    explicit LedFadeIn(uint32_t durationMs, uint8_t targetBrightness = 100)
        : _durationMs(durationMs), _targetBrightness(targetBrightness) {}
    
    void start(uint32_t now) override { 
        _startTime = now; 
        _complete = false;
    }
    
    int16_t update(uint32_t now) override {
        if (_complete) return -1;
        
        uint32_t elapsed = now - _startTime;
        if (elapsed >= _durationMs) {
            _complete = true;
            return _targetBrightness;
        }
        
        // Linear fade
        return (uint16_t)(_targetBrightness * elapsed / _durationMs);
    }
    
    bool isComplete() const override { return _complete; }
    uint32_t duration() const override { return _durationMs; }
    const char* name() const override { return "LedFadeIn"; }
    uint8_t typeId() const override { return 3; }  // FADE_IN

private:
    uint32_t _startTime = 0;
    uint32_t _durationMs;
    uint8_t _targetBrightness;
    bool _complete = false;
};

// ============================================================================
// LedFadeOut Event - Fade from on to off over time
// ============================================================================

/**
 * @brief LED fades out from current/start brightness to off
 */
class LedFadeOut : public ILedEvent {
public:
    /**
     * @brief Create a fade-out event
     * @param durationMs Fade duration in milliseconds
     * @param startBrightness Initial brightness (0-100, default 100)
     */
    explicit LedFadeOut(uint32_t durationMs, uint8_t startBrightness = 100)
        : _durationMs(durationMs), _startBrightness(startBrightness) {}
    
    void start(uint32_t now) override { 
        _startTime = now; 
        _complete = false;
    }
    
    int16_t update(uint32_t now) override {
        if (_complete) return -1;
        
        uint32_t elapsed = now - _startTime;
        if (elapsed >= _durationMs) {
            _complete = true;
            return 0;
        }
        
        // Linear fade
        return _startBrightness - (uint16_t)(_startBrightness * elapsed / _durationMs);
    }
    
    bool isComplete() const override { return _complete; }
    uint32_t duration() const override { return _durationMs; }
    const char* name() const override { return "LedFadeOut"; }
    uint8_t typeId() const override { return 4; }  // FADE_OUT

private:
    uint32_t _startTime = 0;
    uint32_t _durationMs;
    uint8_t _startBrightness;
    bool _complete = false;
};

// ============================================================================
// LedFading Event - Sinusoidal beacon/breathing effect
// ============================================================================

/**
 * @brief LED with sinusoidal fading effect (rotating beacon simulation)
 * 
 * Creates a smooth breathing/pulsing effect using a sine wave.
 * Can simulate a rotating beacon light.
 */
class LedFading : public ILedEvent {
public:
    /**
     * @brief Create a sinusoidal fading event
     * @param cycleDurationMs Duration of one full fade cycle in milliseconds
     * @param durationMs Total event duration in milliseconds (0 = infinite)
     * @param minBrightness Minimum brightness (0-100, default 0)
     * @param maxBrightness Maximum brightness (0-100, default 100)
     */
    explicit LedFading(uint32_t cycleDurationMs, uint32_t durationMs = 0,
                       uint8_t minBrightness = 0, uint8_t maxBrightness = 100)
        : _cycleDurationMs(cycleDurationMs), _durationMs(durationMs),
          _minBrightness(minBrightness), _maxBrightness(maxBrightness) {}
    
    void start(uint32_t now) override { 
        _startTime = now; 
        _complete = false;
    }
    
    int16_t update(uint32_t now) override {
        // Check duration
        if (_durationMs > 0 && (now - _startTime) >= _durationMs) {
            _complete = true;
            return -1;
        }
        
        uint32_t elapsed = now - _startTime;
        
        // Calculate phase (0 to 2*PI) based on cycle position
        float phase = (float)(elapsed % _cycleDurationMs) / _cycleDurationMs * 2.0f * PI;
        
        // Sinusoidal intensity: use (1 - cos(x)) / 2 for smooth 0->1->0 transition
        float normalized = (1.0f - cosf(phase)) / 2.0f;
        
        // Map to brightness range
        uint8_t range = _maxBrightness - _minBrightness;
        return _minBrightness + (uint8_t)(normalized * range);
    }
    
    bool isComplete() const override { return _complete; }
    uint32_t duration() const override { return _durationMs; }
    const char* name() const override { return "LedFading"; }
    uint8_t typeId() const override { return 5; }  // FADING
    
    void setCycleDuration(uint32_t durationMs) { _cycleDurationMs = durationMs; }
    uint32_t cycleDuration() const { return _cycleDurationMs; }

private:
    uint32_t _startTime = 0;
    uint32_t _cycleDurationMs;
    uint32_t _durationMs;
    uint8_t _minBrightness;
    uint8_t _maxBrightness;
    bool _complete = false;
};

// ============================================================================
// LedBeacon Event - Rotating beacon with brief flash and extended off
// ============================================================================

/**
 * @brief Rotating beacon effect — brief peaked flash with extended dark period
 * 
 * Simulates a rotating beacon light (e.g., aircraft anti-collision).
 * A short cosine-shaped pulse occupies a fraction of the cycle, the rest
 * is held at minimum brightness.
 * 
 *   ▄▄                    ▄▄                      Beacon (flashPercent=15)
 *  ▟  ▜                  ▟  ▜
 * ▐    ▌                ▐    ▌
 * ─────────────────────────────────  time →
 *   flash │   off period   │ flash
 * 
 * Compare with LedFading (equal ramp/fall, no extended off):
 *   ▄▄▄▄   ▄▄▄▄   ▄▄▄▄     Fading (sinusoidal, 50% on)
 *  ▟    ▜ ▟    ▜ ▟    ▜
 * ─────────────────────────  time →
 */
class LedBeacon : public ILedEvent {
public:
    /**
     * @brief Create a rotating beacon event
     * @param cycleDurationMs Duration of one full rotation in milliseconds (e.g., 2000)
     * @param durationMs Total event duration in milliseconds (0 = infinite)
     * @param flashPercent Percentage of cycle occupied by flash pulse (1-50, default 15)
     * @param minBrightness Brightness during off period (0-100, default 0)
     * @param maxBrightness Peak brightness during flash (0-100, default 100)
     */
    explicit LedBeacon(uint32_t cycleDurationMs, uint32_t durationMs = 0,
                       uint8_t flashPercent = 15,
                       uint8_t minBrightness = 0, uint8_t maxBrightness = 100)
        : _cycleDurationMs(cycleDurationMs), _durationMs(durationMs),
          _flashPercent(flashPercent < 1 ? 1 : (flashPercent > 50 ? 50 : flashPercent)),
          _minBrightness(minBrightness), _maxBrightness(maxBrightness) {}
    
    void start(uint32_t now) override { 
        _startTime = now; 
        _complete = false;
    }
    
    int16_t update(uint32_t now) override {
        // Check duration
        if (_durationMs > 0 && (now - _startTime) >= _durationMs) {
            _complete = true;
            return -1;
        }
        
        uint32_t elapsed = now - _startTime;
        uint32_t cyclePos = elapsed % _cycleDurationMs;
        
        // Flash window duration
        uint32_t flashWindow = (_cycleDurationMs * _flashPercent) / 100;
        
        if (cyclePos >= flashWindow) {
            // Off period — extended dark
            return _minBrightness;
        }
        
        // Inside flash window: cosine pulse  (1 - cos(θ)) / 2, θ: 0 → 2π
        float phase = (float)cyclePos / flashWindow * 2.0f * PI;
        float normalized = (1.0f - cosf(phase)) / 2.0f;
        
        uint8_t range = _maxBrightness - _minBrightness;
        return _minBrightness + (uint8_t)(normalized * range);
    }
    
    bool isComplete() const override { return _complete; }
    uint32_t duration() const override { return _durationMs; }
    const char* name() const override { return "LedBeacon"; }
    uint8_t typeId() const override { return 6; }  // BEACON
    
    void setCycleDuration(uint32_t durationMs) { _cycleDurationMs = durationMs; }
    uint32_t cycleDuration() const { return _cycleDurationMs; }

private:
    uint32_t _startTime = 0;
    uint32_t _cycleDurationMs;
    uint32_t _durationMs;
    uint8_t _flashPercent;
    uint8_t _minBrightness;
    uint8_t _maxBrightness;
    bool _complete = false;
};

#endif // LED_EVENTS_H
