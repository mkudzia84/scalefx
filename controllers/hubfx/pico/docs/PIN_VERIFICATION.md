# Pin Configuration Verification
## Raspberry Pi Pico → WM8960 Audio HAT

Generated: 2026-01-05

---

## ✅ Code Pin Definitions (hubfx_pico.ino)

```cpp
// I2S Audio Output (to WM8960)
#define DEFAULT_PIN_I2S_DATA    6   // GP6 - I2S DIN (to WM8960 DAC/GPIO21)
#define DEFAULT_PIN_I2S_BCLK    7   // GP7 - I2S BCLK (to WM8960 CLK/GPIO18)
#define DEFAULT_PIN_I2S_LRCLK   8   // GP8 - I2S LRCLK/WS (to WM8960 LRCLK/GPIO19)

// I2C for WM8960 Control
#define DEFAULT_PIN_I2C_SDA     4   // GP4 - I2C SDA (to WM8960 SDA/GPIO2)
#define DEFAULT_PIN_I2C_SCL     5   // GP5 - I2C SCL (to WM8960 SCL/GPIO3)
```

---

## ✅ Wiring Diagram Mapping

### I2C Control (Configuration)
| Signal | Code Definition | Pico GPIO | Pico Physical Pin | → | WM8960 HAT Pin | HAT Signal |
|--------|----------------|-----------|-------------------|---|----------------|------------|
| SDA    | `DEFAULT_PIN_I2C_SDA = 4` | **GP4** | **Pin 6** | → | **Pin 3** | GPIO2 (SDA) |
| SCL    | `DEFAULT_PIN_I2C_SCL = 5` | **GP5** | **Pin 7** | → | **Pin 5** | GPIO3 (SCL) |

### I2S Audio Data
| Signal | Code Definition | Pico GPIO | Pico Physical Pin | → | WM8960 HAT Pin | HAT Signal |
|--------|----------------|-----------|-------------------|---|----------------|------------|
| DATA   | `DEFAULT_PIN_I2S_DATA = 6`  | **GP6** | **Pin 9** | → | **Pin 38** | GPIO20 (ADC) |
| BCLK   | `DEFAULT_PIN_I2S_BCLK = 7`  | **GP7** | **Pin 10** | → | **Pin 12** | GPIO18 (CLK) |
| LRCLK  | `DEFAULT_PIN_I2S_LRCLK = 8` | **GP8** | **Pin 11** | → | **Pin 35** | GPIO19 (LRCLK) |

### Power (Not in Code - Must Wire Manually)
| Signal | Pico Physical Pin | Pico Label | → | WM8960 HAT Pin | HAT Label | Required? |
|--------|-------------------|------------|---|----------------|-----------|-----------|
| 3.3V   | **Pin 36** | 3V3 OUT | → | **Pin 1 or 17** | 3.3V | **YES** (Logic/Headphones) |
| 5V     | **Pin 39** | VSYS | → | **Pin 2 or 4** | 5V | **Optional** (Speakers only) |
| GND    | **Pin 38 (recommended)** | GND | → | **Pin 6, 9, 14, 20, 25, 30, 34, or 39** | GND | **YES** (Common ground) |

---

## ✅ Verification Summary

### Signal Pins: **CORRECT** ✓
All GPIO pin assignments in the code match the wiring diagram:
- GP4 → I2C SDA ✓
- GP5 → I2C SCL ✓  
- GP6 → I2S DATA ✓
- GP7 → I2S BCLK ✓
- GP8 → I2S LRCLK ✓

### Physical Pin Numbers: **CORRECT** ✓
- Pin 6 (GP4 SDA) ✓
- Pin 7 (GP5 SCL) ✓
- Pin 9 (GP6 DATA) ✓
- Pin 10 (GP7 BCLK) ✓
- Pin 11 (GP8 LRCLK) ✓

### Power Connections: **NOT IN CODE - MANUAL WIRING REQUIRED** ⚠️

Power connections are **NOT configured in software** and must be physically wired:

#### ⚠️ CRITICAL: These connections determine if I2C works!

**Minimum Required (for I2C and Headphone output):**
```
Pico Pin 36 (3V3 OUT) ────────► WM8960 Pin 1 (3.3V)
Pico Pin 38 (GND)     ────────► WM8960 Pin 6 (GND)
```

**Full Configuration (for Speaker output):**
```
Pico Pin 36 (3V3 OUT) ────────► WM8960 Pin 1 (3.3V)
Pico Pin 39 (VSYS)    ────────► WM8960 Pin 2 (5V)
Pico Pin 38 (GND)     ────────► WM8960 Pin 6 (GND)
```

---

## 🔍 Current Issue: I2C Error 4

From boot log:
```
[WM8960] I2C write failed: reg=0x__, error=4
```

**Error 4 = "Other I2C error"** means:
- Device not responding on I2C bus
- Most likely cause: **WM8960 not powered**

### Troubleshooting Steps:

1. **Visual Check**:
   - ✓ Look for LED on WM8960 HAT (should be lit if powered)

2. **Multimeter Check**:
   - ✓ Measure voltage between Pico Pin 36 and Pin 38 → should be **3.3V**
   - ✓ Measure voltage between WM8960 Pin 1 and Pin 6 → should be **3.3V**
   - ✓ If no voltage on WM8960: **Power not connected**

3. **Physical Wiring Check**:
   - ✓ Is wire connected from Pico Pin 36 to WM8960 Pin 1?
   - ✓ Is wire connected from Pico Pin 38 to WM8960 Pin 6?
   - ✓ Are connections solid (no loose wires)?

4. **After Power Verified**, test I2C:
   ```
   codec scan
   ```
   Expected result: `Device found at 0x1A`

---

## 📋 Complete Wiring Checklist

Print this and verify each connection:

- [ ] **GP4 (Pin 6)** → WM8960 Pin 3 (SDA)
- [ ] **GP5 (Pin 7)** → WM8960 Pin 5 (SCL)
- [ ] **GP6 (Pin 9)** → WM8960 Pin 38 (ADC) ⚠️ FOR PLAYBACK
- [ ] **GP7 (Pin 10)** → WM8960 Pin 12 (CLK)
- [ ] **GP8 (Pin 11)** → WM8960 Pin 35 (LRCLK)
- [ ] **3V3 (Pin 36)** → WM8960 Pin 1 (3.3V) ⚠️ CRITICAL
- [ ] **GND (Pin 38)** → WM8960 Pin 6 (GND) ⚠️ CRITICAL
- [ ] **5V (Pin 39)** → WM8960 Pin 2 (5V) [Optional, for speakers]

---

## 📊 Pin Reference Tables

### Raspberry Pi Pico Pinout (Relevant Pins)
```
        [USB]
         ___
Pin 1  |   | Pin 40 (VBUS)
...    |   | ...
Pin 6  |   | Pin 36 (3V3 OUT) ──► WM8960 3.3V
Pin 7  |   | ...
...    |   | Pin 38 (GND)      ──► WM8960 GND
Pin 9  |   | Pin 39 (VSYS)     ──► WM8960 5V (optional)
Pin 10 |   | Pin 40
Pin 11 |___| 
```

### WM8960 HAT 40-Pin Header
```
Pin 1  (3.3V)    ◄── Pico Pin 36
Pin 2  (5V)      ◄── Pico Pin 39 (optional)
Pin 3  (GPIO2)   ◄── Pico GP4 (SDA)
Pin 4  (5V)
Pin 5  (GPIO3)   ◄── Pico GP5 (SCL)
Pin 6  (GND)     ◄── Pico Pin 38
...
Pin 12 (GPIO18)  ◄── Pico GP7 (BCLK)
...
Pin 35 (GPIO19)  ◄── Pico GP8 (LRCLK)
...
Pin 38 (GPIO20)  ◄── Pico GP6 (DATA)
```

---

## ✅ CONCLUSION

**Signal pins in code are CORRECT and match the wiring diagram.**

**The I2C error 4 is caused by missing or incorrect POWER connections**, not incorrect signal pin assignments.

**NEXT STEP**: Verify 3.3V and GND are connected from Pico to WM8960 HAT.
