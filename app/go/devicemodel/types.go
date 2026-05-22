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
	// Caps are human-readable capability tokens derived from Flags:
	// input modes (PULSE/SBUS/JETI_EX/UART_RAW), sense channels (V/I/T),
	// or EMITS for a servo port that can also read its pulse back.
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
		return "RC PWM Input"
	case roles.KindSbusInput:
		return "SBUS Input"
	case roles.KindJetiExInput:
		return "Jeti EX Input"
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
		return []byte{roles.KindRcPwmInput, roles.KindSbusInput, roles.KindJetiExInput}
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

// portCaps derives capability tokens from a port's kind + flags.
func portCaps(kind, flags byte) []string {
	switch kind {
	case ports.KindInput:
		return ports.InputFlagsNames(flags)
	case ports.KindPwm, ports.KindHBridge:
		return ports.SenseFlagsNames(flags)
	case ports.KindServo:
		if flags&ports.ServoFlagEmits != 0 {
			return []string{"EMITS"}
		}
	}
	return []string{}
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

// inputRoleKinds is the set of input role kinds any "trigger"/"switch"
// slot accepts — a physical input port can carry RC PWM, SBUS, or Jeti.
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
		ID:    DomainGun,
		Label: "Gun (GunFX)",
		Cap:   core.CapGunFx,
		Slots: []Slot{
			{Key: "muzzle", Label: "Muzzle flash", RoleKinds: []byte{roles.KindLedAnimator}, Direction: DirOutput, Min: 0, Max: 2, Optional: true},
			{Key: "trigger", Label: "Fire trigger", RoleKinds: inputRoleKinds, Direction: DirInput, Min: 0, Max: 1, Optional: true, Shared: true},
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
