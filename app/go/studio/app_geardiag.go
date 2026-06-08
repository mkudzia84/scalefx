package main

// Gearcontrol (generic-expander) diagnostics bindings — structured Wails
// methods backing the GearDiagnosticsTab.  They drive the CONNECTED board's
// role layer DIRECTLY (ROLE_ATTACH/DETACH/LIST + per-role drive/inspect), no
// hub / GUID hop — the same low-level surface the CLI exposes as
// role-attach-local / bimotor-* / servo-* commands.  Used to bring up and
// bench-test an expander (servo travel, gear-motor seek, stall current)
// before a master owns it.  Gated in the UI to controllerType === 'gearcontrol'.
//
// The move/seek/calibrate calls BLOCK until the motor reports its outcome
// (BIMOTOR_ENDSTOP_RESULT async, awaited via a per-call async filter) so the
// frontend gets a rich result — reached/timeout/aborted + travel time + peak
// current — straight from `await`, with no event-stream plumbing.

import (
	"fmt"
	"time"

	"scalefx/client"
	"scalefx/protocol"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
)

// DiagRole is one attached-role row for the diagnostics table.
type DiagRole struct {
	PortKind     byte   `json:"portKind"`
	PortIdx      byte   `json:"portIdx"`
	RoleKind     byte   `json:"roleKind"`
	PortKindName string `json:"portKindName"`
	RoleKindName string `json:"roleKindName"`
	Flags        byte   `json:"flags"`
}

// DiagBiMotorStatus mirrors roles.BiMotorStatus for the frontend.
type DiagBiMotorStatus struct {
	Index      byte   `json:"index"`
	SignedDuty int16  `json:"signedDuty"`
	VoltageMv  int16  `json:"voltageMv"`
	CurrentMa  int16  `json:"currentMa"`
	Stalled    bool   `json:"stalled"`
	Position   byte   `json:"position"`
	PositionNm string `json:"positionName"`
	GuardMode  byte   `json:"guardMode"`
	GuardNm    string `json:"guardName"`
}

// DiagEndstopResult is the awaited outcome of a move/seek (reached/timeout/
// aborted + travel time + peak current).
type DiagEndstopResult struct {
	Outcome     byte   `json:"outcome"`
	OutcomeName string `json:"outcomeName"`
	TravelMs    uint16 `json:"travelMs"`
	PeakMa      uint16 `json:"peakMa"`
	Position    byte   `json:"position"`
	PositionNm  string `json:"positionName"`
}

// DiagCalibration is a two-leg sweep (drive to A, then to B) — travel time
// + peak current for each leg.  Leg B's travel is the full A→B stroke.
type DiagCalibration struct {
	LegA DiagEndstopResult `json:"legA"`
	LegB DiagEndstopResult `json:"legB"`
}

// DiagServoProfile mirrors the servo motion profile for the frontend.
type DiagServoProfile struct {
	MinUs    uint16 `json:"minUs"`
	MaxUs    uint16 `json:"maxUs"`
	CenterUs uint16 `json:"centerUs"`
	Reversed bool   `json:"reversed"`
	MaxSpeed uint16 `json:"maxSpeed"`
	MaxAccel uint16 `json:"maxAccel"`
	MaxJerk  uint16 `json:"maxJerk"`
}

// DiagInit activates the connected expander (INIT, slave mode).  A directly-
// connected expander needs INIT before its role layer accepts attach.
func (a *App) DiagInit() error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Hub.InitMode(0 /*slave*/, 0)
}

// DiagRoleList returns the roles currently attached on the connected board.
func (a *App) DiagRoleList() ([]DiagRole, error) {
	c := a.snapshotClient()
	if c == nil {
		return nil, fmt.Errorf("not connected")
	}
	entries, err := c.Roles.List()
	if err != nil {
		return nil, err
	}
	out := make([]DiagRole, len(entries))
	for i, e := range entries {
		out[i] = DiagRole{
			PortKind:     e.PortKind,
			PortIdx:      e.PortIdx,
			RoleKind:     e.RoleKind,
			PortKindName: ports.KindName(e.PortKind),
			RoleKindName: roles.KindName(e.RoleKind),
			Flags:        e.Flags,
		}
	}
	return out, nil
}

// DiagRoleAttach binds roleKind to (portKind, portIdx) with default config.
func (a *App) DiagRoleAttach(portKind, portIdx, roleKind int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Roles.Attach(byte(portKind), byte(portIdx), byte(roleKind), nil)
}

// DiagRoleDetach removes the role on (portKind, portIdx).
func (a *App) DiagRoleDetach(portKind, portIdx int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Roles.Detach(byte(portKind), byte(portIdx))
}

// DiagBiMotorStatus reads verbose BiDcMotor status for hbridge[portIdx].
func (a *App) DiagBiMotorStatus(portIdx int) (DiagBiMotorStatus, error) {
	c := a.snapshotClient()
	if c == nil {
		return DiagBiMotorStatus{}, fmt.Errorf("not connected")
	}
	st, err := c.Role("").BiMotorGetStatus(byte(portIdx))
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
	}, nil
}

// DiagBiMotorMoveEnd drives to logical end "a" (+duty) or "b" (-duty) and
// BLOCKS until the endstop result lands (or timeout).
func (a *App) DiagBiMotorMoveEnd(portIdx int, end string, duty, timeoutMs int) (DiagEndstopResult, error) {
	c := a.snapshotClient()
	if c == nil {
		return DiagEndstopResult{}, fmt.Errorf("not connected")
	}
	pos, signed, err := endToSigned(end, duty)
	if err != nil {
		return DiagEndstopResult{}, err
	}
	return a.awaitEndstop(c, byte(portIdx), timeoutMs, func() error {
		return c.Role("").BiMotorMoveToEnd(byte(portIdx), pos, signed, uint16(timeoutMs))
	})
}

// DiagBiMotorSeek drives at signedDuty until a stall/endstop or timeout and
// BLOCKS for the result (position-agnostic — doesn't label which end).
func (a *App) DiagBiMotorSeek(portIdx, signedDuty, timeoutMs int) (DiagEndstopResult, error) {
	c := a.snapshotClient()
	if c == nil {
		return DiagEndstopResult{}, fmt.Errorf("not connected")
	}
	return a.awaitEndstop(c, byte(portIdx), timeoutMs, func() error {
		return c.Role("").BiMotorSeekEndstop(byte(portIdx), int16(signedDuty), uint16(timeoutMs))
	})
}

// DiagBiMotorCalibrate sweeps to end A then end B (each with its own timeout),
// returning travel time + peak current per leg.  Leg B's travel is the full
// A→B stroke — the calibration datum.
func (a *App) DiagBiMotorCalibrate(portIdx, duty, timeoutMs int) (DiagCalibration, error) {
	c := a.snapshotClient()
	if c == nil {
		return DiagCalibration{}, fmt.Errorf("not connected")
	}
	posA, sigA, _ := endToSigned("a", duty)
	legA, err := a.awaitEndstop(c, byte(portIdx), timeoutMs, func() error {
		return c.Role("").BiMotorMoveToEnd(byte(portIdx), posA, sigA, uint16(timeoutMs))
	})
	if err != nil {
		return DiagCalibration{}, fmt.Errorf("leg A: %w", err)
	}
	posB, sigB, _ := endToSigned("b", duty)
	legB, err := a.awaitEndstop(c, byte(portIdx), timeoutMs, func() error {
		return c.Role("").BiMotorMoveToEnd(byte(portIdx), posB, sigB, uint16(timeoutMs))
	})
	if err != nil {
		return DiagCalibration{}, fmt.Errorf("leg B: %w", err)
	}
	return DiagCalibration{LegA: legA, LegB: legB}, nil
}

// DiagBiMotorGuardFixed retunes the stall guard to Fixed mode (|I| >= threshold
// sustained for window).  Use when you know the motor's stall current.
func (a *App) DiagBiMotorGuardFixed(portIdx, thresholdMa, windowMs int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Role("").BiMotorSetGuardFixed(uint16(portIdx), uint16(thresholdMa), uint16(windowMs))
}

// DiagBiMotorGuardLiveRatio retunes the stall guard to LiveRatio mode — averages
// running current per stroke, then trips on a ratio spike.  Recommended for a
// motor whose stall current isn't known and varies with battery voltage.
func (a *App) DiagBiMotorGuardLiveRatio(portIdx, ratioX100, runSampleMs, inrushBlankMs, windowMs, maxTravelMs, absMaxMa int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Role("").BiMotorSetGuardLiveRatio(uint16(portIdx), uint16(ratioX100),
		uint16(runSampleMs), uint16(inrushBlankMs), uint16(windowMs), uint16(maxTravelMs), uint16(absMaxMa))
}

// DiagBiMotorStop hard-brakes the motor (aborts an in-flight seek).
func (a *App) DiagBiMotorStop(portIdx int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Role("").BiMotorBrake(byte(portIdx))
}

// DiagBiMotorJog drives the motor at a raw signed duty (NO stall guard) for
// manual jogging; 0 brakes.
func (a *App) DiagBiMotorJog(portIdx, signed int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Role("").BiMotorSetSigned(byte(portIdx), int16(signed))
}

// DiagServoProfileGet reads servo[portIdx]'s motion profile.
func (a *App) DiagServoProfileGet(portIdx int) (DiagServoProfile, error) {
	c := a.snapshotClient()
	if c == nil {
		return DiagServoProfile{}, fmt.Errorf("not connected")
	}
	p, err := c.Role("").ServoGetProfile(byte(portIdx))
	if err != nil {
		return DiagServoProfile{}, err
	}
	return DiagServoProfile{
		MinUs:    p.MinUs,
		MaxUs:    p.MaxUs,
		CenterUs: p.CenterUs,
		Reversed: p.Reversed,
		MaxSpeed: p.MaxSpeedUsPerSec,
		MaxAccel: p.MaxAccelUsPerSec2,
		MaxJerk:  p.MaxJerkUsPerSec3,
	}, nil
}

// DiagServoSetTarget commands a servo target position in microseconds.
func (a *App) DiagServoSetTarget(portIdx, us int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Role("").ServoSetTarget(byte(portIdx), uint16(us))
}

// ─── helpers ─────────────────────────────────────────────────────────

// endToSigned maps a logical end ("a"/"b") + magnitude to (position, signed
// duty): A = +duty, B = −duty.
func endToSigned(end string, duty int) (roles.BiMotorPosition, int16, error) {
	if duty < 0 {
		duty = -duty
	}
	switch end {
	case "a", "A":
		return roles.BiMotorPosA, int16(duty), nil
	case "b", "B":
		return roles.BiMotorPosB, int16(-duty), nil
	}
	return roles.BiMotorPosUnknown, 0, fmt.Errorf("end must be 'a' or 'b'")
}

// awaitEndstop registers a one-shot async filter for BIMOTOR_ENDSTOP_RESULT,
// fires the move (send), and blocks until the matching motor's result lands
// or the deadline (move timeout + slack) elapses.
func (a *App) awaitEndstop(c *client.Client, portIdx byte, timeoutMs int, send func() error) (DiagEndstopResult, error) {
	conn := c.Conn()
	if conn == nil {
		return DiagEndstopResult{}, fmt.Errorf("not connected")
	}
	ch := make(chan *protocol.Response, 4)
	conn.RegisterAsyncFilter(roles.BiMotorEndstopResult, ch)
	defer conn.UnregisterAsyncFilter(roles.BiMotorEndstopResult)

	if err := send(); err != nil {
		return DiagEndstopResult{}, err
	}
	deadline := time.After(time.Duration(timeoutMs+3000) * time.Millisecond)
	for {
		select {
		case resp := <-ch:
			ev, err := roles.DecodeBiMotorEndstopEvent(resp.Payload)
			if err != nil || ev.Index != portIdx {
				continue // not our motor / malformed — keep waiting
			}
			return DiagEndstopResult{
				Outcome:     byte(ev.Outcome),
				OutcomeName: ev.Outcome.String(),
				TravelMs:    ev.TravelMs,
				PeakMa:      ev.PeakMa,
				Position:    byte(ev.Position),
				PositionNm:  ev.Position.String(),
			}, nil
		case <-deadline:
			return DiagEndstopResult{}, fmt.Errorf("no endstop result within %d ms", timeoutMs+3000)
		}
	}
}
