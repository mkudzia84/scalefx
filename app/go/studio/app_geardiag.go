package main

// Gearcontrol (generic-expander) diagnostics bindings — structured Wails
// methods backing the GearDiagnosticsTab.  They drive the CONNECTED board's
// role layer DIRECTLY (ROLE_ATTACH/DETACH/LIST + per-role drive/inspect), no
// hub / GUID hop — the same low-level surface the CLI exposes as
// role-attach-local / bimotor-* / servo-profile-*.  Used to bring up and
// bench-test an expander (servo travel, gear-motor seek, stall current)
// before a master owns it.  Gated in the UI to controllerType === 'gearcontrol'.

import (
	"fmt"

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
}

// DiagServoProfile mirrors the servo motion profile for the frontend.
type DiagServoProfile struct {
	MinUs       uint16 `json:"minUs"`
	MaxUs       uint16 `json:"maxUs"`
	CenterUs    uint16 `json:"centerUs"`
	Reversed    bool   `json:"reversed"`
	MaxSpeed    uint16 `json:"maxSpeed"`
	MaxAccel    uint16 `json:"maxAccel"`
	MaxJerk     uint16 `json:"maxJerk"`
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
	st, err := c.Roles.BiMotorGetStatus(byte(portIdx))
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
		PositionNm: biMotorPosName(st.Position),
		GuardMode:  byte(st.GuardMode),
	}, nil
}

// DiagBiMotorSeek drives the BiDcMotor at signedDuty until stall/timeout.
func (a *App) DiagBiMotorSeek(portIdx, signedDuty, timeoutMs int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Roles.BiMotorSeekEndstop(byte(portIdx), int16(signedDuty), uint16(timeoutMs))
}

// DiagBiMotorMoveEnd drives to logical end "a" (+duty) or "b" (-duty).
func (a *App) DiagBiMotorMoveEnd(portIdx int, end string, duty, timeoutMs int) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	if duty < 0 {
		duty = -duty
	}
	var pos roles.BiMotorPosition
	var signed int16
	switch end {
	case "a", "A":
		pos, signed = roles.BiMotorPosA, int16(duty)
	case "b", "B":
		pos, signed = roles.BiMotorPosB, int16(-duty)
	default:
		return fmt.Errorf("end must be 'a' or 'b'")
	}
	return c.Roles.BiMotorMoveToEnd(byte(portIdx), pos, signed, uint16(timeoutMs))
}

// DiagServoProfileGet reads servo[portIdx]'s motion profile.
func (a *App) DiagServoProfileGet(portIdx int) (DiagServoProfile, error) {
	c := a.snapshotClient()
	if c == nil {
		return DiagServoProfile{}, fmt.Errorf("not connected")
	}
	p, err := c.Roles.ServoGetProfile(byte(portIdx))
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
	return c.Roles.ServoSetTarget(byte(portIdx), uint16(us))
}

// biMotorPosName mirrors the CLI helper (A/B/unknown).
func biMotorPosName(p roles.BiMotorPosition) string {
	switch p {
	case roles.BiMotorPosA:
		return "A"
	case roles.BiMotorPosB:
		return "B"
	}
	return "unknown"
}
