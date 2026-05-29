package devicemodel

import (
	"fmt"
	"strings"

	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
)

// Presets are sensible default wirings.  They reference ports SYMBOLICALLY
// (by board selector + kind + index) so one preset applies to any system
// that has the right board, regardless of the board's runtime GUID.  A
// preset declares two things: role attachments to make, and domain claims
// to register on top of them.

// BoardSel selects which board a preset port lives on:
//
//	""            → the hub itself
//	"guid:3C4D"   → an explicit GUID
//	"lightfx"     → the first connected board of that kind (by name prefix)
type BoardSel string

// PresetPortRef is a symbolic port reference inside a preset.
type PresetPortRef struct {
	Board BoardSel `json:"board"`
	Kind  byte     `json:"kind"`
	Index byte     `json:"index"`
}

// RoleAssign attaches a role to a (symbolic) port.
type RoleAssign struct {
	Port     PresetPortRef `json:"port"`
	RoleKind byte          `json:"roleKind"`
}

// PresetClaim claims a (symbolic) port for a domain slot.
type PresetClaim struct {
	Domain DomainID      `json:"domain"`
	Slot   string        `json:"slot"`
	Port   PresetPortRef `json:"port"`
}

// Preset is a named default wiring.
type Preset struct {
	Name        string        `json:"name"`
	Description string        `json:"description"`
	Roles       []RoleAssign  `json:"roles"`
	Claims      []PresetClaim `json:"claims"`
}

// ResolvedAssign is a concrete role attachment the host must push to the
// wire (ROLE_ATTACH) after a preset resolves.
type ResolvedAssign struct {
	Port     PortRef `json:"port"`
	RoleKind byte    `json:"roleKind"`
	RoleName string  `json:"roleName"`
}

// presetCatalog is the built-in preset library.  Hosts may append their
// own at runtime; this is the shipped baseline.
var presetCatalog = []Preset{
	{
		Name:        "Hub: shared landing switch",
		Description: "Wire input IN_1 as a PPM switch channel that drives BOTH the landing lights and the landing gear (a single channel feeding two domains).",
		Roles: []RoleAssign{
			{Port: PresetPortRef{Board: "", Kind: ports.KindInput, Index: 0}, RoleKind: roles.KindRcPwmInput},
		},
		Claims: []PresetClaim{
			{Domain: DomainLandingLights, Slot: "trigger", Port: PresetPortRef{Board: "", Kind: ports.KindInput, Index: 0}},
			{Domain: DomainLandingGear, Slot: "trigger", Port: PresetPortRef{Board: "", Kind: ports.KindInput, Index: 0}},
		},
	},
	{
		Name:        "LightFX: 4-channel landing lights",
		Description: "Attach LED animators to the first four LightFX PWM channels and assign them to the landing-lights domain.",
		Roles: []RoleAssign{
			{Port: PresetPortRef{Board: "lightfx", Kind: ports.KindPwm, Index: 0}, RoleKind: roles.KindLedAnimator},
			{Port: PresetPortRef{Board: "lightfx", Kind: ports.KindPwm, Index: 1}, RoleKind: roles.KindLedAnimator},
			{Port: PresetPortRef{Board: "lightfx", Kind: ports.KindPwm, Index: 2}, RoleKind: roles.KindLedAnimator},
			{Port: PresetPortRef{Board: "lightfx", Kind: ports.KindPwm, Index: 3}, RoleKind: roles.KindLedAnimator},
		},
		Claims: []PresetClaim{
			{Domain: DomainLandingLights, Slot: "leds", Port: PresetPortRef{Board: "lightfx", Kind: ports.KindPwm, Index: 0}},
			{Domain: DomainLandingLights, Slot: "leds", Port: PresetPortRef{Board: "lightfx", Kind: ports.KindPwm, Index: 1}},
			{Domain: DomainLandingLights, Slot: "leds", Port: PresetPortRef{Board: "lightfx", Kind: ports.KindPwm, Index: 2}},
			{Domain: DomainLandingLights, Slot: "leds", Port: PresetPortRef{Board: "lightfx", Kind: ports.KindPwm, Index: 3}},
		},
	},
}

// Presets returns the built-in preset catalog.
func Presets() []Preset { return presetCatalog }

// PresetByName looks up a preset.
func PresetByName(name string) (Preset, bool) {
	for _, p := range presetCatalog {
		if p.Name == name {
			return p, true
		}
	}
	return Preset{}, false
}

// boardKindPrefixes maps a board-kind selector to the DeviceName prefix
// the firmware emits (see board_server.cpp buildDeviceName).
var boardKindPrefixes = map[string]string{
	"hubfx":       "HubFx",
	"lightfx":     "LightFx",
	"gunfx":       "GunFx",
	"gearcontrol": "GearControl",
	"noop":        "NoOp",
}

// resolveBoard maps a BoardSel to a concrete GUID against the model.
// Returns (guid, true) on success.  An empty selector resolves to the hub
// ("").  A "guid:" selector is taken verbatim.  A board-kind selector
// resolves to the first port whose board name carries the kind's prefix.
func (m *Model) resolveBoard(sel BoardSel) (string, bool) {
	s := string(sel)
	if s == "" {
		return "", true
	}
	if strings.HasPrefix(s, "guid:") {
		return strings.TrimPrefix(s, "guid:"), true
	}
	prefix, ok := boardKindPrefixes[s]
	if !ok {
		prefix = s // allow a raw prefix
	}
	for _, p := range m.Ports {
		if strings.HasPrefix(p.BoardName, prefix) {
			return p.Ref.GUID, true
		}
	}
	return "", false
}

// resolvePort turns a symbolic preset port into a concrete PortRef.
func (m *Model) resolvePort(pr PresetPortRef) (PortRef, error) {
	guid, ok := m.resolveBoard(pr.Board)
	if !ok {
		return PortRef{}, fmt.Errorf("no connected board matches selector %q", pr.Board)
	}
	ref := PortRef{GUID: guid, Kind: pr.Kind, Index: pr.Index}
	if _, ok := m.Port(ref); !ok {
		return PortRef{}, fmt.Errorf("board %q has no %s port %d", pr.Board, ports.KindName(pr.Kind), pr.Index)
	}
	return ref, nil
}

// ApplyPreset resolves a preset against the current topology, then:
//
//   - mutates the model so each named port carries the preset's role
//     (optimistic — the host must push the returned ResolvedAssigns as
//     ROLE_ATTACH commands to make it real on the wire);
//   - adds the preset's claims.
//
// It returns the role attachments to push and a slice of non-fatal
// resolution warnings (ports/boards the preset referenced that aren't
// present — skipped, not failed, so a partial system still gets the parts
// it can host).  A hard error is returned only for an internally invalid
// preset (claim that can't validate even after its role is applied).
func (m *Model) ApplyPreset(p Preset) (assigns []ResolvedAssign, warnings []string, err error) {
	// 1. Resolve + apply role assignments.
	for _, ra := range p.Roles {
		ref, rerr := m.resolvePort(ra.Port)
		if rerr != nil {
			warnings = append(warnings, fmt.Sprintf("role %s: %v (skipped)", roles.KindName(ra.RoleKind), rerr))
			continue
		}
		m.SetRole(ref, ra.RoleKind)
		assigns = append(assigns, ResolvedAssign{Port: ref, RoleKind: ra.RoleKind, RoleName: roles.KindName(ra.RoleKind)})
	}
	// 2. Resolve + add claims (now that roles are in place).
	for _, pc := range p.Claims {
		ref, rerr := m.resolvePort(pc.Port)
		if rerr != nil {
			warnings = append(warnings, fmt.Sprintf("claim %s/%s: %v (skipped)", pc.Domain, pc.Slot, rerr))
			continue
		}
		if cerr := m.Claim(Claim{Domain: pc.Domain, Slot: pc.Slot, Port: ref}); cerr != nil {
			return assigns, warnings, fmt.Errorf("preset %q: %w", p.Name, cerr)
		}
	}
	return assigns, warnings, nil
}

// SetRole updates the in-memory role attached to a port.  Hosts call this
// to reflect a successful ROLE_ATTACH optimistically (so dependent claims
// validate before the next topology refresh).
func (m *Model) SetRole(ref PortRef, roleKind byte) {
	for i := range m.Ports {
		if m.Ports[i].Ref == ref {
			m.Ports[i].RoleKind = roleKind
			m.Ports[i].RoleName = roles.KindName(roleKind)
			return
		}
	}
}
