package engine

// ScaleFX Engine - GunFX Response Parsers

import (
	"fmt"
	"scalefx/protocol"
	"scalefx/protocol/gunfx"
	"strings"
)

func (e *Engine) parseGunFXStatus(data []byte) {
	if len(data) < 20 {
		e.Out.Printf("  GunFX: (incomplete: %d bytes)\n", len(data))
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
	fanOffMs := protocol.ReadU16LE(data, 2)
	servo0 := protocol.ReadU16LE(data, 4)
	servo1 := protocol.ReadU16LE(data, 6)
	servo2 := protocol.ReadU16LE(data, 8)
	rpm := protocol.ReadU16LE(data, 10)
	shots := protocol.ReadU32LE(data, 12)
	heaterMs := protocol.ReadU32LE(data, 16)

	var stateParts []string
	if firing {
		stateParts = append(stateParts, e.Out.C(ColorRed, "FIRING"))
	}
	if flashActive {
		stateParts = append(stateParts, "FLASH")
	}
	if flashFading {
		stateParts = append(stateParts, "FADING")
	}
	if heaterOn {
		stateParts = append(stateParts, e.Out.C(ColorYellow, "HEATER"))
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

	e.Out.Printf("  ── GunFX ──────────────────────\n")
	e.Out.Printf("  State:     %s\n", stateStr)

	if firing {
		e.Out.Printf("  Fire rate: %d RPM\n", rpm)
	}
	e.Out.Printf("  Shots:     %d\n", shots)

	if fanOn || fanSpindown {
		fanInfo := fmt.Sprintf("speed=%d", fanSpeed)
		if fanSpindown && fanOffMs > 0 {
			fanInfo += fmt.Sprintf(", off in %dms", fanOffMs)
		}
		e.Out.Printf("  Fan:       %s\n", fanInfo)
	}

	if heaterMs > 0 {
		heaterSec := float64(heaterMs) / 1000.0
		e.Out.Printf("  Heater:    %.1fs total\n", heaterSec)
	}

	e.Out.Printf("  Servos:    [%dµs, %dµs, %dµs]\n", servo0, servo1, servo2)

	if len(data) >= 22 {
		htrErr := data[20]
		fanErr := data[21]
		if htrErr != 0 || fanErr != 0 {
			e.Out.Printf("  ── Smoke Errors ──────────────\n")
			if htrErr != 0 {
				e.Out.Printf("  Heater:    %s\n", e.Out.C(ColorRed, gunfx.SmokeErrorReasonName(htrErr)))
			}
			if fanErr != 0 {
				e.Out.Printf("  Fan:       %s\n", e.Out.C(ColorRed, gunfx.SmokeErrorReasonName(fanErr)))
			}
		}
	}

	if len(data) >= 24 {
		htrDuty := data[22]
		fanDuty := data[23]
		if htrDuty < 255 || fanDuty < 255 {
			e.Out.Printf("  ── Overcurrent Throttle ──────\n")
			if htrDuty < 255 {
				pct := int(htrDuty) * 100 / 255
				e.Out.Printf("  Heater:    %s\n", e.Out.C(ColorYellow, fmt.Sprintf("throttled to %d%% (duty %d/255)", pct, htrDuty)))
			}
			if fanDuty < 255 {
				pct := int(fanDuty) * 100 / 255
				e.Out.Printf("  Fan:       %s\n", e.Out.C(ColorYellow, fmt.Sprintf("throttled to %d%% (duty %d/255)", pct, fanDuty)))
			}
		}
	}

	if len(data) >= 28 {
		batteryMV := protocol.ReadU16LE(data, 24)
		cellCount := data[26]
		batteryPct := data[27]

		if batteryMV > 0 {
			batteryV := float64(batteryMV) / 1000.0
			battParts := []string{fmt.Sprintf("%.2fV (%dmV)", batteryV, batteryMV)}
			if cellCount > 0 {
				battParts = append(battParts, fmt.Sprintf("%dS", cellCount))
			}
			if batteryPct > 0 {
				pctColor := ColorGreen
				if batteryPct <= 10 {
					pctColor = ColorRed
				} else if batteryPct <= 30 {
					pctColor = ColorYellow
				}
				battParts = append(battParts, e.Out.C(pctColor, fmt.Sprintf("%d%%", batteryPct)))
			}
			e.Out.Printf("  Battery:   %s\n", strings.Join(battParts, ", "))
		} else {
			e.Out.Printf("  Battery:   %s\n", e.Out.C(ColorYellow, "not detected"))
		}
	}
}
