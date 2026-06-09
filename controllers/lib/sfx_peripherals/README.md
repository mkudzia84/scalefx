# sfx_peripherals — Hardware Peripheral Drivers

Reusable hardware drivers for ScaleFX controllers. Covers the PwmOutput
abstraction (one concept, two backends), LED control + multi-channel
manager templated on it, servo output, PWM input, power monitoring, and
indicator LEDs.

**Platforms:** RP2040, RP2350, ESP32-S3

## Module Map

```
sfx_peripherals/
├── gpio/                        Native MCU GPIO
│   └── native_gpio.h            MCU pin wrapper (digital + PWM — Pico: analogWrite / ESP32: LEDC-native)
│
├── pwm/                         PWM-output abstraction + chip drivers
│   ├── pwm_output.h             PwmOutput concept (single duck-typed surface)
│   ├── pca9685.h/.cpp           NXP 16-channel 12-bit I²C PWM driver
│   ├── pwm_control.h/.cpp       RC PWM input capture (averaging + callbacks)
│
├── led/                         LED control & animation
│   ├── led_control.h/.ipp/.cpp  LedControlT<TPwm> single-channel controller
│   ├── led_manager.h/.ipp       LedManager<N,TPwm> multi-channel manager
│   ├── led_events.h             Animation events (On, Off, Flash, Fade, Beacon)
│   ├── led_event_seq.h/.cpp     Looping event sequence player
│   ├── server/led_server.h/.cpp LedProtocolServer (13 LED serial commands)
│   └── client/led_client.h/.cpp LedProtocolClient (command builders + response parsing)
│
├── indicators/
│   └── indicator_leds.h         IndicatorServicePolicy — connection/error LED state machine (auto-prepended by BoardServer/BoardOf)
│
├── servo/                       Servo output with motion profiling
├── motor/                       DC motor topologies + stall detection
├── power/                       INA226 + I2CDevice base + battery monitor + Sensor concept
├── collections/                 Multi-channel facades (servo / PWM / LED) for expander boards
└── library.json
```

---

## PwmOutput Architecture

The LED runtime and any other pin-level consumer templates on a single
**PwmOutput** concept — duck-typed, no virtual dispatch, zero overhead
at LED tick rates.

### The Concept (`pwm/pwm_output.h`)

A type `T` satisfies `PwmOutput` if it provides:

```cpp
bool isAvailable() const;                                // probe
bool setPinDirection(uint8_t pin, bool isInput);          // mark as output
bool writePin(uint8_t pin, bool high);                    // digital write
bool setLedBrightness(uint8_t pin, uint8_t brightness);   // 0-255 PWM duty
```

The "Led" prefix is historical — `setLedBrightness` is just "write
8-bit PWM duty"; the same call drives LEDs, servos (at 50 Hz), motors,
or any generic PWM signal. Per-backend `setFrequency()` selects what
the duty value physically means.

### Implementations

| Provider     | Header                | HAS_HW_PWM | Notes                                                        |
|--------------|-----------------------|------------|--------------------------------------------------------------|
| `NativeGpio` | `gpio/native_gpio.h`  | `true`     | MCU pin — Pico: `analogWrite`; ESP32: LEDC-native. Singleton `instance()`. Also exposes `readPin()` for input. |
| `PCA9685`    | `pwm/pca9685.h`       | `true`     | NXP 16-channel 12-bit I²C PWM, 24-1526 Hz, push-pull / open-drain. Extends `I2CDevice`. |

Both backends additionally expose `static constexpr uint8_t NUM_PINS`
as a caller-side hint; it's not part of the contract.

### Template Consumers

All template on the PwmOutput backend directly — no intermediate driver layer:

| Consumer            | Template Parameter            | Purpose |
|---------------------|-------------------------------|---------|
| `LedControlT<TPwm>` | PwmOutput backend             | Single LED channel (on/off, brightness, events) |
| `LedManager<N,TPwm>`| Channel count + PwmOutput     | Multi-channel manager with event-sequence runtime |

---

## LED Control Stack

Two-layer architecture — LED controllers bind directly to GPIO providers:

```
┌─────────────────────────────────────────────────────────────────┐
│  Protocol Layer (optional)                                       │
│  LedProtocolServer ──→ ILedManager ──→ LedManager<N,TPwm>       │
│  LedProtocolClient                                               │
└─────────────────────────────────────────────────────────────────┘
         │
┌─────────────────────────────────────────────────────────────────┐
│  Animation Layer                                                 │
│  LedEventSeq ──→ ILedOutput ──→ LedControlT<TPwm>               │
│  Events: LedOn, LedOff, LedFlashing, LedFadeIn/Out, LedBeacon   │
└─────────────────────────────────────────────────────────────────┘
         │
┌─────────────────────────────────────────────────────────────────┐
│  Control Layer                                                   │
│  LedControlT<TPwm>                                               │
│    stores: {TPwm* _gpio, uint8_t _pin}                           │
│    calls:  _gpio->writePin(), _gpio->setLedBrightness()          │
└─────────────────────────────────────────────────────────────────┘
         │
┌─────────────────────────────────────────────────────────────────┐
│  PwmOutput Backend Layer                                         │
│  NativeGpio    → MCU pins (Pico: analogWrite / ESP32: LEDC)      │
│  PCA9685       → I²C burst write (16-ch 12-bit PWM)              │
└─────────────────────────────────────────────────────────────────┘
```

### Virtual Interfaces

Two interfaces decouple the template world from non-template consumers:

| Interface | Defined in | Purpose | Implementors |
|-----------|-----------|---------|-------------|
| `ILedOutput` | `led_control.h` | Single LED output (brightness, on, off) | `LedControlT<TPwm>` |
| `ILedManager` | `led_manager.h` | Multi-channel manager (set, toggle, seq, enable) | `LedManager<N, TPwm>` |

`LedEventSeq` and `LedProtocolServer` use these interfaces, so they work with any PwmOutput backend without being templatized themselves.

---

## Usage Examples

### Native GPIO (LightFX Pico)

```cpp
#include <led/led_control.h>
#include <led/led_manager.h>

// LedControl = LedControlT<NativeGpio> (default alias)
LedManager<8> ledManager;                    // 8 native GPIO channels
const uint8_t pins[8] = {0,1,2,3,4,5,6,7};
ledManager.begin(pins);                      // uses NativeGpio::instance()

ledManager.ledSet(1, 80);                    // channel 1 → 80% brightness
ledManager.seqStart(0);                      // start all sequences
```

### PCA9685 (HubFX ESP32-S3)

```cpp
#include <led/led_manager.h>
#include <pwm/pca9685.h>

PCA9685 pwm;
pwm.begin(Wire, PCA9685Address::HUBFX_ADDR);   // 0x70, default 1526 Hz

LedManager<8, PCA9685> ledManager;
const uint8_t pins[8] = {0,1,2,3,4,5,6,7};
ledManager.begin(&pwm, pins);
ledManager.ledSet(3, 50);                       // channel 3 → 50%
ledManager.update();                            // call in loop()
```

### PCA9685 driving servos (any board)

```cpp
PCA9685 pwm;
pwm.begin(Wire, 0x40, 50);                     // 50 Hz for hobby servos

// 1 ms pulse  = 205/4096 of 20 ms period  → setChannel(ch, 205)
// 2 ms pulse  = 410/4096 of 20 ms period  → setChannel(ch, 410)
pwm.setChannel(0, 307);                         // ≈ centre (1.5 ms pulse)
```

### Animation Sequences

```cpp
#include <led/led_events.h>
#include <led/led_event_seq.h>

LedEventSeq seq;
seq.add(new LedOn(500));                     // On for 500ms
seq.add(new LedOff(500));                    // Off for 500ms
seq.add(new LedFadeIn(1000));                // Fade in over 1s
seq.add(new LedFadeOut(1000));               // Fade out over 1s

LedControl led;
led.begin(13, false, true);
seq.setOutput(&led);                         // Bind to ILedOutput
seq.start();

// In loop():
seq.update();
```

---

## Component Reference

### `NativeGpio` (gpio/native_gpio.h)

MCU GPIO wrapper. Singleton via `NativeGpio::instance()`. PWM is platform-native — `analogWrite()` (Arduino-Pico hardware PWM) on RP2040/RP2350, native LEDC (`driver/ledc`) on ESP32-S3. Every MCU pin is available; `isAvailable()` always returns `true`. Exposes `readPin()` for input — not part of `PwmOutput` but available on this backend.

### `PCA9685` (pwm/pca9685.h)

NXP PCA9685 — 16-channel 12-bit I²C PWM driver. Programmable frequency 24-1526 Hz (one frequency per chip; affects all 16 channels). Push-pull or open-drain output. Used on the HubFX 8-channel rev at I²C address `0x70` driving N-MOSFET gates for LED rails. Drop-in for servo / motor / generic-PWM duty with appropriate `setFrequency()`. Extends `I2CDevice`.

### `LedControlT<TPwm>` (led/led_control.h)

Single LED channel. Stores `{TPwm* _gpio, uint8_t _pin}` and calls the backend directly. Supports on/off, PWM brightness (0-100%), master brightness scaling, active-high/low polarity, and event-based animations via `ILedOutput`.

Default alias: `using LedControl = LedControlT<NativeGpio>`

### `LedManager<N, TPwm>` (led/led_manager.h)

Multi-channel manager. Holds `N` `LedControlT<TPwm>` instances, `N` `LedEventSeq` sequences, per-channel enable/disable, and master brightness. 1-based channel numbering (0 = all channels). Implements `ILedManager` for protocol server binding.

Default: `LedManager<N>` = `LedManager<N, NativeGpio>`

### `LedEventSeq` (led/led_event_seq.h)

Looping sequence of `ILedEvent` animations. Up to 24 events per sequence. Drives output through `ILedOutput*` — works with any `LedControlT` variant.

### `ILedEvent` types (led/led_events.h)

| Event | Parameters | Behavior |
|-------|-----------|----------|
| `LedOn` | duration_ms | Constant on |
| `LedOff` | duration_ms | Constant off |
| `LedFlashing` | interval_ms, duration_ms | Square wave on/off |
| `LedFadeIn` | duration_ms | Linear ramp 0→100% |
| `LedFadeOut` | duration_ms | Linear ramp 100→0% |
| `LedFading` | period_ms, duration_ms | Sinusoidal breathing |
| `LedBeacon` | flash_ms, pause_ms, duration_ms | Brief flash, long pause |

### `LedProtocolServer` (led/server/led_server.h)

Handles 13 LightFX LED packet types (0x40-0x4C). Takes `ILedManager*` — works with any `LedManager` instantiation. Can be used standalone or as a base for `LightFxServer`.

### `LedProtocolClient` (led/client/led_client.h)

Client-side command builders and response parsers for all 13 LED commands. Extends `BusClient`. Base for `LightFxClient`.
