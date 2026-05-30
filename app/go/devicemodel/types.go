// Package devicemodel is the authoritative, UI-agnostic model of a
// ScaleFX system's ports, the roles attached to them, and the functional
// DOMAINS that claim them.  It mirrors no wire packet directly; instead it
// is assembled from the topology snapshot (PORT_LIST + ROLE_LIST) and a
// set of domain claims, then answers the questions Studio's tabs need:
//
//   - which ports exist, what they can do, and what role is attached;
//   - which ports a functional domain may legally select for a slot;
//   - whether the current claim set is valid (role-kind match, exclusive
//     outputs, shared inputs, slot cardinality);
//   - sensible default port mappings (presets).
//
// The package is pure: no serial I/O, no Wails, no globals.  Studio and
// the CLI build a Model from a topology snapshot, mutate claims, validate,
// and translate the result into ROLE_ATTACH / config writes themselves.
//
// Two relationships are distinct and must not be conflated:
//
//   - ROLE attachment is wire truth: one role per port, owned by the
//     firmware (ROLE_LIST_RESP).  A servo port hosts a servo-actuator.
//   - DOMAIN claim is a UI/config concept: a functional area (landing
//     lights, landing gear, …) uses a port.  OUTPUT ports are claimed
//     exclusively; INPUT ports may be shared across many domains (one
//     landing switch feeds both the lights and the gear).
package devicemodel

import (
	"fmt"

	"scalefx/protocol/core"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
)

// ─── Port identity & direction ────────────────────────────────────────

// PortRef uniquely identifies a port across the whole system.  An empty
// GUID means the hub itself; a 4-hex-char GUID names an expander.
type PortRef struct {
	GUID  string `json:"guid"`
	Kind  byte   `json:"kind"` // ports.Kind*
	Index byte   `json:"index"`
}

func (r PortRef) String() string {
	g := r.GUID
	if g == "" {
		g = "hub"
	}
	return fmt.Sprintf("%s/%s%d", g, ports.KindName(r.Kind), r.Index)
}

// Direction classifies a port kind as signal sink (output) or source
// (input).  It drives claim semantics: outputs are exclusive, inputs
// are shareable.
type Direction string

const (
	DirOutput Direction = "output"
	DirInput  Direction = "input"
)

// KindDirection maps a port kind to its fixed direction (Rule 31 — port
// direction is set at declaration, never swapped at runtime).
func KindDirection(kind byte) Direction {
	if kind == ports.KindInput {
		return DirInput
	}
	return DirOutput
}

// ─── Port ─────────────────────────────────────────────────────────────

// Port is one physical port enriched with derived capability tokens and
// the role currently attached to it.
type Port struct {
	Ref       PortRef   `json:"ref"`
	BoardName string    `json:"boardName"`
	KindName  string    `json:"kindName"`
	Direction Direction `json:"direction"`
	Flags     byte      `json:"flags"`
	// VoltageMv is the rail voltage (millivolts) the port is wired to,
	// declared by the board's port descriptor (Phase 0 of GunFX rollout,
	// instructions/22). 0 = unknown. Effects use this to compute
	// voltage-scaled PWM duty for sub-rail elements.
	VoltageMv uint16 `json:"voltageMv"`
	// Caps are human-readable capability tokens derived from Flags +
	// voltage: input modes (PULSE/SBUS/JETI_EX/UART_RAW), sense channels
	// (V/I/T), EMITS for a servo port that reads its pulse back, and
	// VOLTAGE_<N>V (e.g. "VOLTAGE_8V") when voltageMv > 0.
	Caps []string `json:"caps"`
	// RoleKind is the attached role (roles.KindNone when the port is
	// free).  RoleName is its canonical name.
	RoleKind byte   `json:"roleKind"`
	RoleName string `json:"roleName"`
	// HardwareName is the board's silkscreen-style label (SRV1, CH3, IN1).
	HardwareName string `json:"hardwareName"`
	// AllowedRoles is the set of role kinds this port may host, derived
	// from its kind (servo→servo-actuator only, pwm→led/motor/heater,
	// etc.).  Drives the role picker so it only offers valid options.
	AllowedRoles []RoleOption `json:"allowedRoles"`
	// Name is the operator-assigned friendly label (overlay state, not on
	// the wire).  Empty until set.
	Name string `json:"name"`

	// Profile is the servo motion profile attached to this port — Rule
	// 42 storage (/hubfx.yaml ports[]), Rule 44 editing surface (feature
	// panel inline).  Non-nil only when KindName=="servo" and a profile
	// has been loaded / authored.  Frontend feature panels read this to
	// populate the inline ServoProfileEditor.
	Profile *ServoMotionProfile `json:"profile,omitempty"`
}

// ServoMotionProfile mirrors `sfx_core::ServoMotionProfile` field-for-
// field; the JSON shape matches both the wire `ServoProfileDTO` (live
// tune) and the Studio's `ServoMotionProfileDTO` (persisted) so one
// `ServoProfileEditor` component binds against both.
type ServoMotionProfile struct {
	MinUs             uint16 `json:"minUs"             yaml:"min_us"`
	MaxUs             uint16 `json:"maxUs"             yaml:"max_us"`
	CenterUs          uint16 `json:"centerUs"          yaml:"center_us"`
	Reversed          bool   `json:"reversed"          yaml:"reversed"`
	MaxSpeedUsPerSec  uint16 `json:"maxSpeedUsPerSec"  yaml:"max_speed_us_per_sec"`
	MaxAccelUsPerSec2 uint16 `json:"maxAccelUsPerSec2" yaml:"max_accel_us_per_sec2"`
	MaxJerkUsPerSec3  uint16 `json:"maxJerkUsPerSec3"  yaml:"max_jerk_us_per_sec3"`
}

// RoleOption is a (kind, label) pair for a role the UI may offer.
type RoleOption struct {
	Kind  byte   `json:"kind"`
	Label string `json:"label"`
}

// RoleLabel returns a human-readable name for a role kind.
func RoleLabel(kind byte) string {
	switch kind {
	case roles.KindNone:
		return "— none —"
	case roles.KindServoActuator:
		return "Servo"
	case roles.KindRcPwmInput:
		// Labelled "PPM" (not "RC PWM") — this role decodes a PPM/pulse
		// stream (up to 8 channels on one wire).  The wire-name stays
		// rc-pwm-input for back-compat; only the GUI label is PPM.
		return "PPM Input"
	case roles.KindSbusInput:
		return "SBUS Input"
	case roles.KindJetiExInput:
		return "Jeti EX Input"
	case roles.KindJetiExTelemetry:
		return "Jeti EX Telemetry"
	case roles.KindLedAnimator:
		return "LED Animator"
	case roles.KindDcMotor:
		return "DC Motor"
	case roles.KindHeater:
		return "Heater"
	case roles.KindBiDcMotor:
		return "Bi-Dir DC Motor"
	default:
		return roles.KindName(kind)
	}
}

// allowedRoleKinds returns the role kinds a port of `kind` may host.
// Deterministic from the port kind (Rule 31 — direction + capability are
// fixed at declaration), so the host derives it without a wire round-trip.
func allowedRoleKinds(kind byte) []byte {
	switch kind {
	case ports.KindServo:
		return []byte{roles.KindServoActuator}
	case ports.KindPwm:
		return []byte{roles.KindLedAnimator, roles.KindDcMotor, roles.KindHeater}
	case ports.KindHBridge:
		return []byte{roles.KindBiDcMotor, roles.KindDcMotor}
	case ports.KindInput:
		return []byte{roles.KindRcPwmInput, roles.KindSbusInput, roles.KindJetiExInput, roles.KindJetiExTelemetry}
	}
	return nil
}

// AllowedRoleOptions returns the labelled role options for a port kind.
func AllowedRoleOptions(kind byte) []RoleOption {
	ks := allowedRoleKinds(kind)
	out := make([]RoleOption, len(ks))
	for i, k := range ks {
		out[i] = RoleOption{Kind: k, Label: RoleLabel(k)}
	}
	return out
}

// IsServoKind reports whether a port kind hosts only the servo-actuator
// role (so the UI shows a name field instead of a role picker).
func IsServoKind(kind byte) bool { return kind == ports.KindServo }

// HardwareLabel is the board's silkscreen-style label for a port, derived
// from kind + index (servo→SRV1.., pwm→CH1.., hbridge→HB1.., input→IN1..).
// This is what the operator sees printed next to the header — distinct
// from the user-assigned Name.  Deterministic from (kind, count) which the
// firmware already reports, so no per-port wire field is needed.
func HardwareLabel(kind, index byte) string {
	n := int(index) + 1
	switch kind {
	case ports.KindServo:
		return fmt.Sprintf("SRV%d", n)
	case ports.KindPwm:
		return fmt.Sprintf("CH%d", n)
	case ports.KindHBridge:
		return fmt.Sprintf("HB%d", n)
	case ports.KindInput:
		return fmt.Sprintf("IN%d", n)
	}
	return fmt.Sprintf("%s%d", ports.KindName(kind), n)
}

// HasRole reports whether a role is attached.
func (p Port) HasRole() bool { return p.RoleKind != roles.KindNone }

// portCaps derives capability tokens from a port's kind + flags +
// declared rail voltage. Voltage appends `VOLTAGE_<N>V` (e.g.
// "VOLTAGE_8V") to the cap list — informational only, doesn't gate
// candidate selection.
func portCaps(kind, flags byte, voltageMv uint16) []string {
	var out []string
	switch kind {
	case ports.KindInput:
		out = ports.InputFlagsNames(flags)
	case ports.KindPwm, ports.KindHBridge:
		out = ports.SenseFlagsNames(flags)
	case ports.KindServo:
		if flags&ports.ServoFlagEmits != 0 {
			out = []string{"EMITS"}
		}
	}
	if out == nil {
		out = []string{}
	}
	if tok := voltageCapToken(voltageMv); tok != "" {
		out = append(out, tok)
	}
	return out
}

// voltageCapToken formats a rail voltage as a "VOLTAGE_<N>V" /
// "VOLTAGE_3V3" cap token (uppercase, dot replaced with V for fractional
// volts so the token round-trips cleanly into CSS / log filters).
// Returns "" when voltage is unknown (0).
func voltageCapToken(mV uint16) string {
	if mV == 0 {
		return ""
	}
	if mV%1000 == 0 {
		return fmt.Sprintf("VOLTAGE_%dV", mV/1000)
	}
	// Fractional rails: 3.3 V → VOLTAGE_3V3, 1.8 V → VOLTAGE_1V8.
	whole := mV / 1000
	tenths := (mV % 1000) / 100
	return fmt.Sprintf("VOLTAGE_%dV%d", whole, tenths)
}

// ─── Domains & slots ──────────────────────────────────────────────────

// DomainID names a functional area of the system.  Each maps to a Studio
// tab and (where applicable) a CoreCapability bit on the hub.
type DomainID string

const (
	DomainLandingLights DomainID = "landing-lights"
	DomainLandingGear   DomainID = "landing-gear"
	DomainLighting      DomainID = "lighting"
	DomainEngine        DomainID = "engine"
	DomainGun           DomainID = "gun"
)

// Slot is one named requirement within a domain — a need for ports of a
// particular role kind and direction, with a cardinality.
type Slot struct {
	Key       string    `json:"key"`   // stable id, e.g. "leds", "trigger"
	Label     string    `json:"label"` // human label for the tab
	RoleKinds []byte    `json:"roleKinds"`
	Direction Direction `json:"direction"`
	Min       int       `json:"min"`      // minimum claims for a valid domain
	Max       int       `json:"max"`      // 0 = unbounded
	Optional  bool      `json:"optional"` // Min==0 convenience flag for the UI
	Shared    bool      `json:"shared"`   // input slot that may co-claim a shared input
}

// accepts reports whether a role kind satisfies this slot.  An empty
// RoleKinds list accepts any role of the right direction.
func (s Slot) accepts(roleKind byte) bool {
	if len(s.RoleKinds) == 0 {
		return true
	}
	for _, rk := range s.RoleKinds {
		if rk == roleKind {
			return true
		}
	}
	return false
}

// Domain is a functional area: a set of slots plus the hub capability bit
// that gates whether the domain is available at all.
type Domain struct {
	ID    DomainID `json:"id"`
	Label string   `json:"label"`
	// Cap is the hub CoreCapability that must be present for this domain
	// (0 = always available).
	Cap   uint32 `json:"cap"`
	Slots []Slot `json:"slots"`
}

// Slot returns the named slot definition.
func (d Domain) Slot(key string) (Slot, bool) {
	for _, s := range d.Slots {
		if s.Key == key {
			return s, true
		}
	}
	return Slot{}, false
}

// ─── Domain catalog ───────────────────────────────────────────────────

// inputRoleKinds is the set of input role kinds any "trigger"/"switch" slot
// accepts — a physical RC channel source: PWM, SBUS, or Jeti EX.  NOT
// JetiExTelemetry: that's the downstream telemetry pass-thru (IN_2), carries no
// RC channels, so it must never appear as a selectable trigger/channel source.
var inputRoleKinds = []byte{roles.KindRcPwmInput, roles.KindSbusInput, roles.KindJetiExInput}

// domainCatalog is the declarative source of truth for functional
// domains.  Adding a domain (or slot) here is all the wiring a new
// functional tab needs on the model side.
var domainCatalog = []Domain{
	{
		ID:    DomainLandingLights,
		Label: "Landing Lights",
		Cap:   core.CapLandingLights,
		Slots: []Slot{
			{Key: "leds", Label: "Light channels", RoleKinds: []byte{roles.KindLedAnimator}, Direction: DirOutput, Min: 1, Max: 8},
			{Key: "servo", Label: "Bay-door servo", RoleKinds: []byte{roles.KindServoActuator}, Direction: DirOutput, Min: 0, Max: 1, Optional: true},
			{Key: "trigger", Label: "Landing switch", RoleKinds: inputRoleKinds, Direction: DirInput, Min: 0, Max: 1, Optional: true, Shared: true},
		},
	},
	{
		ID:    DomainLandingGear,
		Label: "Landing Gear",
		Cap:   core.CapGearCtrl,
		Slots: []Slot{
			{Key: "gear", Label: "Gear units", RoleKinds: []byte{roles.KindBiDcMotor, roles.KindServoActuator}, Direction: DirOutput, Min: 1, Max: 6},
			{Key: "trigger", Label: "Gear switch", RoleKinds: inputRoleKinds, Direction: DirInput, Min: 0, Max: 1, Optional: true, Shared: true},
		},
	},
	{
		ID:    DomainLighting,
		Label: "Lighting (LightFX)",
		Cap:   core.CapLightFx,
		Slots: []Slot{
			{Key: "leds", Label: "LED channels", RoleKinds: []byte{roles.KindLedAnimator}, Direction: DirOutput, Min: 1, Max: 8},
		},
	},
	{
		ID:    DomainEngine,
		Label: "Engine (EngineFX)",
		Cap:   core.CapEngine,
		Slots: []Slot{
			{Key: "throttle", Label: "Throttle input", RoleKinds: inputRoleKinds, Direction: DirInput, Min: 0, Max: 1, Optional: true, Shared: true},
		},
	},
	{
		// Phase 1 of GunFX rollout (instructions/22) expanded this domain
		// from {muzzle, trigger} to the full 10-slot shape — fire trigger
		// + ROF selector + muzzle flash + recoil + smoke heater + smoke
		// fan + yaw/pitch servo + yaw/pitch input. Every per-gun output
		// slot caps at kMaxGuns=4 so two guns can claim independent ports
		// without exhausting the table. Inputs are Shared (the same RC
		// stream can drive multiple guns / domains simultaneously).
		ID:    DomainGun,
		Label: "Gun (GunFX)",
		Cap:   core.CapGunFx,
		Slots: []Slot{
			{Key: "trigger",     Label: "Fire trigger",   RoleKinds: inputRoleKinds,                  Direction: DirInput,  Min: 0, Max: 4, Optional: true, Shared: true},
			{Key: "rofSelector", Label: "ROF selector",   RoleKinds: inputRoleKinds,                  Direction: DirInput,  Min: 0, Max: 4, Optional: true, Shared: true},
			{Key: "muzzleFlash", Label: "Muzzle flash",   RoleKinds: []byte{roles.KindLedAnimator},   Direction: DirOutput, Min: 0, Max: 4, Optional: true},
			{Key: "recoilServo", Label: "Recoil servo",   RoleKinds: []byte{roles.KindServoActuator}, Direction: DirOutput, Min: 0, Max: 4, Optional: true},
			{Key: "smokeHeater", Label: "Smoke heater",   RoleKinds: []byte{roles.KindHeater, roles.KindDcMotor}, Direction: DirOutput, Min: 0, Max: 4, Optional: true},
			{Key: "smokeFan",    Label: "Smoke fan",      RoleKinds: []byte{roles.KindDcMotor},       Direction: DirOutput, Min: 0, Max: 4, Optional: true},
			{Key: "yawServo",    Label: "Yaw servo",      RoleKinds: []byte{roles.KindServoActuator}, Direction: DirOutput, Min: 0, Max: 4, Optional: true},
			{Key: "pitchServo",  Label: "Pitch servo",    RoleKinds: []byte{roles.KindServoActuator}, Direction: DirOutput, Min: 0, Max: 4, Optional: true},
			{Key: "yawInput",    Label: "Yaw channel",    RoleKinds: inputRoleKinds,                  Direction: DirInput,  Min: 0, Max: 4, Optional: true, Shared: true},
			{Key: "pitchInput",  Label: "Pitch channel",  RoleKinds: inputRoleKinds,                  Direction: DirInput,  Min: 0, Max: 4, Optional: true, Shared: true},
		},
	},
}

// Domains returns the full domain catalog (copy-safe — callers get the
// shared slice; treat as read-only).
func Domains() []Domain { return domainCatalog }

// DomainByID looks up a domain definition.
func DomainByID(id DomainID) (Domain, bool) {
	for _, d := range domainCatalog {
		if d.ID == id {
			return d, true
		}
	}
	return Domain{}, false
}

// AvailableDomains filters the catalog to domains whose capability bit is
// present in the hub's advertised capabilities (0-cap domains always
// pass).
func AvailableDomains(hubCaps uint32) []Domain {
	out := make([]Domain, 0, len(domainCatalog))
	for _, d := range domainCatalog {
		if d.Cap == 0 || core.HasCapability(hubCaps, d.Cap) {
			out = append(out, d)
		}
	}
	return out
}
