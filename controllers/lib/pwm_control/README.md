# PWM Control Library

Object-oriented PWM input monitoring library with moving average filtering, hysteresis threshold detection, configurable channel mapping, and async event handling for RC receiver signals.

## Features

- **PWM Input Monitoring**: Hardware interrupt-based PWM pulse measurement
- **Analog Input Support**: ADC reading converted to PWM-equivalent values
- **Serial Input Mode**: External value injection for remote control
- **Moving Average Filter**: Configurable sample count for noise reduction
- **Hysteresis Threshold Detection**: Prevents oscillation at threshold boundaries
- **Configurable Channel Mapping**: Custom channel-to-GPIO mapping support
- **Async Event Handling**: Non-blocking PWM measurement with callbacks
- **Timeout Detection**: Automatic signal validity checking

## Usage

### Basic PWM Input (Synchronous)

```cpp
#include <pwm_control.h>

PwmInput throttleInput;

void setup() {
    // Initialize on pin 2 for PWM input
    throttleInput.begin(PwmInputType::Pwm, 2);
}

void loop() {
    // Update and read averaged value (blocking)
    throttleInput.update();
    int pwm_us = throttleInput.average();
    
    // Check if above threshold with hysteresis
    if (throttleInput.aboveThreshold(1500, 50)) {
        // Above threshold
    }
}
```

### Async PWM Input (Non-Blocking)

```cpp
#include <pwm_control.h>

PwmInput throttleInput;

void onValueChange(PwmInput& input, int valueUs) {
    Serial.printf("Value changed: %d us\n", valueUs);
}

void onThresholdCross(PwmInput& input, bool above) {
    Serial.printf("Threshold %s\n", above ? "CROSSED UP" : "CROSSED DOWN");
}

void setup() {
    // Set up callbacks
    throttleInput.onValueChange(onValueChange);
    throttleInput.onThresholdCross(onThresholdCross);
    throttleInput.setThreshold(1500, 50);  // Configure threshold
    
    // Start async mode with interrupts
    throttleInput.beginAsync(PwmInputType::Pwm, 2);
}

void loop() {
    // Optional: call update() to trigger callbacks from main loop
    // (callbacks also fire from ISR context automatically)
    throttleInput.update();
    
    // Read current average anytime
    int pwm_us = throttleInput.average();
}
```

### Custom Channel Mapping

```cpp
#include <pwm_control.h>

void setup() {
    // Option 1: Simple offset mapping (channel N = pin N + offset)
    PwmInputMapping::setOffset(9, 10);  // ch1=pin10, ch2=pin11, ... ch10=pin19
    
    // Option 2: Custom mapping array
    int myMapping[] = {2, 3, 4, 5, 10, 11, 12, 13};  // ch1=2, ch2=3, etc.
    PwmInputMapping::setMapping(myMapping, 8);
    
    // Now use channel numbers
    PwmInput ch1Input;
    ch1Input.beginChannel(PwmInputType::Pwm, 1);  // Uses mapped pin
}
```

### Using RC Channel Numbers

```cpp
PwmInput ch1Input;

void setup() {
    // Default mapping: channel 1 = pin 10, channel 2 = pin 11, etc.
    ch1Input.beginChannel(PwmInputType::Pwm, 1);
    
    // Or async with channel
    ch1Input.beginAsyncChannel(PwmInputType::Pwm, 1);
}
```

### Band Matching (Rate Selection)

```cpp
#include <pwm_control.h>

PwmInput rateInput;
int currentRate = -1;

// Define rate thresholds (PWM values where rate changes)
const int rateThresholds[] = {1200, 1400, 1600, 1800};
const int numRates = 4;

void setup() {
    rateInput.begin(PwmInputType::Pwm, 10);
}

void loop() {
    rateInput.update();
    int pwm = rateInput.average();
    
    // Match PWM value to rate band with hysteresis
    int newRate = PwmInput::bandMatch(pwm, rateThresholds, numRates, currentRate, 50);
    
    if (newRate != currentRate) {
        currentRate = newRate;
        Serial.printf("Rate changed to: %d\n", currentRate);
    }
}
```

## API Reference

### PwmInputMapping (Static)

| Method | Description |
|--------|-------------|
| `setOffset(offset, maxChannel)` | Set offset-based channel mapping |
| `setMapping(pins[], count)` | Set custom channel-to-pin array |
| `channelToPin(channel)` | Convert channel number to GPIO pin |
| `pinToChannel(pin)` | Convert GPIO pin to channel number |
| `reset()` | Reset to default mapping |

### PwmInput

| Method | Description |
|--------|-------------|
| `begin(type, pin)` | Initialize in synchronous mode |
| `beginChannel(type, channel)` | Initialize using channel number |
| `beginAsync(type, pin)` | Initialize in async/interrupt mode |
| `beginAsyncChannel(type, channel)` | Async init using channel number |
| `end()` | Stop monitoring and detach interrupts |
| `reset()` | Clear sample buffer |
| `update()` | Read input (blocking in sync mode) |
| `setValue(valueUs)` | Set value directly (Serial mode) |
| `average()` | Get filtered average value |
| `latest()` | Get most recent raw reading |
| `aboveThreshold(threshold, hysteresis)` | Check threshold with hysteresis |
| `isValid()` | Check if valid samples exist |
| `isEnabled()` | Check if input is configured |
| `timeSinceUpdate()` | Time since last valid reading |

### Async Callbacks

| Method | Description |
|--------|-------------|
| `onValueChange(callback)` | Called when value changes significantly |
| `onThresholdCross(callback)` | Called when threshold is crossed |
| `setThreshold(value, hysteresis)` | Configure threshold for callbacks |

### Static Utilities

| Method | Description |
|--------|-------------|
| `channelToPin(channel)` | Convert channel to pin |
| `pinToChannel(pin)` | Convert pin to channel |
| `bandMatch(pwm, thresholds, count, current, hysteresis)` | Match value to band |

## Configuration

Constants in `PwmInputConfig` namespace:

| Constant | Default | Description |
|----------|---------|-------------|
| `SAMPLE_COUNT` | 8 | Moving average window size |
| `DEFAULT_HYSTERESIS_US` | 50 | Default hysteresis band |
| `TIMEOUT_US` | 25000 | PWM pulse timeout |
| `MAX_CHANNELS` | 16 | Maximum channel count |
| `DEFAULT_CHANNEL_PIN_OFFSET` | 9 | Default channel offset |
| `MIN_PULSE_US` | 800 | Minimum valid pulse |
| `MAX_PULSE_US` | 2200 | Maximum valid pulse |
| `VALUE_CHANGE_THRESHOLD_US` | 10 | Callback trigger threshold |

## Input Types

| Type | Description |
|------|-------------|
| `PwmInputType::None` | Disabled |
| `PwmInputType::Pwm` | Standard RC PWM (1000-2000µs) |
| `PwmInputType::Analog` | ADC input mapped to PWM range |
| `PwmInputType::Serial` | External value via `setValue()` |
