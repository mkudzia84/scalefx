# sfx_peripherals — Hardware Peripheral Drivers

Reusable hardware drivers for ScaleFX controllers. Covers GPIO abstraction, LED control with pluggable backends, servo output, PWM input, power monitoring, and indicator LEDs.

**Platforms:** RP2040, RP2350, ESP32-S3

## Module Map

```
sfx_peripherals/
├── gpio/                        GPIO abstraction layer
│   ├── gpio_expander.h          Concept definition + common types + SFINAE traits
│   ├── native_gpio.h            MCU GPIO wrapper (singleton, HW PWM via analogWrite)
│   ├── aw9523b.h/.cpp           AW9523B I2C expander (HW LED PWM, 256-step)
│   └── pcal6416a.h/.cpp         PCAL6416A I2C expander (GPIO only, port-level bulk I/O)
│
├── led/                         LED control & animation
│   ├── led_control.h/.ipp/.cpp  LedControlT<TGpio> single-channel controller
│   ├── led_manager.h/.ipp       LedManager<N,TGpio> multi-channel manager
│   ├── bam_led_drv.h            ExpanderBamT<T> software BAM engine + GPIO provider
│   ├── led_events.h             Animation events (On, Off, Flash, Fade, Beacon)
│   ├── led_event_seq.h/.cpp     Looping event sequence player
│   ├── server/led_server.h/.cpp LedProtocolServer (13 LED serial commands)
│   └── client/led_client.h/.cpp LedProtocolClient (command builders + response parsing)
│
├── indicators/
│   └── indicator_leds.h         Connection/error LED state machine (used by SfxServer)
│
├── servo/                       Servo output with motion profiling
├── pwm/                         RC PWM input with averaging and callbacks
├── power/                       INA226 power monitor, I2CDevice base, battery monitor
└── library.json
```

---

## GPIO Provider Architecture

All LED and GPIO-consuming templates are parameterized on a **GPIO provider** — a duck-typed C++ concept rather than a virtual base class. This gives zero-overhead abstraction at LED tick rates.

### The Concept (`gpio/gpio_expander.h`)

A type `T` satisfies the GPIO provider concept if it provides:

```cpp
bool isAvailable() const;                               // Device online?
bool setPinDirection(uint8_t pin, bool isInput);         // Configure pin I/O
bool setLedMode(uint8_t pin, bool ledMode);              // Enable HW LED mode (no-op if N/A)
bool setLedBrightness(uint8_t pin, uint8_t brightness);  // 0-255 PWM duty
bool writePin(uint8_t pin, bool high);                   // Digital write
static constexpr bool HAS_HW_PWM;                        // true if real PWM, false for BAM
```

Optional port-level bulk I/O (required by `ExpanderBamT` but not by `LedControlT`):

```cpp
bool    setPortDirection(uint8_t port, uint8_t mask);
uint8_t getPortDirection(uint8_t port);
bool    writePort(uint8_t port, uint8_t value);
uint8_t readPort(uint8_t port);
```

SFINAE helpers for compile-time checks:

| Trait | Tests |
|-------|-------|
| `expander_has_hw_pwm_v<T>` | `T::HAS_HW_PWM == true` |
| `expander_has_led_methods_v<T>` | `T::setLedMode()` and `T::setLedBrightness()` exist |

### Implementations

| Provider | Header | HAS_HW_PWM | PWM Method | Notes |
|----------|--------|------------|------------|-------|
| `NativeGpio` | `gpio/native_gpio.h` | `true` | `analogWrite` / LEDC | Singleton via `instance()`. No port-level I/O. |
| `AW9523B` | `gpio/aw9523b.h` | `true` | 256-step constant-current | I2C 0x58–0x5B. Per-pin GPIO/LED mode. Extends `I2CDevice`. |
| `PCAL6416A` | `gpio/pcal6416a.h` | `false` | N/A (use BAM) | I2C 0x20–0x27. Port-level bulk I/O. Extends `I2CDevice`. |
| `ExpanderBamT<T>` | `led/bam_led_drv.h` | `false` | Software BAM (5-bit, 32 levels) | Wraps a port-level expander. IS a GPIO provider itself. |

### Template Consumers

All template on the GPIO provider directly — no intermediate driver layer:

| Consumer | Template Parameter | Purpose |
|----------|-------------------|---------|
| `LedControlT<TGpio>` | GPIO provider | Single LED channel (on/off, brightness, events) |
| `LedManager<N, TGpio>` | Channel count + GPIO provider | Multi-channel manager with sequences |
| `ExpanderBamT<TExpander>` | Port-level expander | BAM engine that itself becomes a GPIO provider |

---

## LED Control Stack

Two-layer architecture — LED controllers bind directly to GPIO providers:

```
┌─────────────────────────────────────────────────────────────────┐
│  Protocol Layer (optional)                                       │
│  LedProtocolServer ──→ ILedManager ──→ LedManager<N,TGpio>      │
│  LedProtocolClient                                               │
└─────────────────────────────────────────────────────────────────┘
         │
┌─────────────────────────────────────────────────────────────────┐
│  Animation Layer                                                 │
│  LedEventSeq ──→ ILedOutput ──→ LedControlT<TGpio>              │
│  Events: LedOn, LedOff, LedFlashing, LedFadeIn/Out, LedBeacon   │
└─────────────────────────────────────────────────────────────────┘
         │
┌─────────────────────────────────────────────────────────────────┐
│  Control Layer                                                   │
│  LedControlT<TGpio>                                              │
│    stores: {TGpio* _gpio, uint8_t _pin}                          │
│    calls:  _gpio->writePin(), _gpio->setLedBrightness()          │
└─────────────────────────────────────────────────────────────────┘
         │
┌─────────────────────────────────────────────────────────────────┐
│  GPIO Provider Layer                                             │
│  NativeGpio          → analogWrite / digitalWrite                │
│  AW9523B             → I2C HW LED registers                      │
│  ExpanderBamT<T>     → software BAM → T::writePort()             │
│    └── PCAL6416A     → I2C port write                            │
└─────────────────────────────────────────────────────────────────┘
```

### Virtual Interfaces

Two interfaces decouple the template world from non-template consumers:

| Interface | Defined in | Purpose | Implementors |
|-----------|-----------|---------|-------------|
| `ILedOutput` | `led_control.h` | Single LED output (brightness, on, off) | `LedControlT<TGpio>` |
| `ILedManager` | `led_manager.h` | Multi-channel manager (set, toggle, seq, enable) | `LedManager<N, TGpio>` |

`LedEventSeq` and `LedProtocolServer` use these interfaces, so they work with any GPIO provider without being templatized themselves.

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

### I2C Expander with HW PWM (AW9523B)

```cpp
#include <led/led_control.h>
#include <gpio/aw9523b.h>

AW9523B expander;
expander.begin(Wire, 0x58);

LedControlT<AW9523B> led;
led.begin(&expander, 0, false, true);        // P0_0, active-high, PWM
led.setBrightness(75);                       // 256-step HW PWM
```

### Software BAM on GPIO-Only Expander (PCAL6416A)

```cpp
#include <led/led_manager.h>
#include <led/bam_led_drv.h>

PCAL6416A expander;
expander.begin(Wire, 0x20);

ExpanderBamT<PCAL6416A> bamEngine;
bamEngine.begin(&expander, 0);               // BAM on port 0, pins 0-5

LedManager<6, ExpanderBamT<PCAL6416A>> ledManager;
const uint8_t pins[6] = {0,1,2,3,4,5};
ledManager.begin(&bamEngine, pins);
ledManager.ledSet(3, 50);                    // channel 3 → 50%

// In loop() — MUST call frequently (~3 kHz ideal):
bamEngine.update();
ledManager.update();
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

MCU GPIO wrapper. Singleton via `NativeGpio::instance()`. Uses `analogWrite()` for PWM (Arduino-Pico LEDC on ESP32-S3). Every MCU pin is available; `isAvailable()` always returns `true`.

Does **not** provide port-level bulk I/O — `ExpanderBamT<NativeGpio>` won't compile (intentionally: native GPIO has real PWM and doesn't need software BAM).

### `AW9523B` (gpio/aw9523b.h)

Awinic AW9523B — 16-pin I2C GPIO expander with per-pin LED constant-current drivers. Each pin independently operates in GPIO mode or LED mode (256-step HW PWM at ~430 Hz). Extends `I2CDevice`. I2C address range: 0x58–0x5B.

### `PCAL6416A` (gpio/pcal6416a.h)

NXP PCAL6416AHF — 16-pin I2C GPIO expander. GPIO only (no HW PWM). Provides port-level bulk I/O with configurable drive strength, pull resistors, and interrupt masking. Extends `I2CDevice`. I2C address range: 0x20–0x27.

### `ExpanderBamT<T>` (led/bam_led_drv.h)

Software Binary Angle Modulation for GPIO-only I2C expanders. Wraps one port (8 pins) of a port-level expander. Accepts 0-255 PWM values mapped to 32 levels via a gamma-2.2 perceptual LUT (`BamLut::pwmToBam()`). Produces ~100 Hz flicker-free output with one I2C port-write per BAM tick.

**Dual role:** BAM engine (call `update()` in loop) AND GPIO provider (pass to `LedControlT` / `LedManager`).

### `LedControlT<TGpio>` (led/led_control.h)

Single LED channel. Stores `{TGpio* _gpio, uint8_t _pin}` and calls the provider directly. Supports on/off, PWM brightness (0-100%), master brightness scaling, active-high/low polarity, and event-based animations via `ILedOutput`.

Default alias: `using LedControl = LedControlT<NativeGpio>`

### `LedManager<N, TGpio>` (led/led_manager.h)

Multi-channel manager. Holds `N` `LedControlT<TGpio>` instances, `N` `LedEventSeq` sequences, per-channel enable/disable, and master brightness. 1-based channel numbering (0 = all channels). Implements `ILedManager` for protocol server binding.

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
