package main

// ScaleFX CLI - GunFX Response Parsers

import (
	"fmt"
	"strings"
)

func parseGunFXStatus(data []byte) {
	// Wire format (28 bytes):
	//   [flags:u8][fanSpeed:u8][fanOffMs:u16]
	//   [servo0:u16][servo1:u16][servo2:u16]
	//   [rpm:u16][shots:u32][heaterMs:u32]
	//   [heaterError:u8][fanError:u8]
	//   [heaterDuty:u8][fanDuty:u8]
	//   [batteryV_mV:u16][cellCount:u8][batteryPct:u8]
	if len(data) < 20 {
		fmt.Printf("  GunFX: (incomplete: %d bytes)\n", len(data))
		return
	}

	flags := data[0]
	firing := flags&0x01 != 0
	flashActive := flags&0x02 != 0
	flashFading := flags&0x04 != 0
	heaterOn := flags&0x08 != 0
	fanOn := flags&0x10 != 0
	fanSpindown := flags&0x20 != 0

	fanSpeed := data[1]
	fanOffMs := ReadU16LE(data, 2)
	servo0 := ReadU16LE(data, 4)
	servo1 := ReadU16LE(data, 6)
	servo2 := ReadU16LE(data, 8)
	rpm := ReadU16LE(data, 10)
	shots := ReadU32LE(data, 12)
	heaterMs := ReadU32LE(data, 16)

	// Build state flags string
	var stateParts []string
	if firing {
		stateParts = append(stateParts, colorize(colorRed, "FIRING"))
	}
	if flashActive {
		stateParts = append(stateParts, "FLASH")
	}
	if flashFading {
		stateParts = append(stateParts, "FADING")
	}
	if heaterOn {
		stateParts = append(stateParts, colorize(colorYellow, "HEATER"))
	}
	if fanOn {
		stateParts = append(stateParts, "FAN")
	}
	if fanSpindown {
		stateParts = append(stateParts, "SPINDOWN")
	}
	stateStr := "IDLE"
	if len(stateParts) > 0 {
		stateStr = strings.Join(stateParts, ", ")
	}

	fmt.Printf("  ── GunFX ──────────────────────\n")
	fmt.Printf("  State:     %s\n", stateStr)

	// Muzzle flash
	if firing {
		fmt.Printf("  Fire rate: %d RPM\n", rpm)
	}
	fmt.Printf("  Shots:     %d\n", shots)

	// Fan
	if fanOn || fanSpindown {
		fanInfo := fmt.Sprintf("speed=%d", fanSpeed)
		if fanSpindown && fanOffMs > 0 {
			fanInfo += fmt.Sprintf(", off in %dms", fanOffMs)
		}
		fmt.Printf("  Fan:       %s\n", fanInfo)
	}

	// Heater
	if heaterMs > 0 {
		heaterSec := float64(heaterMs) / 1000.0
		fmt.Printf("  Heater:    %.1fs total\n", heaterSec)
	}

	// Servos
	fmt.Printf("  Servos:    [%dµs, %dµs, %dµs]\n", servo0, servo1, servo2)

	// Smoke error reasons (bytes 20-21)
	if len(data) >= 22 {
		htrErr := data[20]
		fanErr := data[21]
		if htrErr != 0 || fanErr != 0 {
			fmt.Printf("  ── Smoke Errors ──────────────\n")
			if htrErr != 0 {
				fmt.Printf("  Heater:    %s\n", colorize(colorRed, SmokeErrorReasonName(htrErr)))
			}
			if fanErr != 0 {
				fmt.Printf("  Fan:       %s\n", colorize(colorRed, SmokeErrorReasonName(fanErr)))
			}
		}
	}

	// Overcurrent throttle state (bytes 22-23)
	if len(data) >= 24 {
		htrDuty := data[22]
		fanDuty := data[23]
		if htrDuty < 255 || fanDuty < 255 {
			fmt.Printf("  ── Overcurrent Throttle ──────\n")
			if htrDuty < 255 {
				pct := int(htrDuty) * 100 / 255
				fmt.Printf("  Heater:    %s\n", colorize(colorYellow, fmt.Sprintf("throttled to %d%% (duty %d/255)", pct, htrDuty)))
			}
			if fanDuty < 255 {
				pct := int(fanDuty) * 100 / 255
				fmt.Printf("  Fan:       %s\n", colorize(colorYellow, fmt.Sprintf("throttled to %d%% (duty %d/255)", pct, fanDuty)))
			}
		}
	}

	// Battery (bytes 24-27)
	if len(data) >= 28 {
		batteryMV := ReadU16LE(data, 24)
		cellCount := data[26]
		batteryPct := data[27]

		if batteryMV > 0 {
			batteryV := float64(batteryMV) / 1000.0
			battParts := []string{fmt.Sprintf("%.2fV (%dmV)", batteryV, batteryMV)}
			if cellCount > 0 {
				battParts = append(battParts, fmt.Sprintf("%dS", cellCount))
			}
			if batteryPct > 0 {
				pctColor := colorGreen
				if batteryPct <= 10 {
					pctColor = colorRed
				} else if batteryPct <= 30 {
					pctColor = colorYellow
				}
				battParts = append(battParts, fmt.Sprintf("%s%d%%%s", pctColor, batteryPct, colorReset))
			}
			fmt.Printf("  Battery:   %s\n", strings.Join(battParts, ", "))
		} else {
			fmt.Printf("  Battery:   %s\n", colorize(colorYellow, "not detected"))
		}
	}
}
