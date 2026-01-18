# Logging System Documentation

## Overview

The ScaleFX Hub system uses a unified, component-based logging system for consistent output across all modules. This document explains the logging infrastructure, best practices, and how to use it.

## Logging Levels

The logging system provides four main levels:

### 1. **ERROR (stderr)**
Used for critical failures that prevent system operation or component functionality.

```c
LOG_ERROR(LOG_GUN, "Failed to allocate memory for gun FX");
```

**Output:**
```
[GUN]    Error: Failed to allocate memory for gun FX
```

**Use Case:** Component initialization failures, memory allocation errors, invalid configuration.

### 2. **WARN (stderr)**
Used for non-critical issues that degraded functionality but allow system to continue.

```c
LOG_WARN(LOG_GUN, "Failed to create nozzle flash LED");
```

**Output:**
```
[GUN]    Warning: Failed to create nozzle flash LED
```

**Use Case:** Missing optional components, partial failures, configuration warnings.

### 3. **INFO (stdout)**
Used for informational messages about normal system operation and state changes.

```c
LOG_INFO(LOG_GUN, "Nozzle flash LED initialized on GPIO %d", pin);
```

**Output:**
```
[GUN]    Nozzle flash LED initialized on GPIO 23
```

**Use Case:** Component initialization, state transitions, operational milestones.

### 4. **DEBUG (stdout, compile-time optional)**
Used for detailed debugging information, only compiled when DEBUG is defined.

```c
LOG_DEBUG(LOG_GUN, "Processing trigger PWM: %d µs", pwm_value);
```

**Output:** (only with `-DDEBUG` flag)
```
[GUN]    [DEBUG] Processing trigger PWM: 1650 µs
```

**Use Case:** Detailed diagnostic information during development.

## Component Prefixes

Each component has a standard prefix for easy log filtering:

| Component | Prefix | File | Notes |
|-----------|--------|------|-------|
| Main System | `LOG_SFXHUB` | main.c | System-level events |
| Configuration | `LOG_CONFIG` | config_loader.c | Configuration loading |
| Engine FX | `LOG_ENGINE` | engine_fx.c | Engine sound effects |
| Gun FX | `LOG_GUN` | gun_fx.c | Gun effects controller |
| Servo Control | `LOG_SERVO` | servo.c | Servo motor control |
| Audio System | `LOG_AUDIO` | audio_player.c | Audio mixing/playback |
| Smoke System | `LOG_SMOKE` | smoke_generator.c | Smoke generator control |
| GPIO | `LOG_GPIO` | gpio.c | GPIO abstraction layer |

## Specialized Logging Macros

### State Messages
```c
LOG_STATE(LOG_GUN, "IDLE", "FIRING");
// Output: [GUN]    State: IDLE → FIRING
```

### Initialization
```c
LOG_INIT(LOG_SERVO, "Servo with speed limit 500 µs/s");
// Output: [SERVO]  Initialized: Servo with speed limit 500 µs/s
```

### Shutdown
```c
LOG_SHUTDOWN(LOG_GUN, "Gun FX system");
// Output: [GUN]    Shutdown: Gun FX system
```

### Status Lines (No Tag)
```c
LOG_STATUS("[GUN STATUS @ %.1fs] Firing: %s | Rate: %d | RPM: %d", 
           elapsed, firing_str, rate, rpm);
// Output: [GUN STATUS @ 10.2s] Firing: YES | Rate: 2 | RPM: 550
```

## Usage Examples

### In Gun FX Processing Thread

```c
// Thread started
LOG_INFO(LOG_GUN, "Processing thread started");

// State change
if (trigger_active && !was_firing) {
    LOG_STATE(LOG_GUN, "IDLE", "FIRING");
    LOG_INFO(LOG_GUN, "Firing started (%d RPM)", rpm);
}

// Periodic status
if (elapsed_ms > 10000) {
    LOG_STATUS("[GUN STATUS @ %.1fs] Firing: %s | Rate: %d | RPM: %d | Trigger PWM: %d µs",
               elapsed, firing_str, rate, rpm, trigger_pwm);
    last_status = now;
}

// Error handling
if (!gun_rate_set) {
    LOG_ERROR(LOG_GUN, "Cannot allocate memory for rates");
    return NULL;
}

// Thread cleanup
LOG_INFO(LOG_GUN, "Processing thread stopped");
```

### In Servo Control

```c
// Initialization
if (!servo) {
    LOG_ERROR(LOG_SERVO, "Config is NULL");
    return NULL;
}

// Configuration
LOG_INIT(LOG_SERVO, "Input: 1000-2000 µs, Output: 1000-2000 µs, "
                    "Speed: 500 µs/s, Rate: 50 Hz");

// State tracking
LOG_STATE(LOG_SERVO, "SETTLING", "AT_TARGET");

// Cleanup
LOG_SHUTDOWN(LOG_SERVO, "Servo instance");
```

## Filtering Logs by Component

### Using journalctl
```bash
# View only gun-related messages
sudo journalctl -u sfxhub | grep "\[GUN\]"

# View only servo messages
sudo journalctl -u sfxhub | grep "\[SERVO\]"

# View only errors
sudo journalctl -u sfxhub | grep "Error:"

# View only warnings
sudo journalctl -u sfxhub | grep "Warning:"
```

### Using grep
```bash
# Filter for a specific component
grep "\[GUN\]" /var/log/syslog

# Filter for status lines
grep "STATUS" /var/log/syslog

# Filter for errors and warnings
grep -E "(Error:|Warning:)" /var/log/syslog
```

## Compilation with Debug Logging

To enable DEBUG logging messages:

```bash
make clean
make CFLAGS="-Wall -Wextra -Werror -Wno-unused-function -std=c11 -pthread -O2 -D_DEFAULT_SOURCE -DDEBUG"
```

Or add to Makefile:
```makefile
CFLAGS = ... -DDEBUG
```

## Best Practices

### 1. **Use Appropriate Log Levels**
- ERROR: Only for failures that prevent operation
- WARN: For degraded functionality
- INFO: For normal state changes and milestones
- DEBUG: For detailed diagnostic information

### 2. **Include Context**
Always include relevant values and parameters:

```c
// Good
LOG_INFO(LOG_GUN, "Rate changed from %d to %d RPM", old_rpm, new_rpm);

// Bad
LOG_INFO(LOG_GUN, "Rate changed");
```

### 3. **Use Status Lines for Periodic Dumps**
For high-frequency telemetry, use `LOG_STATUS()` without component tags:

```c
// Periodically (every 10 seconds)
LOG_STATUS("[GUN STATUS @ %.1fs] Firing: %s | Rate: %d | RPM: %d",
           elapsed, firing_str, rate, rpm);
```

### 4. **Log State Transitions**
Always log significant state changes:

```c
LOG_STATE(LOG_ENGINE, "STOPPED", "STARTING");
LOG_INFO(LOG_ENGINE, "Playing starting sound");
```

### 5. **Be Consistent with Units**
Always specify units for measurements:

```c
LOG_INFO(LOG_SERVO, "Speed limit set to 500 µs/s");      // Good
LOG_INFO(LOG_SERVO, "Speed limit set to 500");            // Bad
```

### 6. **Use Consistent Terminology**
- GPIO pins: "pin N" or "GPIO N"
- PWM: "X µs" (microseconds)
- Delays: "X ms" (milliseconds)
- Frequencies: "X Hz"

```c
LOG_INFO(LOG_GUN, "Trigger PWM monitoring on GPIO %d", pin);
LOG_INFO(LOG_SERVO, "Servo update rate: %d Hz", rate_hz);
```

## Example Log Output

### System Startup
```
[CONFIG] System Configuration
════════════════════════════════════════════════════════════════

ENGINE FX: ✓ ENABLED
  PWM Input Pin: 17 µs
  Throttle Threshold: 1500 µs
  ...

GUN FX: ✓ ENABLED
  Trigger PWM Input Pin: 27
  Nozzle Flash LED: ✓ ENABLED (GPIO 23)
  ...
  Turret Control Servos:
    ├─ PITCH Servo: ✓ ENABLED
    │  PWM Input (GPIO 13) ← Range: 1000-2000 µs
    │  PWM Output (GPIO 7) → Range: 1000-2000 µs
    │  Motion Limits: 500 µs/s speed | 2000 µs/s² accel
    └─ YAW Servo: ✓ ENABLED
       ...

════════════════════════════════════════════════════════════════

[SFXHUB] Initializing Engine FX...
[ENGINE] PWM monitoring started on pin 17 (threshold: 1500 us)
[ENGINE] Engine FX created (channel: 0)
[SFXHUB] Engine FX initialized

[SFXHUB] Initializing Gun FX...
[GUN]    Nozzle flash LED initialized on GPIO 23
[GUN]    Smoke generator initialized (heater GPIO 25, fan GPIO 24)
[GUN]    Trigger PWM monitoring started on GPIO 27
[GUN]    Pitch servo (input pin: 13, output pin: 7)
[GUN]    Yaw servo (input pin: 16, output pin: 8)
[GUN]    Gun FX created
[SFXHUB] Gun FX initialized with 2 rates

[SFXHUB] System ready. Press Ctrl+C to exit.
```

### Runtime Operation
```
[ENGINE] State: STOPPED → STARTING
[ENGINE] Playing starting sound
[ENGINE] Transitioning to RUNNING
[ENGINE] Playing running sound (looping)

[GUN]    State: IDLE → FIRING
[GUN]    Firing started (550 RPM)
[GUN STATUS @ 10.2s] Firing: YES | Rate: 2 | RPM: 550 | Trigger PWM: 1650 µs | Heater: ON
[GUN SERVOS] Pitch: 1500 µs | Yaw: 1600 µs | Pitch Servo: ACTIVE | Yaw Servo: ACTIVE

[SMOKE]  Heater ON
[SMOKE]  Fan ON
```

### Shutdown
```
[SFXHUB] Shutting down...
[SFXHUB] Cleaning up...
[GUN]    Processing thread stopped
[GUN]    Shutdown: Gun FX system
[ENGINE] Processing thread stopped
[ENGINE] Shutdown: Engine FX system
[SFXHUB] Shutdown complete.
```

## Troubleshooting

### No Log Output
Ensure logs are being piped to stdout/stderr and not captured elsewhere.

### Missing Component Logs
Verify that:
1. Component initialization was reached (check for earlier errors)
2. Logging macros are using correct component prefix
3. DEBUG logging is enabled if using `LOG_DEBUG()`

### Excessive Log Output
Use filtering with `grep` or `journalctl` to focus on specific components or log levels.

## Related Documentation
- README.md - System overview
- config.yaml - Configuration file format
