# Components Library

Reusable hardware component drivers for ScaleFX controllers. This library provides generic, controller-agnostic building blocks for audio, storage, I2C devices, LEDs, PWM inputs, servos, and the binary serial protocol.

**Supported platforms:** RP2040, RP2350, ESP32-S3

## Architecture

```
sfx_common/
├── platform/
│   └── sfx_platform.h         Cross-platform abstraction (mutexes, delays, GPIO)
├── audio/
│   ├── audio_config.h          Audio system constants (sample rate, channels, I2S pins)
│   ├── audio_log.h             Audio-specific logging macros (via DiagLog)
│   ├── audio_ring_buffer.h     Lock-free SPSC ring buffer (DMA-friendly on ESP32)
│   ├── audio_codec.h           Abstract codec base class
│   ├── audio_mixer.h/.cpp      8-channel WAV mixer with I2S output
│   ├── simple_i2s_codec.h/.cpp Generic I2S codec (no I2C control)
│   ├── tas5825_codec.h/.cpp    TI TAS5825M digital amp driver (I2C)
│   └── mock_i2s_sink.h/.cpp    Mock I2S for testing (statistics capture)
├── led/
│   ├── led_control.h/.cpp      GPIO LED on/off/PWM brightness
│   ├── led_events.h            Event-based LED animations (ILedEvent)
│   └── led_event_seq.h/.cpp    Sequenced LED event playback
├── power/
│   ├── battery_monitor.h/.cpp  ADC battery voltage with cell detection
│   ├── i2c_device.h/.cpp       Base class for all I2C peripherals
│   └── ina226.h/.cpp           TI INA226 power monitor driver
├── pwm/
│   └── pwm_control.h/.cpp      RC PWM input with averaging/hysteresis
├── serial/
│   ├── serial.h                Umbrella include
│   ├── core/                   CoreProtocol, COBS, CRC-8, CommandRouter, SFX_* macros
│   │   ├── core.h/.cpp         Protocol constants, encode/decode, handler interfaces
│   │   ├── bus_server.h/.cpp   BusServer + CoreCommandServer base classes
│   │   ├── stream.h/.cpp       StreamWriter (chunked CRC-16 streaming)
│   │   └── diag_log.h/.cpp     DiagLog singleton (ring buffer → COBS packets)
│   ├── client/                 Client-side transport (HubFX only)
│   │   ├── bus.h/.cpp          SerialBus (COBS over USB CDC)
│   │   ├── bus_client.h/.cpp   BusClient base class
│   │   ├── usb_host.h/.cpp     PIO-USB host driver
│   │   └── result_queue.h      Tag-correlated command/response matching
│   ├── gunfx/gunfx.h           GunFxServer, GunFxClient, GunFxPacket, GunFxError
│   ├── lightfx/lightfx.h       LightFxServer, LightFxClient, LightFxPacket, LightFxError
│   └── gearcontrol/gearcontrol.h GearControlServer, GearControlClient, GearControlPacket
├── server/
│   └── sfx_server.h/.cpp       Common server controller boilerplate
├── servo/
│   └── srv_control.h/.cpp      Servo motion profiling with jerk effects
└── storage/
    ├── storage_types.h          Shared enums (StorageType, StorageError)
    ├── sd_card.h/.cpp           SdFat SD card singleton (SPI)
    └── flash.h/.cpp             LittleFS onboard flash singleton
```

## Component Reuse Guidelines

> **Rule:** When implementing controller firmware, always check the components library first. If a generic hardware abstraction exists here, use it instead of writing controller-specific code.

> **Rule:** When adding support for a new hardware peripheral (sensor, actuator, display, etc.), create the driver here as a reusable component — not embedded in controller code. Controller firmware should only contain controller-specific logic and protocol handling.

---

## Platform Abstraction

### sfx_platform.h

Cross-platform abstraction layer for all shared library code. Provides unified macros, types, and inline functions that compile to the native SDK calls on each platform.

**Location:** `platform/sfx_platform.h`

**Supported platforms:**

| Macro | Platform | Chips |
|-------|----------|-------|
| `SFX_PLATFORM_PICO` | Arduino-Pico (Earle Philhower) | RP2040, RP2350 |
| `SFX_PLATFORM_ESP32` | ESP-IDF Arduino | ESP32-S3 |

**Abstractions provided:**

| Category | API | Pico Native | ESP32 Native |
|----------|-----|-------------|---------------|
| Timing | `SFX_DELAY_MS(ms)` | `busy_wait_ms(ms)` | `vTaskDelay(pdMS_TO_TICKS(ms))` |
| Timing | `SFX_DELAY_US(us)` | `busy_wait_us_32(us)` | `esp_rom_delay_us(us)` |
| System | `SFX_FREE_HEAP()` | `rp2040.getFreeHeap()` | `esp_get_free_heap_size()` |
| System | `SFX_REBOOT()` | `rp2040.reboot()` | `esp_restart()` |
| System | `SFX_CPU_MHZ()` | `F_CPU / 1000000` | `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` |
| System | `SFX_PLATFORM_NAME` | `"RP2040"` or `"RP2350"` | `"ESP32-S3"` or `"ESP32"` |
| System | `sfxGetBoardId(buf, len)` | Flash unique ID | MAC address |
| System | `sfxRebootToBootloader()` | BOOTSEL mode | `esp_restart()` |
| Mutex | `SfxMutex` | `mutex_t` | FreeRTOS `SemaphoreHandle_t` |
| Mutex | `sfxMutexInit(mtx)` | `mutex_init(&mtx)` | `xSemaphoreCreateMutex()` |
| Mutex | `sfxMutexLock(mtx)` | `mutex_enter_blocking(&mtx)` | `xSemaphoreTake(handle, MAX)` |
| Mutex | `sfxMutexTryLock(mtx)` | `mutex_try_enter(&mtx, nullptr)` | `xSemaphoreTake(handle, 0)` |
| Mutex | `sfxMutexUnlock(mtx)` | `mutex_exit(&mtx)` | `xSemaphoreGive(handle)` |
| GPIO | `SFX_VBUS_PIN` | GPIO 24 | 0xFF (N/A) |
| Servo | `#include <Servo.h>` | PIO-based | LEDC-based (`ESP32Servo.h`) |
| Interrupt | `SFX_ATTACH_INTERRUPT_PARAM(...)` | `attachInterruptParam()` | `attachInterruptArg()` |
| I2S | `SFX_I2S_PICO` / `SFX_I2S_ESP32` | PIO I2S | Hardware I2S peripheral |
| Memory | `SFX_DMA_BUFFER` | (no-op) | `DMA_ATTR DRAM_ATTR` |
| Memory | `SFX_IRAM_FUNC` | (no-op) | `IRAM_ATTR` |
| Dual-core | `SFX_DUAL_CORE_PICO` | `setup1()`/`loop1()` | — |
| Dual-core | `SFX_DUAL_CORE_FREERTOS` | — | `xTaskCreatePinnedToCore()` |

**Usage in shared library code:**
```cpp
#include "platform/sfx_platform.h"

SfxMutex _mutex;
sfxMutexInit(_mutex);
sfxMutexLock(_mutex);
// ... critical section ...
sfxMutexUnlock(_mutex);

SFX_DELAY_MS(10);
uint32_t heap = SFX_FREE_HEAP();
```

> **Rule:** Shared library code (`controllers/lib/sfx_common/`) MUST use `sfx_platform.h` abstractions instead of raw Pico SDK or ESP-IDF calls. Controller firmware (single-platform) may use platform-native APIs directly.

> **Note:** `millis()` is safe on all platforms and not abstracted — use it freely.

**Components using sfx_platform.h:**
- `serial/core/diag_log.h/.cpp` — SfxMutex for ring buffer thread safety
- `serial/client/result_queue.cpp` — SFX_DELAY_MS in tag wait loop
- `audio/audio_mixer.cpp` — SfxMutex for command queue, SFX_DELAY_MS in I2S init
- `audio/audio_ring_buffer.h` — SFX_DMA_BUFFER for ESP32 internal SRAM placement
- `server/sfx_server.cpp` — SFX_FREE_HEAP, SFX_CPU_MHZ, sfxGetBoardId, sfxRebootToBootloader

---

## Audio

### Audio Buffer Architecture

The audio system uses a 5-layer buffer pipeline designed for glitch-free playback on dual-core systems. Buffer sizes are platform-conditional to leverage ESP32-S3's larger RAM.

```
SD Card → [SD Read Buffer] → WAV Decode → [Per-Channel Float Buffers]
    → Float Mix → [SPSC Ring Buffer] → [I2S DMA / Batch Buffer] → Speakers
```

**Buffer sizes (platform-conditional):**

| Buffer | Pico (RP2040/RP2350) | ESP32-S3 | Location |
|--------|---------------------|----------|----------|
| Ring buffer | 16K frames (64 KB) | 32K frames (128 KB) | `audio_ring_buffer.h` |
| Per-channel float | 1,024 frames (8 KB/ch) | 2,048 frames (16 KB/ch) | `audio_mixer.h` |
| SD read buffer | 4 KB | 16 KB | `audio_mixer.h` |
| Mix buffer | 512 samples | 1,024 samples | `audio_config.h` |
| Stream buffer | 2 KB | 8 KB | `audio_config.h` |
| ESP32 I2S batch | — | 1,024 int16 (2 KB) | `audio_mixer.cpp` |
| ESP32 I2S DMA | — | 8×512 frames (16 KB) | `audio_mixer.cpp` |

**Core allocation:**
- **Core 0 (Producer):** SD read → WAV decode → float mix → ring buffer write
- **Core 1 (Consumer):** Ring buffer read → int16 conversion → I2S DMA output

**Feature guard:** All audio code is compiled only when `-DSFX_HAS_AUDIO=1` is defined (HubFX only). Other controllers compile the components library without any audio overhead.

See `audio_config.h` for configurable constants (sample rate, channel count, I2S pins).

See `audio_mixer.h` for the full `AudioMixer` singleton API.

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

### SfxServer::IndicatorLedManager (Nested Class)

Manages the two standardized indicator LEDs (GP13=connection, GP14=error) present on all ScaleFX Pico server boards. Encapsulates the blink patterns and state transitions for connection status and error reporting. Defined as a nested class inside `SfxServer` since it is only used there.

**States:**

| LED | Pin | Waiting for INIT | Connected | Connection Lost |
|-----|-----|-----------------|-----------|----------------|
| Connection | GP13 | Blink 500ms | Solid ON | OFF |
| Error | GP14 | OFF | OFF | Blink 200ms (if error) |

**Basic usage:**
```cpp
#include <server/sfx_server.h>

// Access via SfxServer instance:
server.indicators().setConnected(true);         // INIT received
server.indicators().setErrorCondition(true);    // Module-specific error
server.indicators().setWarningCondition(true);  // Module-specific warning
// update() is called automatically by server.loop()
```

> **Note:** Indicator LEDs are managed automatically by `SfxServer`. Access via `server.indicators()` for setting error/warning conditions.

---

## SfxServer

### SfxServer Class

Common server controller boilerplate for all ScaleFX server firmware. Encapsulates the USB serial initialization, device naming, indicator LEDs, core protocol handling, command routing, and connection timeout management that was previously duplicated across every controller.

Cross-platform: supports RP2040, RP2350, and ESP32-S3 via `sfx_platform.h`.

**What SfxServer handles:**
- USB serial initialization (1Mbps baud)
- Unique device name from board ID (e.g. "GunFX-A1B2")
- Indicator LEDs on GP13/GP14 (connection + error status) via nested `IndicatorLedManager`
- CoreCommandServer with board info and INIT/SHUTDOWN/REBOOT/BOOTSEL callbacks
- CommandRouter with automatic handler priority (core first, then module)
- Connection timeout / watchdog detection (15s)
- Common loop tasks (router process, activity forwarding, free RAM, indicators)

**Basic usage (minimal controller):**
```cpp
#include <server/sfx_server.h>

SfxServer server;

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
    SFX_DELAY_MS(1);
}
```

**With a module handler:**
```cpp
#include <server/sfx_server.h>
#include <serial.h>

SfxServer server;
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
    SFX_DELAY_MS(1);
}
```

**Setting error/warning conditions:**
```cpp
// In loop(), before server.loop() or after:
server.indicators().setErrorCondition(hasError);
server.indicators().setWarningCondition(hasWarning);
// SfxServer calls indicators.update() automatically in server.loop()
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

- Arduino framework (earlephilhower/arduino-pico for RP2040/RP2350, ESP-IDF Arduino for ESP32-S3)
- Servo library (for srv_control)
- Wire library (for I2C — provided by Arduino)
- ADC (for battery_monitor — provided by Arduino)
- C++17 or later
