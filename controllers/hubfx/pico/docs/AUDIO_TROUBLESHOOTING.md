# Audio Troubleshooting Guide

## No Sound Output - Diagnostic Steps

When you don't hear any audio output from the Pico, follow these steps systematically:

---

### Step 1: Run Codec Diagnostic

Type this command in the serial console:

```
codec status
audio status
```

**Expected Output:**
```
=== WM8960 Codec Status ===
Initialized: YES
I2C Interface: Connected

I2C Test:
  I2C write test: SUCCESS

Key Registers:
  POWER1 (0x19): 0x00FE
  POWER2 (0x1A): 0x01FF
  POWER3 (0x2F): 0x000C
  IFACE1 (0x07): 0x0080
  CLOCK1 (0x04): 0x0001
  DACCTL1 (0x05): 0x0000
===========================
```

**Check These:**
- **Initialized**: Should be `YES` - if `NO`, codec didn't initialize
- **I2C write test**: Should be `SUCCESS` - if failed, check I2C wiring

**If "ERROR: NACK on address":**
- WM8960 not detected on I2C bus
- Check I2C wiring (SDA = GP4, SCL = GP5)
- Check I2C address (should be 0x1A)
- Try: `codec scan` to see all I2C devices
- Try: `codec recover` if I2C bus is stuck

---

### Step 2: Test Audio Playback

Try playing an audio file from the SD card:

```
audio play 0 /sounds/test.wav
```

**Expected:**
```
[MAIN] Playing audio file: /sounds/test.wav
```

Check audio status:

```
audio status
```

**Expected:**
```
=== Audio Status ===
Channel 0: PLAYING

Master Volume: 1.00
```

**If channel not playing:**
- SD card may not have audio files
- File may not be in correct format (16-bit WAV, 44.1kHz recommended)
- Check SD card initialization in boot log

**If playing but no sound:**
- Issue is with I2S transmission or codec (Step 6)

---

### Step 6: Verify Codec Settings

Check codec volume and mute status:

```
audio vol
audio status
```

**Expected:**
```
Master volume: 100%
```

**If muted or volume 0:**
```
audio vol 100
```

Try adjusting codec-specific volumes:

```
codec status
```

Look for headphone/speaker volume registers. The WM8960 should show:
- `LOUT1` (headphone left): ~0x17F (0dB)
- `ROUT1` (headphone right): ~0x17F (0dB)
- `LOUT2` (speaker left): ~0x17F (0dB)
- `ROUT2` (speaker right): ~0x17F (0dB)

**If volumes are too low:**

The codec may need higher volume. Try this manual override:

```
codec write 0x02 0x17F
codec write 0x03 0x17F
codec write 0x28 0x17F
codec write 0x29 0x17F
```

---

### Step 7: Check I2S Wiring

**CRITICAL: I2S requires short, matched-length wires!**

From [WIRING.md](WIRING.md) and [wm8960_codec.cpp](../src/audio/wm8960_codec.cpp):

```
Clock Frequencies:
  BCLK:   2,822,400 Hz  (2.8 MHz)
  LRCLK:     44,100 Hz  (44.1 kHz)

Signal Integrity Requirements:
  • Wire Length: < 6 inches (< 150mm) for ALL I2S signals
  • Wire Matching: BCLK, LRCLK, DATA within ±1 inch
  • Ground Return: Run GND parallel to each signal
  • Wire Gauge: 22-26 AWG solid core
  • Separation: Keep away from power/servo wires
```

**Common Issues:**
- Wires > 6 inches → signal degradation, clicks, pops
- Unequal wire lengths → clock/data skew → bit errors
- No ground return → noise, intermittent dropouts
- Twisted with power wires → EMI interference

**Test with Oscilloscope (if available):**
- BCLK: Should see clean 2.8 MHz square wave
- LRCLK: Should see 44.1 kHz square wave
- DATA: Should see changing bit patterns synchronized to BCLK

---

### Step 8: Check Speakers/Headphones

**Physical Checks:**
1. Are speakers/headphones plugged into the correct jack?
   - WM8960 HAT usually has separate headphone and speaker outputs
2. Are speakers powered on (if active speakers)?
3. Try different speakers/headphones
4. Check speaker polarity (+ and -)

**Volume Test:**
Try adjusting volumes:

```
audio master 1.0
audio volume 0 1.0
```

Then try playing audio again.

---

### Step 9: Codec Re-initialization

Sometimes the codec needs a hard reset:

```
codec reinit
```

This will:
1. Reset the WM8960 to default state
2. Re-run PLL configuration
3. Re-enable DAC and outputs
4. Set default volumes

**After reinit:**
```
audio play 0 /sounds/test.wav
```

---

### Step 10: I2C Bus Scan

Check if the codec is visible on the I2C bus:

```
codec scan
```

**Expected:**
```
Scanning I2C bus...
  Device found at 0x1A
Found 1 device(s)
```

**If no devices found:**
- I2C wiring problem
- Check SDA (GP4) and SCL (GP5) connections
- Check I2C pull-up resistors (should be on WM8960 HAT)
- Verify WM8960 HAT is receiving power

**If wrong address:**
- Some WM8960 boards use 0x34 (with CSB pin pulled high)
- Check [wm8960_codec.cpp](../src/audio/wm8960_codec.cpp) line 23: `#define WM8960_I2C_ADDR 0x1A`

---

## Common Issues and Solutions

### Issue: "I2S Write Count: 0" (no data transmitted)

**Cause:** Core 1 audio task not running or mixer not processing

**Solution:**
1. Check serial output during boot - should see "[MAIN] Core 1 started (Audio processing)"
2. Verify `audio_initialized` is `true` in serial log
3. Try restarting the Pico

---

### Issue: I2S count increasing but no sound

**Cause:** Codec not outputting audio (I2S data is being sent but codec isn't converting to analog)

**Solution:**
1. Run `codec status` - check "Initialized: YES"
2. Run `codec test` - should see "I2C write test: SUCCESS"
3. Check `POWER1`, `POWER2`, `POWER3` registers - should not be 0x0000
4. Try `codec reinit`
5. Check physical speaker/headphone connection

---

### Issue: Audio clicks, pops, or distortion

**Cause:** Signal integrity issues at 2.8 MHz BCLK

**Solution:**
1. **Shorten all I2S wires** (< 6 inches)
2. Use twisted pairs or parallel wires for signal + ground
3. Keep I2S wires away from servo/motor wires
4. Add capacitors: 100nF near Pico I2S pins, 10µF on codec AVDD
5. Check ground connections (poor grounding causes massive noise)

---

### Issue: "ERROR: NACK on address (device not found)"

**Cause:** I2C communication failure with WM8960

**Solution:**
1. Check I2C wiring: SDA = GP4, SCL = GP5
2. Verify I2C pull-up resistors present (usually on HAT)
3. Check WM8960 HAT power supply (3.3V and GND)
4. Run `codec scan` to see if device appears at any address
5. Try lower I2C speed: Check [audio_config.h](../src/audio/audio_config.h):
   ```cpp
   #define WM8960_I2C_SPEED 50000  // 50 kHz (default)
   ```
   Some setups work better at 50 kHz vs 100 kHz due to capacitance.

---

### Issue: Left or right channel silent

**Cause:** Routing or codec configuration issue

**Solution:**
1. Test with stereo audio file:
   ```
   audio play 0 /sounds/stereo_test.wav
   ```
   - Left should be 300 Hz, Right should be 600 Hz
2. Check codec output mixer registers:
   ```
   codec status
   ```
   - LOUTMIX (0x22) should be ~0x100 (DAC to left output)
   - ROUTMIX (0x25) should be ~0x100 (DAC to right output)
3. Check speaker wiring (swap L/R to verify)

---

## Quick Command Reference

```bash
# Boot verification
<serial monitor at 115200 baud>

# Codec checks
codec status
codec test
codec scan

# Audio playback
audio play 0 /sounds/test.wav
audio status
audio stop 0

# Volume checks
audio master 1.0
audio volume 0 1.0

# Emergency reset
codec reinit
codec recover
```

---

## Still No Sound?

If you've completed all steps and still hear nothing:

1. **Hardware Verification:**
   - Test WM8960 HAT with known-working software (e.g., aplay on Raspberry Pi)
   - Verify Pico boots correctly (LED blinks or serial output)
   - Check all power rails with multimeter (3.3V, GND)

2. **Firmware Verification:**
   - Reflash firmware: Put Pico in BOOTSEL mode, copy firmware.uf2
   - Try Build 101 vs Build 102 (in case of regression)
   - Enable verbose debug in [audio_config.h](../src/audio/audio_config.h):
     ```cpp
     #define AUDIO_DEBUG_TIMING 1
     ```

3. **Contact Support:**
   - Provide output of `codec status` and `audio status`
   - Provide output of `codec test`
   - Describe physical setup (HAT model, speaker type, wire lengths)
   - Attach serial log from boot to test execution

---

## Additional Resources

- [WIRING.md](WIRING.md) - Detailed wiring guide with timing requirements
- [WM8960 Datasheet](https://www.cirrus.com/products/wm8960/) - Full register map and specifications
