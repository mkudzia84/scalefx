package main

// Gear-motor (BiDcMotor / H-bridge) drive + calibration bindings for the
// GEAR PANEL — the same low-level role surface as app_geardiag.go, but
// addressed by PortRef (guid + index) so it works TRANSPARENTLY whether the
// gear motor is a hub-local port or lives on an expander (Rule 58).  The
// diagnostics tab (app_geardiag.go) hard-codes `Role("")` because it bench-
// tests a directly-connected board; the gear panel owns motors on the hub's
// own /hubfx.yaml ports, so it routes per-GUID.
//
// Reuses the Diag* result structs (DiagBiMotorStatus / DiagEndstopResult /
// DiagCalibration), `endToSigned`, and `awaitEndstop` from app_geardiag.go —
// they're the same package.  Calibration/move calls BLOCK until the motor
// reports BIMOTOR_ENDSTOP_RESULT (awaited via a per-call async filter), so the
// frontend gets travel-time + peak-current straight from `await`.

import "fmt"

// GearMotorStatus reads verbose BiDcMotor status (duty / voltage / CURRENT mA /
// position / stall / guard) for the motor on (guid, hbridge[portIdx]).  The
// current reading is what the gear panel surfaces live next to each strut.
func (a *App) GearMotorStatus(guid string, portIdx int) (DiagBiMotorStatus, error) {
	c := a.snapshotClient()
	if c == nil {
		return DiagBiMotorStatus{}, fmt.Errorf("not connected")
	}
	st, err := c.Role(guid).BiMotorGetStatus(byte(portIdx))
	if err != nil {
		return DiagBiMotorStatus{}, err
	}
	return DiagBiMotorStatus{
		Index:      st.Index,
		SignedDuty: st.SignedDuty,
		VoltageMv:  st.VoltageMv,
		CurrentMa:  st.CurrentMa,
		Stalled:    st.Stalled,
		Position:   byte(st.Position),
		PositionNm: st.Position.String(),
		GuardMode:  byte(st.GuardMode),
		GuardNm:    st.GuardMode.String(),
		ElementMv:  st.ElementMv,
		RailNowMv:  st.RailNowMv,
		CapDuty:    st.CapDuty,
	}, nil
}

// GearMotorMoveEnd drives to logical end "a" (+duty) or "b" (-duty) and BLOCKS
// until the endstop result lands (reached / timeout / aborted + travel + peak
// current).
func (a *App) GearMotorMoveEnd(guid string, portIdx int, end string, duty, timeoutMs int) (DiagEndstopResult, error) {
	c := a.snapshotClient()
	if c == nil {
		return DiagEndstopResult{}, fmt.Errorf("not connected")
	}
	pos, signed, err := endToSigned(end, duty)
	if err != nil {
		return DiagEndstopResult{}, err
	}
	return a.awaitEndstop(c, byte(portIdx), timeoutMs, func() error {
		return c.Role(guid).BiMotorMoveToEnd(byte(portIdx), pos, signed, uint16(timeoutMs))
	})
}

// GearMotorCalibrate sweeps to end A then end B (each with its own timeout),
// returning travel time + peak current per leg.  Leg B's travel is the full
// A→B stroke — the datum the operator turns into a travel timeout.
func (a *App) GearMotorCalibrate(guid string, portIdx, duty, timeoutMs int) (DiagCalibration, error) {
	c := a.snapshotClient()
	if c == nil {
		return DiagCalibration{}, fmt.Errorf("not connected")
	}
	posA, sigA, _ := endToSigned("a", duty)
	legA, err := a.awaitEndstop(c, byte(portIdx), timeoutMs, func() error {
		return c.Role(guid).BiMotorMoveToEnd(byte(portIdx), posA, sigA, uint16(timeoutMs))
	})
	if err != nil {
		return DiagCalibration{}, fmt.Errorf("leg A: %w", err)
	}
	posB, sigB, _ := endToSigned("b", duty)
	legB, err := a.awaitEndstop(c, byte(portIdx), timeoutMs, func() error {
		return c.Role(guid).BiMotorMoveToEnd(byte(portIdx), posB, sigB, uint16(timeoutMs))
	})
	if err != nil {
		return DiagCalibration{}, fmt.Errorf("leg B: %w", err)
	}
	return DiagCalibration{LegA: legA, LegB: legB}, nil
}

// GearMotorJog drives the motor at a raw signed duty (NO stall guard) for
// manual hold-to-jog; 0 brakes.
func (a *App) GearMotorJog(guid string, portIdx, signed int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Role(guid).BiMotorSetSigned(byte(portIdx), int16(signed))
}

// GearMotorStop hard-brakes the motor (aborts an in-flight seek).
func (a *App) GearMotorStop(guid string, portIdx int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Role(guid).BiMotorBrake(byte(portIdx))
}

// GearMotorGuardFixed retunes the stall guard to Fixed mode (|I| >= threshold
// sustained for window).  Live push to the role — not persisted to YAML.
// motorVoltageMv: the rated-voltage duty cap — ≥0 sets it (0 = cap off),
// negative leaves the role's cap untouched (emits the 14-byte SET_GUARD form).
func (a *App) GearMotorGuardFixed(guid string, portIdx, thresholdMa, windowMs, motorVoltageMv int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	if motorVoltageMv >= 0 {
		return c.Role(guid).BiMotorSetGuardFixed(uint16(portIdx), uint16(thresholdMa), uint16(windowMs), uint16(motorVoltageMv))
	}
	return c.Role(guid).BiMotorSetGuardFixed(uint16(portIdx), uint16(thresholdMa), uint16(windowMs))
}

// GearMotorGuardLiveRatio retunes the stall guard to LiveRatio mode (auto-
// baseline running current, trip on a ratio spike).  Recommended when the
// motor's stall current is unknown / varies with battery voltage.  Live push.
// motorVoltageMv semantics as GearMotorGuardFixed.
func (a *App) GearMotorGuardLiveRatio(guid string, portIdx, ratioX100, runSampleMs, inrushBlankMs, windowMs, maxTravelMs, absMaxMa, motorVoltageMv int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	if motorVoltageMv >= 0 {
		return c.Role(guid).BiMotorSetGuardLiveRatio(uint16(portIdx), uint16(ratioX100),
			uint16(runSampleMs), uint16(inrushBlankMs), uint16(windowMs), uint16(maxTravelMs), uint16(absMaxMa), uint16(motorVoltageMv))
	}
	return c.Role(guid).BiMotorSetGuardLiveRatio(uint16(portIdx), uint16(ratioX100),
		uint16(runSampleMs), uint16(inrushBlankMs), uint16(windowMs), uint16(maxTravelMs), uint16(absMaxMa))
}
