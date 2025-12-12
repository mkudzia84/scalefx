# JetiEX Quick Reference

## Configuration Template

```yaml
jetiex:
  enabled: true
  serial_port: "/dev/ttyAMA0"
  baud_rate: 115200
  manufacturer_id: 0xA409
  device_id: 0x0001
  update_rate_hz: 10
  text_messages: true
  
  sensors:
    gun_rpm:
      enabled: true
      sensor_id: 0
      label: "Gun RPM"
    
    engine_rpm:
      enabled: true
      sensor_id: 1
      label: "Engine RPM"
    
    battery_voltage:
      enabled: true
      sensor_id: 2
      label: "Battery"
      precision: 1
    
    temperature:
      enabled: true
      sensor_id: 3
      label: "Temp"
      precision: 0
```

## Quick API Reference

### Initialization
```c
JetiEXConfig config = {
    .serial_port = "/dev/ttyAMA0",
    .baud_rate = 115200,
    .manufacturer_id = 0xA409,
    .device_id = 0x0001,
    .update_rate_hz = 10,
    .text_messages = true
};

JetiEX *jetiex = jetiex_create(&config);
```

### Adding Sensors
```c
// RPM sensor
JetiEXSensor rpm = jetiex_sensor_rpm(0, "Gun RPM");
jetiex_add_sensor(jetiex, &rpm);

// Voltage sensor (1 decimal place)
JetiEXSensor voltage = jetiex_sensor_voltage(1, "Battery", 1);
jetiex_add_sensor(jetiex, &voltage);

// Temperature sensor (0 decimal places)
JetiEXSensor temp = jetiex_sensor_temperature(2, "Temp", 0);
jetiex_add_sensor(jetiex, &temp);

// Percentage sensor
JetiEXSensor ammo = jetiex_sensor_percentage(3, "Ammo");
jetiex_add_sensor(jetiex, &ammo);
```

### Starting/Stopping
```c
jetiex_start(jetiex);    // Start telemetry thread
jetiex_stop(jetiex);     // Stop telemetry thread
jetiex_destroy(jetiex);  // Cleanup
```

### Updating Sensors
```c
// Update RPM
jetiex_update_sensor(jetiex, 0, 550);

// Update voltage (12.6V with precision 1 = 126)
jetiex_update_sensor(jetiex, 1, 126);

// Update temperature (25°C)
jetiex_update_sensor(jetiex, 2, 25);

// Update percentage (75%)
jetiex_update_sensor(jetiex, 3, 75);
```

### Text Messages
```c
jetiex_send_text(jetiex, "Gun Firing!");
jetiex_send_text(jetiex, "Engine Started");
jetiex_send_text(jetiex, "Battery Low");
```

## GPIO Wiring

```
Raspberry Pi                    Jeti Receiver
------------                    -------------
GPIO 14 (TX) ----[1kΩ]----->   EX Bus
     GND     ---------------->   GND
```

## UART Setup

```bash
# Edit /boot/config.txt
sudo nano /boot/config.txt

# Add these lines:
dtoverlay=disable-bt
enable_uart=1

# Save and reboot
sudo reboot

# Add user to dialout group
sudo usermod -a -G dialout $USER
# Logout/login for changes to take effect
```

## Build Commands

```bash
# Build everything
make

# Build debug version
make debug

# Install system-wide
sudo make install
```

## Common Sensor Configurations

### Gun RPM (22-bit integer)
- **ID**: 0
- **Label**: "Gun RPM"
- **Range**: 0-2,097,151
- **Update**: On firing rate change

### Engine RPM (22-bit integer)
- **ID**: 1
- **Label**: "Engine RPM"
- **Range**: 0-2,097,151
- **Update**: From throttle PWM

### Battery Voltage (14-bit, 1 decimal)
- **ID**: 2
- **Label**: "Battery"
- **Range**: 0.0-819.1V
- **Precision**: 1
- **Value**: voltage * 10 (e.g., 12.6V = 126)

### Temperature (14-bit, 0 decimals)
- **ID**: 3
- **Label**: "Temp"
- **Range**: -8191 to +8191°C
- **Precision**: 0
- **Value**: degrees (e.g., 25°C = 25)

### Ammunition (14-bit percent)
- **ID**: 4
- **Label**: "Ammo"
- **Range**: 0-100%
- **Value**: percentage (e.g., 75% = 75)

## Troubleshooting

### No telemetry
```bash
# Check UART device
ls -l /dev/ttyAMA0

# Test serial output
./build/helifx &
sudo cat /dev/ttyAMA0 | hexdump -C

# Check permissions
sudo usermod -a -G dialout $USER
```

### Corrupt data
- Reduce update_rate_hz to 5
- Try lower baud rate (9600)
- Check wiring and ground connection
- Add 1kΩ resistor on TX line

### High CPU
- Reduce update_rate_hz to 10 or lower
- Disable unused sensors

## Update Rate Guidelines

| Rate | Use Case | CPU Impact |
|------|----------|------------|
| 5 Hz | Basic monitoring | Very Low |
| 10 Hz | **Recommended** | Low |
| 20 Hz | Fast changes | Medium |
| 50+ Hz | Not recommended | High |

## Baud Rate Guidelines

| Baud | Reliability | Recommended |
|------|-------------|-------------|
| 9600 | Excellent | Development |
| 19200 | Very Good | |
| 57600 | Good | |
| **115200** | Fair | **Production** |
| 230400 | Poor | Not recommended |

## Log Messages

With `-DDEBUG` flag:
```
[JETIEX] Initialized on /dev/ttyAMA0 at 115200 baud (Mfr:0xA409 Dev:0x0001)
[JETIEX] Added sensor #0: Gun RPM (rpm)
[JETIEX] Telemetry started at 10 Hz
[JETIEX] Sending telemetry packet (15 bytes, 2 sensors)
[JETIEX] Sensor #0 updated: 550 rpm
[JETIEX] Text message queued: Gun Firing!
[JETIEX] Stopping telemetry...
```

## Files Reference

- **Header**: `include/jetiex.h`
- **Implementation**: `src/jetiex.c`
- **Documentation**: `docs/JETIEX.md`
- **Integration Guide**: `docs/JETIEX_INTEGRATION.md`
