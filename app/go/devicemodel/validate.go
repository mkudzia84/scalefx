package devicemodel

import (
	"fmt"

	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
)

// Severity ranks a validation issue.
type Severity string

const (
	SevError Severity = "error" // gates apply/save
	SevWarn  Severity = "warn"  // surfaced but non-blocking
)

// Issue is one validation finding.  Domain/Slot/Port are zero-valued when
// the issue isn't scoped to one of them.
type Issue struct {
	Severity Severity `json:"severity"`
	Domain   DomainID `json:"domain,omitempty"`
	Slot     string   `json:"slot,omitempty"`
	Port     *PortRef `json:"port,omitempty"`
	Message  string   `json:"message"`
}

func (i Issue) Error() string { return i.Message }

// checkClaim validates a single prospective claim against the current
// model, returning a blocking *Issue or nil.  Used by Model.Claim to
// reject illegal claims at mutation time.
func (m *Model) checkClaim(c Claim) *Issue {
	dom, ok := DomainByID(c.Domain)
	if !ok {
		return &Issue{Severity: SevError, Domain: c.Domain, Message: fmt.Sprintf("unknown domain %q", c.Domain)}
	}
	s, ok := dom.Slot(c.Slot)
	if !ok {
		return &Issue{Severity: SevError, Domain: c.Domain, Slot: c.Slot, Message: fmt.Sprintf("domain %q has no slot %q", c.Domain, c.Slot)}
	}
	p, ok := m.Port(c.Port)
	if !ok {
		ref := c.Port
		return &Issue{Severity: SevError, Domain: c.Domain, Slot: c.Slot, Port: &ref, Message: fmt.Sprintf("port %s not present in topology", c.Port)}
	}
	if p.Direction != s.Direction {
		ref := c.Port
		return &Issue{Severity: SevError, Domain: c.Domain, Slot: c.Slot, Port: &ref,
			Message: fmt.Sprintf("port %s is %s but slot %q needs %s", c.Port, p.Direction, c.Slot, s.Direction)}
	}
	if !s.accepts(p.RoleKind) {
		ref := c.Port
		return &Issue{Severity: SevError, Domain: c.Domain, Slot: c.Slot, Port: &ref,
			Message: fmt.Sprintf("port %s has role %s; slot %q requires %s", c.Port, roles.KindName(p.RoleKind), c.Slot, slotRoleNames(s))}
	}
	// Exclusive-output rule: an output port may be claimed by only one
	// (domain, slot).  Inputs are shareable, so skip the check for them.
	if s.Direction == DirOutput && m.outputClaimedElsewhere(c.Port, c.Domain, c.Slot) {
		ref := c.Port
		other := m.ClaimsForPort(c.Port)[0]
		return &Issue{Severity: SevError, Domain: c.Domain, Slot: c.Slot, Port: &ref,
			Message: fmt.Sprintf("output port %s already claimed by %s/%s", c.Port, other.Domain, other.Slot)}
	}
	// Max cardinality.
	if s.Max > 0 && len(m.claimsForSlot(c.Domain, c.Slot)) >= s.Max {
		return &Issue{Severity: SevError, Domain: c.Domain, Slot: c.Slot,
			Message: fmt.Sprintf("slot %q already holds its maximum of %d port(s)", c.Slot, s.Max)}
	}
	return nil
}

// Validate audits the entire model and returns every issue found:
//
//   - claim references a missing port / wrong direction / wrong role;
//   - an output port claimed by more than one domain (error);
//   - a slot below its Min (warn — incomplete wiring) or above Max (error);
//   - a claimed port with no role attached yet (warn — attach the role on
//     the Port/Role setup tab first).
//
// A model with zero SevError issues is safe to apply / persist.
func (m *Model) Validate() []Issue {
	var issues []Issue

	// Per-claim checks + output exclusivity (collected, not short-circuited).
	outputClaims := map[PortRef][]Claim{}
	for _, c := range m.Claims {
		dom, ok := DomainByID(c.Domain)
		if !ok {
			issues = append(issues, Issue{Severity: SevError, Domain: c.Domain, Message: fmt.Sprintf("unknown domain %q", c.Domain)})
			continue
		}
		s, ok := dom.Slot(c.Slot)
		if !ok {
			issues = append(issues, Issue{Severity: SevError, Domain: c.Domain, Slot: c.Slot, Message: fmt.Sprintf("domain %q has no slot %q", c.Domain, c.Slot)})
			continue
		}
		p, ok := m.Port(c.Port)
		if !ok {
			ref := c.Port
			issues = append(issues, Issue{Severity: SevError, Domain: c.Domain, Slot: c.Slot, Port: &ref, Message: fmt.Sprintf("claimed port %s is no longer present", c.Port)})
			continue
		}
		if p.Direction != s.Direction {
			ref := c.Port
			issues = append(issues, Issue{Severity: SevError, Domain: c.Domain, Slot: c.Slot, Port: &ref, Message: fmt.Sprintf("port %s is %s but slot %q needs %s", c.Port, p.Direction, c.Slot, s.Direction)})
		}
		if !s.accepts(p.RoleKind) {
			ref := c.Port
			issues = append(issues, Issue{Severity: SevError, Domain: c.Domain, Slot: c.Slot, Port: &ref, Message: fmt.Sprintf("port %s has role %s; slot %q requires %s", c.Port, roles.KindName(p.RoleKind), c.Slot, slotRoleNames(s))})
		} else if !p.HasRole() {
			ref := c.Port
			issues = append(issues, Issue{Severity: SevWarn, Domain: c.Domain, Slot: c.Slot, Port: &ref, Message: fmt.Sprintf("port %s has no role attached — attach %s before use", c.Port, slotRoleNames(s))})
		}
		if s.Direction == DirOutput {
			outputClaims[c.Port] = append(outputClaims[c.Port], c)
		}
	}
	for ref, cs := range outputClaims {
		if len(cs) > 1 {
			r := ref
			issues = append(issues, Issue{Severity: SevError, Port: &r, Message: fmt.Sprintf("output port %s claimed by %d domains (must be exclusive)", ref, len(cs))})
		}
	}

	// Per-slot cardinality across every domain.
	for _, dom := range domainCatalog {
		for _, s := range dom.Slots {
			n := len(m.claimsForSlot(dom.ID, s.Key))
			if n == 0 {
				continue // unused domain — nothing to warn about
			}
			if n < s.Min {
				issues = append(issues, Issue{Severity: SevWarn, Domain: dom.ID, Slot: s.Key, Message: fmt.Sprintf("slot %q has %d port(s); needs at least %d", s.Key, n, s.Min)})
			}
			if s.Max > 0 && n > s.Max {
				issues = append(issues, Issue{Severity: SevError, Domain: dom.ID, Slot: s.Key, Message: fmt.Sprintf("slot %q has %d port(s); max is %d", s.Key, n, s.Max)})
			}
		}
	}
	return issues
}

// HasErrors reports whether any issue is blocking.
func HasErrors(issues []Issue) bool {
	for _, i := range issues {
		if i.Severity == SevError {
			return true
		}
	}
	return false
}

// slotRoleNames renders a slot's accepted role kinds for messages.
func slotRoleNames(s Slot) string {
	if len(s.RoleKinds) == 0 {
		return "any " + string(s.Direction) + " role"
	}
	out := ""
	for i, rk := range s.RoleKinds {
		if i > 0 {
			out += " or "
		}
		out += roles.KindName(rk)
	}
	return out
}

// compile-time: keep ports import referenced even if KindName moves.
var _ = ports.KindName
