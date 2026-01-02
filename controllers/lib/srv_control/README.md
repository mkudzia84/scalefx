# Servo Control Library

Object-oriented servo output control with trapezoidal motion profiling, acceleration/deceleration, and jerk effects.

## Features

- **Motion Profiling**: Smooth trapezoidal velocity profiles for natural movement
- **Configurable Acceleration**: Independent acceleration and deceleration rates
- **Speed Limiting**: Maximum velocity clamping
- **Position Limits**: Software limits with clamping
- **Jerk Effect**: Offset simulation for gun recoil
- **Event Callbacks**: Notifications for position changes and target reached
- **Immediate Mode**: Option to bypass motion profiling

## Usage

### Basic Servo Control

```cpp
#include <srv_control.h>

ServoControl servo;

void setup() {
    // Initialize servo on pin 1 with limits 500-2500us
    servo.begin(1, 500, 2500);
    
    // Configure motion profile (optional - has defaults)
    servo.setMotionProfile(
        4000,   // max speed: 4000 us/second
        8000,   // acceleration: 8000 us/second^2
        8000    // deceleration: 8000 us/second^2
    );
}

void loop() {
    // Set target position (servo moves smoothly)
    servo.setTarget(1800);
    
    // Update motion - call frequently!
    servo.update();
}
```

### Motion Profile Configuration

```cpp
// Set all motion parameters at once
servo.setMotionProfile(4000, 8000, 8000);

// Or set individually
servo.setMaxSpeed(4000);      // us per second
servo.setAcceleration(8000);  // us per second^2
servo.setDeceleration(8000);  // us per second^2

// Set position limits
servo.setLimits(600, 2400);
```

### Immediate Position (No Motion Profile)

```cpp
// Jump directly to position without motion profiling
servo.setPositionImmediate(1500);
```

### Jerk Offset

Apply a temporary offset to the servo position (e.g., for recoil simulation):

```cpp
void onShot() {
    // Apply jerk offset (positive or negative)
    servo.applyJerk(-50);  // Move 50us in negative direction
}

void onShotComplete() {
    // Clear jerk offset after effect ends
    servo.clearJerk();
}
```

The jerk value is added to the current position but clamped to stay within the servo's motion limits.

### Position Query

```cpp
void loop() {
    servo.update();
    
    // Get current positions
    int current = servo.position();  // Actual position
    int target = servo.target();     // Target position
    
    // Check motion state
    if (servo.isMoving()) {
        Serial.printf("Moving at %.1f us/s\n", servo.velocity());
    }
    
    if (servo.atTarget()) {
        Serial.println("Reached target");
    }
}
```

### Event Callbacks

```cpp
void onPositionChange(ServoControl& srv, int positionUs) {
    Serial.printf("Servo %d position: %d us\n", srv.id(), positionUs);
}

void onTargetReached(ServoControl& srv) {
    Serial.printf("Servo %d reached target\n", srv.id());
}

void setup() {
    servo.begin(1, 500, 2500);
    servo.setId(1);  // Optional user ID
    
    servo.onPositionChange(onPositionChange);
    servo.onTargetReached(onTargetReached);
}
```

### Managing Multiple Servos

```cpp
ServoControl servos[3];

void setup() {
    int pins[] = {1, 2, 3};
    for (int i = 0; i < 3; i++) {
        servos[i].begin(pins[i], 500, 2500);
        servos[i].setId(i + 1);
        servos[i].setMotionProfile(4000, 8000, 8000);
    }
}

void loop() {
    // Update all servos
    for (int i = 0; i < 3; i++) {
        servos[i].update();
    }
}

void applyJerkToAll(int jerkUs) {
    for (int i = 0; i < 3; i++) {
        servos[i].applyJerk(jerkUs);
    }
}
```

## API Reference

### ServoControl Class

#### Initialization

| Method | Description |
|--------|-------------|
| `begin(pin, minUs, maxUs, initialUs)` | Initialize servo on pin with limits |
| `end()` | Detach servo and release resources |
| `isAttached()` | Check if servo is initialized |

#### Configuration

| Method | Description |
|--------|-------------|
| `setLimits(minUs, maxUs)` | Set position limits |
| `setMotionProfile(maxSpeed, accel, decel)` | Set all motion parameters |
| `setMaxSpeed(maxSpeed)` | Set maximum speed (us/s) |
| `setAcceleration(accel)` | Set acceleration (us/s²) |
| `setDeceleration(decel)` | Set deceleration (us/s²) |
| `setId(id)` | Set user-defined ID |

#### Jerk Offset

| Method | Description |
|--------|-------------|
| `applyJerk(jerkUs)` | Apply offset in microseconds (+/-) |
| `clearJerk()` | Clear jerk offset |
| `jerkOffset()` | Get current jerk offset |

#### Position Control

| Method | Description |
|--------|-------------|
| `setTarget(targetUs)` | Set target position (with motion profile) |
| `setPositionImmediate(positionUs)` | Jump to position immediately |
| `update()` | Update motion (call frequently) |
| `update(nowMs)` | Update with explicit timestamp |

#### Getters

| Method | Description |
|--------|-------------|
| `position()` | Get current position (us) |
| `target()` | Get target position (us) |
| `velocity()` | Get current velocity (us/s) |
| `isMoving()` | Check if servo is moving |
| `atTarget()` | Check if at target position |
| `minLimit()` / `maxLimit()` | Get position limits |
| `maxSpeed()` / `acceleration()` / `deceleration()` | Get motion settings |
| `pin()` | Get GPIO pin |
| `id()` | Get user ID |

#### Callbacks

| Method | Description |
|--------|-------------|
| `onPositionChange(callback)` | Set position change callback |
| `onTargetReached(callback)` | Set target reached callback |

### Configuration Constants (ServoControlConfig namespace)

| Constant | Value | Description |
|----------|-------|-------------|
| `DEFAULT_MAX_SPEED` | 4000 | Default max speed (us/s) |
| `DEFAULT_ACCEL` | 8000 | Default acceleration (us/s²) |
| `DEFAULT_DECEL` | 8000 | Default deceleration (us/s²) |
| `DEFAULT_MIN_US` | 500 | Default minimum limit |
| `DEFAULT_MAX_US` | 2500 | Default maximum limit |
| `DEFAULT_CENTER_US` | 1500 | Default center position |
| `ABSOLUTE_MIN_US` | 300 | Hardware minimum limit |
| `ABSOLUTE_MAX_US` | 2700 | Hardware maximum limit |

### Callback Types

```cpp
// Position change callback (fires when position changes by ~5us)
using ServoPositionCallback = std::function<void(ServoControl& servo, int positionUs)>;

// Target reached callback (fires when motion stops at target)
using ServoTargetReachedCallback = std::function<void(ServoControl& servo)>;
```

## Motion Profile Theory

The servo uses trapezoidal velocity profiling:

1. **Acceleration phase**: Velocity increases at `acceleration` rate until `maxSpeed` is reached
2. **Cruise phase**: Constant velocity at `maxSpeed` (if distance allows)
3. **Deceleration phase**: Velocity decreases at `deceleration` rate to stop at target

For short moves, the velocity may not reach `maxSpeed` before deceleration begins.

```
Velocity
    ^
    |     ___________
    |    /           \
    |   /             \
    |  /               \
    | /                 \
    +-----------------------> Time
      Accel  Cruise  Decel
```

## Dependencies

- Arduino framework (earlephilhower/arduino-pico)
- Servo library
- Raspberry Pi Pico (RP2040)
- C++11 or later (for std::function)
