# HubFX Pico - Test Scripts

Automated testing and verification scripts for firmware functionality.

## Available Tests

### Hardware Tests

#### ```test_i2c_scan.py```
Scans I2C bus for connected devices:
```bash
python tests/test_i2c_scan.py
```
Expected devices:
- ```0x1A``` - WM8960 codec (if connected and powered)

#### ```test_codec.py```
Comprehensive codec testing:
```bash
python tests/test_codec.py
```
Tests:
- I2C communication (```codec scan```)
- Device response (```codec test```)
- Register access (```codec status```)

### Boot and Serial Tests

#### ```check_boot.py```
Captures and displays boot log:
```bash
python tests/check_boot.py
```
Useful for:
- Verifying firmware version and build number
- Checking initialization sequence
- Debugging startup issues

#### ```test_serial.py```
Basic serial communication test:
```bash
python tests/test_serial.py
```
Sends commands and verifies responses.

### Output Format Tests

#### ```test_json_output.py```
Tests JSON-formatted command output:
```bash
python tests/test_json_output.py
```
Validates JSON parsing for programmatic access.

#### ```test_extended.py```
Extended functionality testing:
```bash
python tests/test_extended.py
```
Tests advanced features and edge cases.

## Common Issues

### COM Port Access
If you get "Access denied" errors:
1. Close any serial monitors (Arduino IDE, PlatformIO, PuTTY)
2. Disconnect/reconnect USB cable
3. Check Device Manager for correct COM port

### Device Not Responding
If tests timeout:
1. Verify firmware is flashed: ```python tests/check_boot.py```
2. Check USB connection
3. Try power cycling the Pico
4. Verify COM port with ```mode``` command (Windows)

### I2C Errors
- **Error 4 (no device):** Check 3.3V/GND power to codec
- **Error 5 (timeout):** Lower I2C speed in ```audio_config.h``` to 50kHz or 10kHz

## See Also

- [../docs/AUDIO_CONFIGURATION.md](../docs/AUDIO_CONFIGURATION.md) - I2C speed tuning
- [../scripts/](../scripts/) - Build and flash utilities
