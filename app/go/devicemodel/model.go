package devicemodel

import (
	"sort"

	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
	"scalefx/protocol/topology"
)

// Claim binds one port to one (domain, slot).  The same INPUT port may
// appear in several claims (different domains); an OUTPUT port may appear
// at most once across the whole model.
type Claim struct {
	Domain DomainID `json:"domain"`
	Slot   string   `json:"slot"`
	Port   PortRef  `json:"port"`
}

// Model is the working device model — a flat list of ports (with their
// attached roles) plus the current set of domain claims.  It is a value
// the host owns and mutates; nothing here touches the wire.
type Model struct {
	Ports  []Port  `json:"ports"`
	Claims []Claim `json:"claims"`
}

// BuildModel assembles a Model from a topology snapshot.  `boardPorts`
// comes from Topology.PortList(""), `boardRoles` from RoleList("").
// Attached roles are matched onto ports by (guid, kind, index); ports
// with no matching role come back free (roles.KindNone).
//
// `hubGUID` is the master's OWN identity GUID (from INIT_READY). Hub-local
// is canonically the EMPTY guid (instructions/31): newer firmware already
// emits "" for its own port/role blocks, but a board GUID equal to `hubGUID`
// (older firmware, or a stray persisted ref) is folded to "" here so the
// model carries exactly ONE hub-local form. Nothing downstream — keys,
// routing, the Wails DTO — ever sees the hub's GUID as a port address.
func BuildModel(boardPorts []topology.BoardPorts, boardRoles []topology.BoardRoles, hubGUID string) *Model {
	// Fold the hub's own identity GUID → "" (the canonical hub-local form).
	canon := func(guid string) string {
		if hubGUID != "" && guid == hubGUID {
			return ""
		}
		return guid
	}
	// Index attached roles by port for O(1) lookup.
	roleAt := map[PortRef]byte{}
	for _, b := range boardRoles {
		for _, r := range b.Roles {
			roleAt[PortRef{GUID: canon(b.GUID), Kind: r.PortKind, Index: r.PortIdx}] = r.RoleKind
		}
	}

	m := &Model{}
	add := func(guid, name string, kind byte, ds []ports.PortDescriptor) {
		guid = canon(guid)
		for _, d := range ds {
			ref := PortRef{GUID: guid, Kind: kind, Index: d.Index}
			rk := roleAt[ref] // zero value == KindNone == free
			m.Ports = append(m.Ports, Port{
				Ref:          ref,
				BoardName:    name,
				KindName:     ports.KindName(kind),
				Direction:    KindDirection(kind),
				Flags:        d.Flags,
				VoltageMv:    d.VoltageMv,
				Caps:         portCaps(kind, d.Flags, d.VoltageMv),
				RoleKind:     rk,
				RoleName:     roles.KindName(rk),
				HardwareName: HardwareLabel(kind, d.Index),
				AllowedRoles: AllowedRoleOptions(kind),
			})
		}
	}
	for _, b := range boardPorts {
		add(b.GUID, b.DeviceName, ports.KindServo, b.Ports.Servos)
		add(b.GUID, b.DeviceName, ports.KindPwm, b.Ports.Pwms)
		add(b.GUID, b.DeviceName, ports.KindHBridge, b.Ports.HBridges)
		add(b.GUID, b.DeviceName, ports.KindInput, b.Ports.Inputs)
	}
	sort.SliceStable(m.Ports, func(i, j int) bool {
		a, b := m.Ports[i].Ref, m.Ports[j].Ref
		if a.GUID != b.GUID {
			return a.GUID < b.GUID
		}
		if a.Kind != b.Kind {
			return a.Kind < b.Kind
		}
		return a.Index < b.Index
	})
	return m
}

// Port returns the port with the given ref, if present.
func (m *Model) Port(ref PortRef) (Port, bool) {
	for _, p := range m.Ports {
		if p.Ref == ref {
			return p, true
		}
	}
	return Port{}, false
}

// ─── Claim mutations ──────────────────────────────────────────────────

// Claim adds a claim after checking it is legal in the current model.
// Returns an error describing why a claim was rejected; the model is left
// unchanged on error.  Idempotent: re-claiming the same (domain,slot,port)
// is a no-op.
func (m *Model) Claim(c Claim) error {
	if issue := m.checkClaim(c); issue != nil {
		return issue
	}
	for _, ex := range m.Claims {
		if ex == c {
			return nil // already present
		}
	}
	m.Claims = append(m.Claims, c)
	return nil
}

// Unclaim removes a claim (no error if absent).
func (m *Model) Unclaim(c Claim) {
	out := m.Claims[:0]
	for _, ex := range m.Claims {
		if ex != c {
			out = append(out, ex)
		}
	}
	m.Claims = out
}

// ClaimsForPort returns every claim that references a port — used to show
// "this input feeds N domains".
func (m *Model) ClaimsForPort(ref PortRef) []Claim {
	var out []Claim
	for _, c := range m.Claims {
		if c.Port == ref {
			out = append(out, c)
		}
	}
	return out
}

// claimsForSlot counts/collects claims in a specific (domain, slot).
func (m *Model) claimsForSlot(id DomainID, slot string) []Claim {
	var out []Claim
	for _, c := range m.Claims {
		if c.Domain == id && c.Slot == slot {
			out = append(out, c)
		}
	}
	return out
}

// (Candidates() — the per-slot picker query — was removed 2026-06-13 with the
// generic Domains tab; picker pools are now computed frontend-side from
// freePortPool + effectClaims.  claimsForSlot + outputClaimedElsewhere below
// stay alive via validate.go's exclusivity check.)

// outputClaimedElsewhere reports whether an output port is already claimed
// by any claim other than the given (domain, slot).
func (m *Model) outputClaimedElsewhere(ref PortRef, id DomainID, slot string) bool {
	for _, c := range m.Claims {
		if c.Port == ref && !(c.Domain == id && c.Slot == slot) {
			return true
		}
	}
	return false
}
