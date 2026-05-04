// Virtual board — shared LED event evaluator.
//
// Re-implementation of the firmware's event state machine (see
// controllers/lib/sfx_peripherals/led/led_events.h) used by both the
// LightFX virtual board and the HubFX hub-side LED channels. For
// firmware-level fidelity tests run against the real C++ code, use
// tests/lightfx_sim/.

package shared

import "math"

// LED event type IDs. Mirror LightFxEventType.* / HubFxLedEventType.*.
const (
	EvtOn      = 0x00
	EvtOff     = 0x01
	EvtFlash   = 0x02
	EvtFadeIn  = 0x03
	EvtFadeOut = 0x04
	EvtFading  = 0x05
	EvtBeacon  = 0x06
)

// SeqEvent is the wire-format-flat representation of a sequence event.
type SeqEvent struct {
	Type byte
	P1   uint16
	P2   uint16
	P3   uint8
	P4   uint8
	P5   uint8
}

// EvaluateEvent returns (brightness 0..100, complete) for an event after
// `elapsedMs` milliseconds since (re)start.
func EvaluateEvent(e SeqEvent, elapsedMs uint32) (uint8, bool) {
	switch e.Type {
	case EvtOn:
		dur := uint32(e.P1)
		bright := e.P3
		if bright == 0 {
			bright = 100
		}
		if dur > 0 && elapsedMs >= dur {
			return 0, true
		}
		return bright, false

	case EvtOff:
		dur := uint32(e.P1)
		if dur > 0 && elapsedMs >= dur {
			return 0, true
		}
		return 0, false

	case EvtFlash:
		interval := uint32(e.P1)
		duration := uint32(e.P2)
		bright := e.P3
		if bright == 0 {
			bright = 100
		}
		duty := uint32(e.P4)
		if duty == 0 {
			duty = 50
		}
		if interval == 0 {
			return 0, true
		}
		if duration > 0 && elapsedMs >= duration {
			return 0, true
		}
		full := interval * 2
		cycle := elapsedMs % full
		on := full * duty / 100
		if cycle < on {
			return bright, false
		}
		return 0, false

	case EvtFadeIn:
		dur := uint32(e.P1)
		target := e.P3
		if target == 0 {
			target = 100
		}
		if dur == 0 {
			return target, true
		}
		if elapsedMs >= dur {
			return target, true
		}
		return uint8(uint32(target) * elapsedMs / dur), false

	case EvtFadeOut:
		dur := uint32(e.P1)
		start := e.P3
		if start == 0 {
			start = 100
		}
		if dur == 0 {
			return 0, true
		}
		if elapsedMs >= dur {
			return 0, true
		}
		return uint8(uint32(start) - uint32(start)*elapsedMs/dur), false

	case EvtFading:
		cycle := uint32(e.P1)
		duration := uint32(e.P2)
		minB := uint32(e.P3)
		maxB := uint32(e.P4)
		if maxB == 0 {
			maxB = 100
		}
		if cycle == 0 {
			return uint8(minB), true
		}
		if duration > 0 && elapsedMs >= duration {
			return 0, true
		}
		phase := float64(elapsedMs%cycle) / float64(cycle) * 2 * math.Pi
		norm := (1 - math.Cos(phase)) / 2
		return uint8(minB + uint32(norm*float64(maxB-minB))), false

	case EvtBeacon:
		cycle := uint32(e.P1)
		duration := uint32(e.P2)
		flashPct := uint32(e.P3)
		if flashPct == 0 {
			flashPct = 15
		}
		if flashPct > 50 {
			flashPct = 50
		}
		maxB := uint32(e.P4)
		minB := uint32(e.P5)
		if maxB == 0 {
			maxB = 100
		}
		if cycle == 0 {
			return uint8(minB), true
		}
		if duration > 0 && elapsedMs >= duration {
			return 0, true
		}
		pos := elapsedMs % cycle
		flashWin := cycle * flashPct / 100
		if flashWin == 0 {
			return uint8(minB), false
		}
		if pos >= flashWin {
			return uint8(minB), false
		}
		phase := float64(pos) / float64(flashWin) * 2 * math.Pi
		norm := (1 - math.Cos(phase)) / 2
		return uint8(minB + uint32(norm*float64(maxB-minB))), false
	}
	return 0, true
}
