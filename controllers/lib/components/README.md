# Components Library

Reusable hardware component drivers for ScaleFX controllers. This library provides generic, controller-agnostic building blocks for I2C devices, LEDs, PWM inputs, and servos.

## Architecture

```
components/
├── Battery Monitoring
│   └── battery_monitor.h/.cpp  ADC battery voltage with cell detection
├── I2C Device Framework
│   ├── i2c_device.h/.cpp      Base class for all I2C peripherals
│   └── ina226.h/.cpp          TI INA226 power monitor driver
├── LED Control
│   ├── led_control.h/.cpp     GPIO LED on/off/PWM brightness
│   ├── led_events.h           Event-based LED animations (ILedEvent)
│   └── led_event_seq.h/.cpp   Sequenced LED event playback
├── Indicator LEDs
│   └── indicator_leds.h/.cpp  Connection/error LED state machine
├── Pico Server
│   └── pico_server.h/.cpp     Common server controller boilerplate
├── PWM Input
│   └── pwm_control.h/.cpp     RC PWM input with averaging/hysteresis
└── Servo Output
    └── srv_control.h/.cpp     Servo motion profiling with jerk effects
```

## Component Reuse Guidelines

> **Rule:** When implementing controller firmware, always check the components library first. If a generic hardware abstraction exists here, use it instead of writing controller-specific code.

> **Rule:** When adding support for a new hardware peripheral (sensor, actuator, display, etc.), create the driver here as a reusable component — not embedded in controller code. Controller firmware should only contain controller-specific logic and protocol handling.

---

## Battery Monitoring

### BatteryMonitor Class

ADC-based battery voltage monitor for LiPo and Li-Ion packs. Reads voltage through a resistor divider, auto-detects cell count (1S–6S), and monitors for dangerous low-voltage conditions.

**Features:**
- Oversampled ADC reading with configurable divider ratio
- LiPo and Li-Ion chemistry profiles with per-cell thresholds
- Auto-detect series cell count from measured voltage
- Low and critical voltage callbacks with hysteresis
- Estimated state-of-charge percentage (0–100%)

**Basic usage:**
```cpp
#include <battery_monitor.h>

BatteryMonitor battery;
battery.begin(29, 6.0f);                   // GP29, ÷6 divider (default: LiPo)
battery.update();                           // Call in loop()

uint16_t v = battery.voltage_mV();          // Total pack voltage (mV)
uint16_t cv = battery.cellVoltage_mV();     // Per-cell average (mV)
uint8_t cells = battery.cellCount();        // Detected cells (1-6)
uint8_t pct = battery.percentage();         // Estimated SOC (0-100%)
bool low = battery.isLow();                 // Below low threshold?
bool crit = battery.isCritical();           // Below critical threshold?
```

**Li-Ion chemistry:**
```cpp
BatteryMonitor battery;
battery.begin(29, 6.0f, BatteryChemistry::LI_ION);  // Li-Ion thresholds
```

**Manual cell count (for deeply discharged packs):**
```cpp
BatteryMonitor battery;
battery.begin(29, 6.0f);
battery.setCellCount(3);  // Force 3S (overrides auto-detect)
```

**Low-voltage alerts:**
```cpp
BatteryMonitor battery;
battery.begin(29, 6.0f);

battery.onLowVoltage([](uint16_t voltage_mV, uint8_t cells) {
    Serial.printf("WARNING: Low battery %dmV (%dS)\n", voltage_mV, cells);
});

battery.onCriticalVoltage([](uint16_t voltage_mV, uint8_t cells) {
    Serial.printf("CRITICAL: Battery %dmV (%dS) - shut down!\n", voltage_mV, cells);
    performSafeShutdown();
});
```

**Chemistry profiles (per-cell defaults):**

| Chemistry | Full Charge | Nominal | Low Warning | Critical |
|-----------|-------------|---------|-------------|----------|
| LiPo      | 4200 mV     | 3700 mV | 3200 mV     | 3000 mV  |
| Li-Ion    | 4200 mV     | 3600 mV | 3200 mV     | 2800 mV  |

Thresholds can be overridden per-cell with `setLowThreshold_mV()` and `setCriticalThreshold_mV()`.

**Cell count auto-detection:** Uses `round(voltage / nominal_per_cell)` for the configured chemistry. Reliable when battery is above ~3.3V/cell. For deeply discharged packs, use `setCellCount()` to set manually.

**Hardware notes:**
- RP2040 ADC: 12-bit resolution, 3.3V reference
- Divider multiplier = (R_top + R_bottom) / R_bottom (e.g., 50kΩ/10kΩ = 6.0)
- Maximum measurable voltage = 3.3V × divider (e.g., ÷6 → 19.8V max → supports up to 4S LiPo)

**Unit Convention:** All methods include the unit and magnitude as a suffix
(e.g., `_mV`, `_ms`) per ScaleFX Rule 4: Physical Units in Code.

---

## I2C Device Framework

### I2CDevice Base Class

All I2C device drivers inherit from `I2CDevice`, which provides:

- **Register I/O:** `readRegister8()`, `readRegister16()`, `writeRegister8()`, `writeRegister16()`, `readBytes()`, `writeBytes()`
- **Bus utilities:** `probe()` (ACK check), `scan()` (range scan)
- **Error tracking:** `errorCount()`, `lastError()` (I2CError enum)
- **Init pattern:** `begin()` → probe → `identify()` (virtual)

**Creating a new device driver:**
```cpp
#include "i2c_device.h"

class MyDevice : public I2CDevice {
public:
    bool identify() override {
        return readRegister8(WHO_AM_I_REG) == EXPECTED_ID;
    }

    bool begin(TwoWire& wire, uint8_t address = 0x68) {
        if (!I2CDevice::begin(wire, address)) return false;
        // Device-specific initialization...
        return true;
    }

    uint16_t readSensor() {
        return readRegister16(DATA_REG);
    }
};
```

**Generic bus scan (any device):**
```cpp
uint8_t found[128];
uint8_t count = I2CDevice::scan(Wire, 0x08, 0x77, found, 128);
for (uint8_t i = 0; i < count; i++) {
    Serial.printf("Device at 0x%02X\n", found[i]);
}
```

**Multiple devices sharing one bus:**
```cpp
INA226 powerMon;
MyDevice sensor;

void setup() {
    Wire.begin();
    powerMon.begin(Wire, 0x40, 0.1f, 3.2f);
    sensor.begin(Wire, 0x68);
}

void loop() {
    powerMon.update();
    uint16_t data = sensor.readSensor();
    if (powerMon.errorCount() > 0) { /* handle */ }
}
```

---

### INA226 Power Monitor

Texas Instruments INA226 high-precision current/power monitor (TI SBOS547).
Extends `I2CDevice` with INA226-specific calibration, measurement, and identification.

**Features:**
- Bus voltage measurement (0–36V, LSB = 1.25 mV, §7.6.2)
- Shunt voltage measurement (±81.92 mV, LSB = 2.5 µV, §7.6.1)
- Current calculation via calibration register (§7.6.3)
- Power calculation (LSB = 25 × Current_LSB, §7.6.4)
- Configurable averaging and conversion time
- Multi-instance support (up to 16 devices per I2C bus)
- Bus scan with MFG_ID + DIE_ID verification

**Single Monitor:**
```cpp
#include <ina226.h>

INA226 monitor;
monitor.begin(Wire, 0x40, 0.1f, 3.2f);  // 100mΩ shunt, 3.2A max
monitor.update();

float v = monitor.busVoltage_mV();     // millivolts
float i = monitor.current_mA();        // milliamps
float p = monitor.power_mW();          // milliwatts
float s = monitor.shuntVoltage_uV();   // microvolts
```

**Multiple Monitors (config struct):**
```cpp
#include <ina226.h>

INA226Config configs[] = {
    { .address = INA226Address::GND_GND, .shuntResistance_ohms = 0.1f,
      .maxCurrent_A = 3.2f, .channel = 0 },
    { .address = INA226Address::GND_VS,  .shuntResistance_ohms = 0.05f,
      .maxCurrent_A = 6.4f, .channel = 1 },
};

INA226 monitors[2];

void setup() {
    Wire.begin();
    for (int i = 0; i < 2; i++) {
        monitors[i].begin(Wire, configs[i]);
    }
}
```

**Unit Convention:** All methods include the unit and magnitude as a suffix
(e.g., `_mV`, `_mA`, `_mW`, `_uV`) per ScaleFX Rule 4: Physical Units in Code.

---

## LED Control

### LedControl Class

Simple LED control on GPIO pins with on/off, toggle, active-low support, and PWM brightness.

```cpp
#include <led_control.h>

LedControl statusLed;
statusLed.begin(13);            // GPIO 13 (active-high)
statusLed.on();                 // Turn on
statusLed.toggle();             // Toggle
statusLed.setBrightness(128);   // Half brightness (PWM mode)
```

**Active-Low LED:**
```cpp
LedControl led;
led.begin(13, true);  // active-low mode
led.on();   // Pin goes LOW, LED lights up
```

**API:**

| Method | Description |
|--------|-------------|
| `begin(pin, activeLow, usePwm)` | Initialize LED on pin |
| `end()` | Release GPIO pin |
| `on()` / `off()` / `toggle()` | Basic control |
| `set(state)` | Set LED state |
| `setBrightness(0-255)` | PWM brightness (requires `usePwm`) |
| `isOn()` / `isOff()` | State query |

### LED Events (ILedEvent)

Event-based LED behavior animations for creating patterns:

| Event | Description |
|-------|-------------|
| `LedOn` | Constant on (with optional power-saving PWM) |
| `LedOff` | Constant off for a duration |
| `LedFlashing` | On/Off at specified frequency |
| `LedFadeIn` | Fade from off to on |
| `LedFadeOut` | Fade from on to off |
| `LedFading` | Sinusoidal beacon/breathing effect |

### LedEventSeq Class

Manages a looping sequence of LED events (up to 24):

```cpp
#include <led_event_seq.h>

LedControl led;
led.begin(13, false, true);  // PWM enabled

LedEventSeq seq;
seq.attachLed(&led);
seq.add(new LedFlashing(500, 2000));
seq.add(new LedFadeIn(1000));
seq.add(new LedFadeOut(1000));
seq.start();

// In loop():
seq.update();
```

---

## PWM Input

### PwmInput Class

RC PWM input monitoring with hardware interrupts, moving average filtering, and hysteresis.

**Features:**
- Hardware interrupt-based PWM measurement (non-blocking)
- Moving average filter (8 samples)
- Hysteresis threshold detection
- Configurable channel-to-GPIO mapping
- Async callbacks for value changes and threshold crossings
- Band matching for rate selection

```cpp
#include <pwm_control.h>

PwmInput throttle;
throttle.begin(PwmInputType::Pwm, 10);  // GP10
throttle.update();
int avg = throttle.average();
bool above = throttle.aboveThreshold(1500, 50);
```

**Async mode with callbacks:**
```cpp
PwmInput throttle;

void onValueChange(PwmInput& input, int valueUs) {
    Serial.printf("Value: %d us\n", valueUs);
}

void setup() {
    throttle.onValueChange(onValueChange);
    throttle.beginAsync(PwmInputType::Pwm, 10);
}
```

**Band matching (rate selection):**
```cpp
const int thresholds[] = {1200, 1400, 1600, 1800};
int rate = PwmInput::bandMatch(pwm, thresholds, 4, currentRate, 50);
```

---

## Servo Output

### ServoControl Class

Servo output control with trapezoidal motion profiling, acceleration/deceleration, and jerk effects.

**Features:**
- Trapezoidal velocity profiles (smooth motion)
- Configurable max speed, acceleration, deceleration
- Position limits with clamping
- Jerk offset for recoil simulation
- Target reached callbacks

```cpp
#include <srv_control.h>

ServoControl servo;
servo.begin(1, 500, 2500);                  // Pin 1, limits 500-2500µs
servo.setMotionProfile(4000, 8000, 8000);   // Speed, accel, decel
servo.setTarget(1800);                      // Moves smoothly
servo.update();                             // Call frequently
```

**Jerk offset (recoil simulation):**
```cpp
servo.applyJerk(-50);   // Temporary offset
servo.clearJerk();      // Remove offset
```

**Motion profile theory:** The servo uses trapezoidal velocity profiling with acceleration, cruise, and deceleration phases. For short moves, velocity may not reach max speed before deceleration begins.

---

## Indicator LEDs

### IndicatorLedManager Class

Manages the two standardized indicator LEDs (GP13=connection, GP14=error) present on all ScaleFX Pico server boards. Encapsulates the blink patterns and state transitions for connection status and error reporting.

**States:**

| LED | Pin | Waiting for INIT | Connected | Connection Lost |
|-----|-----|-----------------|-----------|----------------|
| Connection | GP13 | Blink 500ms | Solid ON | OFF |
| Error | GP14 | OFF | OFF | Blink 200ms (if error) |

**Basic usage:**
```cpp
#include <indicator_leds.h>

IndicatorLedManager indicators;
indicators.begin(13, 14);              // GP13=connection, GP14=error

indicators.setConnected(true);         // INIT received
indicators.setErrorCondition(true);    // Module-specific error
indicators.setWarningCondition(true);  // Module-specific warning
indicators.update();                   // Call in loop()
```

> **Note:** When using `PicoServer`, indicator LEDs are managed automatically. Access via `server.indicators()` for setting error/warning conditions.

---

## Pico Server

### PicoServer Class

Common server controller boilerplate for all ScaleFX Pico server firmware. Encapsulates the USB serial initialization, device naming, indicator LEDs, core protocol handling, command routing, and connection timeout management that was previously duplicated across every controller.

**What PicoServer handles:**
- USB serial initialization (115200 baud)
- Unique device name from Pico board ID (e.g. "GunFX-A1B2")
- Indicator LEDs on GP13/GP14 (connection + error status)
- CoreCommandServer with board info and INIT/SHUTDOWN/REBOOT/BOOTSEL callbacks
- CommandRouter with automatic handler priority (core first, then module)
- Connection timeout / watchdog detection (15s)
- Common loop tasks (router process, activity forwarding, free RAM, indicators)

**Basic usage (minimal controller):**
```cpp
#include <pico_server.h>

PicoServer server;

void setup() {
    server.begin("MyController", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]()     { resetHardware(); });
    server.onShutdown([]() { safeHardware();  });
    
    // Core-only controller (no module commands)
    server.addModuleHandler(nullptr);
}

void loop() {
    server.loop();       // protocol, timeout, indicators
    updateHardware();    // module-specific work
    delay(1);
}
```

**With a module handler:**
```cpp
#include <pico_server.h>
#include <serial.h>

PicoServer server;
GunFxServer gunfxServer;

void setup() {
    server.begin("GunFX", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]()     { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });
    
    // Configure module handler
    gunfxServer.begin(&Serial, server.deviceName());
    gunfxServer.onTriggerOn([](uint16_t rpm) -> uint8_t { ... });
    // ... register more callbacks ...
    
    // Append module data to STATUS response
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        // Write module-specific status bytes
        return writeModuleStatus(buf, maxLen);
    });
    
    // Finalize router (core + module handlers)
    server.addModuleHandler(&gunfxServer);
}

void loop() {
    server.loop();          // protocol, timeout, indicators
    updateServos();         // module-specific updates
    delay(1);
}
```

**Setting error/warning conditions:**
```cpp
// In loop(), before server.loop() or after:
server.indicators().setErrorCondition(hasError);
server.indicators().setWarningCondition(hasWarning);
// PicoServer calls indicators.update() automatically in server.loop()
```

**API:**

| Method | Description |
|--------|-------------|
| `begin(prefix, version, buildNumber)` | Initialize serial, device name, indicators, core |
| `onInit(cb)` | Register controller-specific init callback |
| `onShutdown(cb)` | Register controller-specific shutdown callback |
| `addModuleHandler(handler)` | Add module handler and finalize router |
| `loop()` | Process protocol, timeout, indicators |
| `core()` | Access CoreCommandServer (for onStatusData, etc.) |
| `indicators()` | Access IndicatorLedManager (for error/warning) |
| `router()` | Access CommandRouter (advanced use) |
| `deviceName()` | Get generated device name (e.g. "GunFX-A1B2") |

---

## Dependencies

- Arduino framework (earlephilhower/arduino-pico)
- Servo library (for srv_control)
- Wire library (for I2C — provided by Arduino)
- ADC (for battery_monitor — provided by Arduino)
- Raspberry Pi Pico (RP2040)
- C++17 or later
