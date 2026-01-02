/*
 * PWM Control - Header
 * 
 * Object-oriented PWM input monitoring with averaging and hysteresis.
 * Used by engine_fx and gun_fx modules for RC receiver input processing.
 * 
 * Features:
 *   - Moving average filter for noise reduction
 *   - Hysteresis for threshold detection
 *   - Support for PWM, analog, and serial inputs
 *   - Abstract input channels mapped to GPIO pins (configurable)
 *   - Async event handling with callbacks
 *   - Hardware interrupt-based PWM measurement (non-blocking)
 * 
 * Usage:
 *   PwmInput input;
 *   input.begin(PwmInputType::Pwm, 10);  // GP10
 *   input.update();  // Call regularly
 *   int avg = input.average();
 *   bool above = input.aboveThreshold(1500, 50);
 * 
 * Async Usage:
 *   input.onValueChange([](PwmInput& in, int value) { ... });
 *   input.onThresholdCross([](PwmInput& in, bool above) { ... });
 *   input.beginAsync(PwmInputType::Pwm, 10);
 */

#ifndef PWM_CONTROL_H
#define PWM_CONTROL_H

#include <Arduino.h>
#include <functional>

// ============================================================================
// Constants
// ============================================================================

namespace PwmInputConfig {
    // Number of samples for moving average
    constexpr int SAMPLE_COUNT = 8;
    
    // Default hysteresis for threshold detection
    constexpr int DEFAULT_HYSTERESIS_US = 50;
    
    // PWM pulse timeout (25ms = 40Hz minimum)
    constexpr unsigned long TIMEOUT_US = 25000;
    
    // Maximum supported channels
    constexpr int MAX_CHANNELS = 16;
    
    // Default channel to GPIO pin offset (channel 1 = pin 10)
    constexpr int DEFAULT_CHANNEL_PIN_OFFSET = 9;
    
    // Minimum pulse width considered valid
    constexpr int MIN_PULSE_US = 800;
    
    // Maximum pulse width considered valid
    constexpr int MAX_PULSE_US = 2200;
    
    // Minimum change to trigger value change callback
    constexpr int VALUE_CHANGE_THRESHOLD_US = 10;
}

// ============================================================================
// Channel Mapping
// ============================================================================

/**
 * @brief Global channel-to-GPIO pin mapping configuration
 * 
 * Allows customizing how abstract channel numbers map to GPIO pins.
 * Default mapping: channel N -> GPIO (N + offset)
 */
class PwmInputMapping {
public:
    /**
     * @brief Set a simple offset-based mapping
     * @param offset Channel offset (channel 1 = pin (1 + offset))
     * @param maxChannel Maximum channel number
     */
    static void setOffset(int offset, int maxChannel = PwmInputConfig::MAX_CHANNELS);
    
    /**
     * @brief Set a custom channel-to-pin mapping
     * @param channelPins Array where index is (channel-1), value is GPIO pin
     * @param count Number of channels in the array
     */
    static void setMapping(const int* channelPins, int count);
    
    /**
     * @brief Convert channel number to GPIO pin
     * @param channel Channel number (1-based)
     * @return GPIO pin, or -1 if invalid
     */
    static int channelToPin(int channel);
    
    /**
     * @brief Convert GPIO pin to channel number
     * @param pin GPIO pin
     * @return Channel number (1-based), or -1 if not mapped
     */
    static int pinToChannel(int pin);
    
    /**
     * @brief Get maximum valid channel number
     */
    static int maxChannel() { return _maxChannel; }
    
    /**
     * @brief Reset to default offset-based mapping
     */
    static void reset();

private:
    static int _channelPins[PwmInputConfig::MAX_CHANNELS];
    static int _maxChannel;
    static bool _useCustomMapping;
    static int _offset;
};

// ============================================================================
// Input Types
// ============================================================================

enum class PwmInputType {
    None = 0,       // Disabled
    Pwm,            // Standard RC PWM (1000-2000us)
    Analog,         // Analog input (ADC)
    Serial          // Value set via setValue()
};

// Forward declaration
class PwmInput;

// ============================================================================
// Callback Types
// ============================================================================

/**
 * @brief Callback for value change events
 * @param input Reference to the PwmInput that changed
 * @param valueUs New value in microseconds
 */
using PwmInputValueCallback = std::function<void(PwmInput& input, int valueUs)>;

/**
 * @brief Callback for threshold crossing events
 * @param input Reference to the PwmInput
 * @param aboveThreshold true if crossed above, false if crossed below
 */
using PwmInputThresholdCallback = std::function<void(PwmInput& input, bool aboveThreshold)>;

// ============================================================================
// PwmInput Class
// ============================================================================

/**
 * @brief PWM input monitor with averaging and hysteresis
 */
class PwmInput {
public:
    PwmInput() = default;
    ~PwmInput();
    
    // Non-copyable (due to interrupt handling)
    PwmInput(const PwmInput&) = delete;
    PwmInput& operator=(const PwmInput&) = delete;

    // ========================================================================
    // Static Utilities
    // ========================================================================

    /**
     * @brief Convert abstract input channel (1-N) to GPIO pin
     * @param channel Input channel (1-based)
     * @return GPIO pin number, or -1 if invalid
     * @note Uses PwmInputMapping configuration
     */
    static int channelToPin(int channel) { return PwmInputMapping::channelToPin(channel); }

    /**
     * @brief Convert GPIO pin to abstract input channel
     * @param pin GPIO pin number
     * @return Input channel (1-based), or -1 if not a valid input pin
     * @note Uses PwmInputMapping configuration
     */
    static int pinToChannel(int pin) { return PwmInputMapping::pinToChannel(pin); }

    /**
     * @brief Match PWM value to a band from a list of thresholds
     * 
     * Finds the highest threshold that is exceeded by the PWM value.
     * Applies hysteresis to the currently selected band to prevent oscillation.
     * 
     * @param pwmUs Current PWM value in microseconds
     * @param thresholds Array of threshold values
     * @param count Number of thresholds
     * @param currentIndex Currently selected index (-1 if none)
     * @param hysteresisUs Hysteresis band in microseconds
     * @return Index of matched band (0 to count-1), or -1 if below all thresholds
     */
    static int bandMatch(int pwmUs, const int* thresholds, int count,
                         int currentIndex, int hysteresisUs);

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize PWM input monitor (synchronous/blocking mode)
     * @param type Input type (Pwm, Analog, Serial, None)
     * @param pin GPIO pin number (-1 to disable)
     */
    void begin(PwmInputType type, int pin);

    /**
     * @brief Initialize PWM input from abstract channel number (synchronous mode)
     * @param type Input type
     * @param channel Abstract input channel (1-based)
     */
    void beginChannel(PwmInputType type, int channel);

    /**
     * @brief Initialize PWM input in async mode with interrupts
     * 
     * Uses hardware interrupts to measure PWM without blocking.
     * Callbacks are invoked from interrupt context.
     * 
     * @param type Input type (Pwm only for async)
     * @param pin GPIO pin number
     * @return true if async setup succeeded
     */
    bool beginAsync(PwmInputType type, int pin);

    /**
     * @brief Initialize PWM input in async mode from channel number
     * @param type Input type
     * @param channel Abstract input channel (1-based)
     * @return true if async setup succeeded
     */
    bool beginAsyncChannel(PwmInputType type, int channel);

    /**
     * @brief End input monitoring and detach interrupts
     */
    void end();

    /**
     * @brief Reset sample buffer and state
     */
    void reset();

    // ========================================================================
    // Async Event Handlers
    // ========================================================================

    /**
     * @brief Set callback for value change events
     * 
     * Called when the averaged value changes by more than VALUE_CHANGE_THRESHOLD_US.
     * In async mode, called from interrupt context (keep it fast!).
     * 
     * @param callback Function to call on value change, or nullptr to disable
     */
    void onValueChange(PwmInputValueCallback callback) { _valueCallback = callback; }

    /**
     * @brief Set callback for threshold crossing events
     * 
     * Called when the value crosses the configured threshold.
     * Must call setThreshold() to configure the threshold value.
     * In async mode, called from interrupt context (keep it fast!).
     * 
     * @param callback Function to call on threshold cross, or nullptr to disable
     */
    void onThresholdCross(PwmInputThresholdCallback callback) { _thresholdCallback = callback; }

    /**
     * @brief Configure threshold for async threshold crossing detection
     * @param thresholdUs Threshold value in microseconds
     * @param hysteresisUs Hysteresis band (default: DEFAULT_HYSTERESIS_US)
     */
    void setThreshold(int thresholdUs, int hysteresisUs = PwmInputConfig::DEFAULT_HYSTERESIS_US);

    /**
     * @brief Check if running in async mode
     * @return true if using interrupt-based reading
     */
    bool isAsync() const { return _asyncMode; }

    // ========================================================================
    // Update
    // ========================================================================

    /**
     * @brief Update input reading (call regularly)
     * 
     * Reads the input and adds to sample buffer.
     * For blocking PWM reading, this may take up to TIMEOUT_US.
     * 
     * @return Latest reading in microseconds, 0 if no valid reading
     */
    int update();

    /**
     * @brief Set value directly (for Serial input type)
     * @param valueUs Value in microseconds
     */
    void setValue(int valueUs);

    // ========================================================================
    // Getters
    // ========================================================================

    /**
     * @brief Get latest raw reading
     * @return Latest reading in microseconds
     */
    int latest() const { return _latestUs; }

    /**
     * @brief Get averaged reading
     * @return Average of samples in microseconds, 0 if no samples
     */
    int average() const;

    /**
     * @brief Check if input is above threshold with hysteresis
     * 
     * Uses hysteresis to prevent oscillation around threshold.
     * Once state changes, the threshold shifts by hysteresis amount.
     * 
     * @param thresholdUs Threshold in microseconds
     * @param hysteresisUs Hysteresis band in microseconds
     * @return true if above threshold (with hysteresis)
     */
    bool aboveThreshold(int thresholdUs, int hysteresisUs = PwmInputConfig::DEFAULT_HYSTERESIS_US);

    /**
     * @brief Check if input has valid readings
     * @return true if at least one valid sample exists
     */
    bool isValid() const { return _sampleCount > 0; }

    /**
     * @brief Check if input is enabled (pin >= 0)
     * @return true if input is enabled
     */
    bool isEnabled() const { return _pin >= 0 && _type != PwmInputType::None; }

    /**
     * @brief Get the GPIO pin for this input
     * @return GPIO pin number, -1 if disabled
     */
    int pin() const { return _pin; }

    /**
     * @brief Get the input type
     * @return Input type
     */
    PwmInputType type() const { return _type; }

    /**
     * @brief Get time since last valid reading
     * @return Microseconds since last update, or ULONG_MAX if never updated
     */
    unsigned long timeSinceUpdate() const;

private:
    void addSample(int value);
    void checkCallbacks(int oldAvg, int newAvg);
    
    // ISR handler (static to work with attachInterrupt)
    #ifndef IRAM_ATTR
    #define IRAM_ATTR
    #endif
    static void IRAM_ATTR isrHandler(void* arg);
    void handleInterrupt();

    // Configuration
    int _pin = -1;
    PwmInputType _type = PwmInputType::None;
    bool _asyncMode = false;
    
    // Sample buffer for averaging
    volatile int _samples[PwmInputConfig::SAMPLE_COUNT] = {0};
    volatile int _sampleIndex = 0;
    volatile int _sampleCount = 0;
    
    // Latest reading
    volatile int _latestUs = 0;
    
    // Hysteresis state
    volatile bool _thresholdState = false;
    int _lastThreshold = 0;
    int _configuredThreshold = 0;
    int _configuredHysteresis = PwmInputConfig::DEFAULT_HYSTERESIS_US;
    
    // Timing
    volatile unsigned long _lastUpdateUs = 0;
    volatile unsigned long _pulseStartUs = 0;
    
    // Callbacks
    PwmInputValueCallback _valueCallback = nullptr;
    PwmInputThresholdCallback _thresholdCallback = nullptr;
    int _lastCallbackValue = 0;
};

#endif // PWM_CONTROL_H
