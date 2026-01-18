# Software PWM Implementation

## Overview

The software PWM implementation provides a flexible way to generate PWM signals on any GPIO pin without requiring hardware PWM support. This is particularly useful for servo control and other applications requiring precise pulse-width modulation.

## Architecture

### Components

1. **PWMEmitter Structure**
   - `pin`: GPIO pin number (BCM numbering)
   - `feature_name`: Descriptive name for logging
   - `value_us`: Current pulse width in microseconds (atomic)
   - `active`: Status flag (atomic)

2. **PWM Emitting Thread**
   - Single background thread manages all PWM emitters
   - 50Hz update rate (20ms period)
   - Generates PWM signals by toggling GPIO pins at precise intervals

### How It Works

The PWM emitting thread operates in a continuous loop:

1. **Phase 1**: At the start of each 20ms cycle, all active emitters with non-zero values have their pins set HIGH
2. **Phase 2**: As time progresses through the cycle, pins are set LOW when their configured pulse width expires
3. **Timing**: The thread samples at 100μs intervals to balance precision with CPU usage
4. **Synchronization**: All emitters are synchronized to the same 20ms cycle for consistent timing

## API Usage

### Initialization

```c
#include "gpio.h"

// Initialize GPIO subsystem first
gpio_init();

// Create PWM emitters
PWMEmitter *servo1 = pwm_emitter_create(7, "pitch_servo");
PWMEmitter *servo2 = pwm_emitter_create(8, "yaw_servo");
```

### Setting PWM Values

```c
// Set pulse width in microseconds (0-20000 for 50Hz PWM)
// Typical servo range: 1000-2000μs
pwm_emitter_set_value(servo1, 1500);  // Center position
pwm_emitter_set_value(servo2, 2000);  // Maximum position
```

### Reading Current Values

```c
int current_value = pwm_emitter_get_value(servo1);
printf("Current pulse width: %d us\n", current_value);
```

### Cleanup

```c
// Destroy emitters when done
pwm_emitter_destroy(servo1);
pwm_emitter_destroy(servo2);

// Cleanup GPIO subsystem
gpio_cleanup();
```

## Configuration

### PWM Parameters

- **Period**: 20ms (50Hz) - standard for servo control
- **Pulse Width Range**: 0-20000μs
  - Servos typically use 1000-2000μs
  - 0μs = always LOW
  - 20000μs = always HIGH
- **Update Interval**: 5μs (provides 200 discrete positions over 1000-2000μs servo range)
- **Resolution**: 0.5% per step - very smooth servo control

### Pin Selection

- Any GPIO pin can be used (except audio HAT reserved pins)
- Audio HAT reserves: GPIO 2, 3, 18-22
- Maximum 8 simultaneous emitters (configurable via `MAX_PWM_EMITTERS`)

## Performance Considerations

### CPU Usage

- The emitting thread runs continuously with 100μs sleep intervals
- CPU usage scales with the number of active emitters
- Typical usage: <5% CPU on Raspberry Pi Zero for 2-3 servos

### Timing Precision

- Software PWM timing is affected by system load
- For critical timing requirements, consider hardware PWM
- Real-time priority can be set via `nice` or `chrt` if needed

### Thread Safety

- All API functions are thread-safe
- Atomic operations ensure consistent pulse widths
- Multiple threads can safely update different emitters

## Integration with Servo Control

The PWM emitter integrates seamlessly with the servo control module:

```c
// In gun_fx.c or similar
PWMEmitter *pitch_emitter = pwm_emitter_create(pitch_output_pin, "pitch");
PWMEmitter *yaw_emitter = pwm_emitter_create(yaw_output_pin, "yaw");

// In servo update loop
int pitch_output = servo_get_output(pitch_servo);
int yaw_output = servo_get_output(yaw_servo);

pwm_emitter_set_value(pitch_emitter, pitch_output);
pwm_emitter_set_value(yaw_emitter, yaw_output);
```

## Advantages Over Hardware PWM

1. **Flexibility**: Works on any GPIO pin, not limited to hardware PWM channels
2. **Simplicity**: No kernel module configuration required
3. **Portability**: Works across different Raspberry Pi models
4. **Multiple Outputs**: Support for 8+ PWM outputs (vs 2 hardware PWM channels)

## Limitations

1. **Timing Precision**: Less precise than hardware PWM due to OS scheduling
2. **CPU Usage**: Requires active CPU time (hardware PWM is offloaded)
3. **System Load Sensitivity**: Heavy system load can affect timing

## Future Enhancements

Possible improvements:
- Real-time thread priority for improved timing
- Configurable update rate (currently 50Hz)
- DMA-based PWM generation for reduced CPU usage
- Variable precision mode (trade CPU vs timing accuracy)
