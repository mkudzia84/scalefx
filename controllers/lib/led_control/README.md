# LED Control Library

Simple LED control on GPIO pins with on/off functionality.

## Features

- **On/Off Control**: Simple digital output control
- **Active-Low Support**: Configure for active-low LEDs
- **State Tracking**: Query current LED state
- **Toggle Function**: Easy state inversion

## Usage

### Basic LED Control

```cpp
#include <led_control.h>

LedControl statusLed;

void setup() {
    // Initialize LED on pin 13 (active-high, default)
    statusLed.begin(13);
}

void loop() {
    statusLed.on();
    delay(500);
    statusLed.off();
    delay(500);
}
```

### Active-Low LED

```cpp
LedControl led;

void setup() {
    // LED is on when pin is LOW
    led.begin(13, true);  // active-low mode
    
    led.on();   // Pin goes LOW, LED lights up
    led.off();  // Pin goes HIGH, LED turns off
}
```

### Toggle and State Query

```cpp
LedControl led;

void setup() {
    led.begin(13);
}

void loop() {
    led.toggle();
    
    if (led.isOn()) {
        Serial.println("LED is ON");
    } else {
        Serial.println("LED is OFF");
    }
    
    delay(1000);
}
```

### Set State Directly

```cpp
bool errorCondition = checkForError();
led.set(errorCondition);  // LED on if error, off otherwise
```

### Multiple LEDs

```cpp
LedControl leds[3];

void setup() {
    int pins[] = {13, 14, 15};
    for (int i = 0; i < 3; i++) {
        leds[i].begin(pins[i]);
    }
}

void setAllOn() {
    for (int i = 0; i < 3; i++) {
        leds[i].on();
    }
}

void setAllOff() {
    for (int i = 0; i < 3; i++) {
        leds[i].off();
    }
}
```

## API Reference

### Initialization

| Method | Description |
|--------|-------------|
| `begin(pin, activeLow)` | Initialize LED on pin, optional active-low mode |
| `end()` | Release GPIO pin |
| `isAttached()` | Check if LED is initialized |

### Control

| Method | Description |
|--------|-------------|
| `on()` | Turn LED on |
| `off()` | Turn LED off |
| `toggle()` | Toggle LED state |
| `set(state)` | Set LED state (true=on, false=off) |

### State

| Method | Description |
|--------|-------------|
| `isOn()` | Returns true if LED is on |
| `isOff()` | Returns true if LED is off |
| `pin()` | Get GPIO pin number |
| `isActiveLow()` | Check if active-low mode |
