package main

// Role-layer live-tune Wails bindings (Phase 4 IO tab port-role
// editor).  Thin wrappers over `client.RoleTarget` so Studio can read/push
// DC-motor / heater element scaling without re-attaching the role (Rule 42).
//
// Targeting (Rule 58): the motor/heater element bindings take a leading `guid`
// ("" = hub; an expander GUID routes through the hub transparently via
// RoleTarget).  So the ⚙ Tune editor works identically on a hub-local PWM port
// and an expander's DC-motor / heater.

import (
	"fmt"

	"scalefx/client"
)

// DTO mirrors — match `client.ServoProfile` / `client.MotorElement` /
// `client.HeaterElement` field-for-field.  Wails generates TS
// interfaces from these for the frontend.

type ServoProfileDTO struct {
	MinUs             uint16 `json:"minUs"`
	MaxUs             uint16 `json:"maxUs"`
	MaxSpeedUsPerSec  uint16 `json:"maxSpeedUsPerSec"`
	Reversed          bool   `json:"reversed"`
	CenterUs          uint16 `json:"centerUs"`
	MaxAccelUsPerSec2 uint16 `json:"maxAccelUsPerSec2"`
	MaxJerkUsPerSec3  uint16 `json:"maxJerkUsPerSec3"`
}

type MotorElementDTO struct {
	ElementMv  uint16 `json:"elementMv"`
	Scaling    byte   `json:"scaling"` // 0 passthrough, 1 linear, 2 quadratic
	PortRailMv uint16 `json:"portRailMv"`
}

type HeaterElementDTO struct {
	ElementMv  uint16 `json:"elementMv"`
	Scaling    byte   `json:"scaling"`
	DrivePct   uint8  `json:"drivePct"`
	HystCx10   int16  `json:"hystCx10"`
	PortRailMv uint16 `json:"portRailMv"`
}

// ─── Servo profile ───────────────────────────────────────────────────

func (a *App) ServoGetProfile(portIdx uint8) (ServoProfileDTO, error) {
	c := a.snapshotClient()
	if c == nil {
		return ServoProfileDTO{}, fmt.Errorf("not connected")
	}
	p, err := c.Roles.ServoGetProfile(portIdx)
	if err != nil {
		return ServoProfileDTO{}, err
	}
	return ServoProfileDTO(p), nil
}

func (a *App) ServoSetProfile(portIdx uint8, p ServoProfileDTO) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Roles.ServoSetProfile(portIdx, client.ServoProfile(p))
}

// ─── DC motor element ────────────────────────────────────────────────

func (a *App) MotorGetElement(guid string, portIdx uint8) (MotorElementDTO, error) {
	c := a.snapshotClient()
	if c == nil {
		return MotorElementDTO{}, fmt.Errorf("not connected")
	}
	cg, _ := a.canonGuid(guid)
	e, err := c.Role(cg).MotorGetElement(portIdx)
	if err != nil {
		return MotorElementDTO{}, err
	}
	return MotorElementDTO{
		ElementMv:  e.ElementMv,
		Scaling:    byte(e.Scaling),
		PortRailMv: e.PortRailMv,
	}, nil
}

func (a *App) MotorSetElement(guid string, portIdx uint8, e MotorElementDTO) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	cg, _ := a.canonGuid(guid)
	return c.Role(cg).MotorSetElement(portIdx, client.MotorElement{
		ElementMv: e.ElementMv,
		Scaling:   client.ElementScaling(e.Scaling),
	})
}

// MotorSetPct — intent-layer "drive at N % of element voltage".
func (a *App) MotorSetPct(guid string, portIdx, pct uint8) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	cg, _ := a.canonGuid(guid)
	return c.Role(cg).MotorSetPct(portIdx, pct)
}

// ─── Heater element ──────────────────────────────────────────────────

func (a *App) HeaterGetElement(guid string, portIdx uint8) (HeaterElementDTO, error) {
	c := a.snapshotClient()
	if c == nil {
		return HeaterElementDTO{}, fmt.Errorf("not connected")
	}
	cg, _ := a.canonGuid(guid)
	e, err := c.Role(cg).HeaterGetElement(portIdx)
	if err != nil {
		return HeaterElementDTO{}, err
	}
	return HeaterElementDTO{
		ElementMv:  e.ElementMv,
		Scaling:    byte(e.Scaling),
		DrivePct:   e.DrivePct,
		HystCx10:   e.HystCx10,
		PortRailMv: e.PortRailMv,
	}, nil
}

func (a *App) HeaterSetElement(guid string, portIdx uint8, e HeaterElementDTO) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	cg, _ := a.canonGuid(guid)
	return c.Role(cg).HeaterSetElement(portIdx, client.HeaterElement{
		ElementMv: e.ElementMv,
		Scaling:   client.ElementScaling(e.Scaling),
		DrivePct:  e.DrivePct,
		HystCx10:  e.HystCx10,
	})
}
