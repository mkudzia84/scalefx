package devicemodel

import (
	"testing"

	"scalefx/protocol/core"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
	"scalefx/protocol/topology"
)

// sampleTopology builds a small hub + lightfx system:
//   - hub: 1 input (IN_1), 1 servo
//   - lightfx "3C4D": 4 pwm channels
func sampleTopology() ([]topology.BoardPorts, []topology.BoardRoles) {
	bp := []topology.BoardPorts{
		{GUID: "", DeviceName: "HubFx-AAAA", Ports: ports.PortList{
			Servos: []ports.PortDescriptor{{Index: 0}},
			Inputs: []ports.PortDescriptor{{Index: 0, Flags: ports.InputFlagPulse | ports.InputFlagSbus}},
		}},
		{GUID: "3C4D", DeviceName: "LightFx-3C4D", Ports: ports.PortList{
			Pwms: []ports.PortDescriptor{{Index: 0}, {Index: 1}, {Index: 2}, {Index: 3}},
		}},
	}
	return bp, nil
}

func TestBuildModelDerivesDirectionAndCaps(t *testing.T) {
	bp, br := sampleTopology()
	m := BuildModel(bp, br)
	if len(m.Ports) != 6 {
		t.Fatalf("expected 6 ports, got %d", len(m.Ports))
	}
	in, ok := m.Port(PortRef{GUID: "", Kind: ports.KindInput, Index: 0})
	if !ok {
		t.Fatal("hub input not found")
	}
	if in.Direction != DirInput {
		t.Errorf("input port direction = %s, want input", in.Direction)
	}
	if len(in.Caps) != 2 {
		t.Errorf("input caps = %v, want [PULSE SBUS]", in.Caps)
	}
	if in.HasRole() {
		t.Error("fresh input should have no role")
	}
}

func TestCandidatesFilterByRoleKind(t *testing.T) {
	bp, br := sampleTopology()
	m := BuildModel(bp, br)
	// No roles attached yet → leds slot (needs led-animator) has no
	// candidates among the pwm ports.
	if got := m.Candidates(DomainLandingLights, "leds"); len(got) != 0 {
		t.Fatalf("expected 0 led candidates before attach, got %d", len(got))
	}
	// Attach led-animator to two pwm ports.
	m.SetRole(PortRef{GUID: "3C4D", Kind: ports.KindPwm, Index: 0}, roles.KindLedAnimator)
	m.SetRole(PortRef{GUID: "3C4D", Kind: ports.KindPwm, Index: 1}, roles.KindLedAnimator)
	if got := m.Candidates(DomainLandingLights, "leds"); len(got) != 2 {
		t.Fatalf("expected 2 led candidates after attach, got %d", len(got))
	}
}

func TestInputSharedAcrossDomains(t *testing.T) {
	bp, br := sampleTopology()
	m := BuildModel(bp, br)
	m.SetRole(PortRef{GUID: "", Kind: ports.KindInput, Index: 0}, roles.KindRcPwmInput)
	in := PortRef{GUID: "", Kind: ports.KindInput, Index: 0}

	if err := m.Claim(Claim{Domain: DomainLandingLights, Slot: "trigger", Port: in}); err != nil {
		t.Fatalf("first input claim rejected: %v", err)
	}
	// Same input feeds a SECOND domain — must be allowed (shareable).
	if err := m.Claim(Claim{Domain: DomainLandingGear, Slot: "trigger", Port: in}); err != nil {
		t.Fatalf("shared input claim rejected: %v", err)
	}
	if got := m.ClaimsForPort(in); len(got) != 2 {
		t.Fatalf("input should feed 2 domains, got %d", len(got))
	}
	if issues := m.Validate(); HasErrors(issues) {
		t.Errorf("shared input should validate clean, got %v", issues)
	}
}

func TestOutputExclusive(t *testing.T) {
	bp, br := sampleTopology()
	m := BuildModel(bp, br)
	led := PortRef{GUID: "3C4D", Kind: ports.KindPwm, Index: 0}
	m.SetRole(led, roles.KindLedAnimator)

	if err := m.Claim(Claim{Domain: DomainLandingLights, Slot: "leds", Port: led}); err != nil {
		t.Fatalf("first output claim rejected: %v", err)
	}
	// A second domain claiming the same OUTPUT port must be rejected.
	err := m.Claim(Claim{Domain: DomainLighting, Slot: "leds", Port: led})
	if err == nil {
		t.Fatal("expected exclusive-output rejection, got nil")
	}
}

func TestRoleMismatchRejected(t *testing.T) {
	bp, br := sampleTopology()
	m := BuildModel(bp, br)
	servo := PortRef{GUID: "", Kind: ports.KindServo, Index: 0}
	m.SetRole(servo, roles.KindServoActuator)
	// landing-lights "leds" slot wants led-animator, not servo-actuator.
	if err := m.Claim(Claim{Domain: DomainLandingLights, Slot: "leds", Port: servo}); err == nil {
		t.Fatal("expected role-mismatch rejection, got nil")
	}
}

func TestValidateWarnsOnUnattachedAndUnderfilled(t *testing.T) {
	bp, br := sampleTopology()
	m := BuildModel(bp, br)
	led := PortRef{GUID: "3C4D", Kind: ports.KindPwm, Index: 0}
	// Claim WITHOUT attaching a role: checkClaim rejects it (role
	// mismatch), so inject directly to exercise Validate's warn path.
	m.Claims = append(m.Claims, Claim{Domain: DomainLandingLights, Slot: "leds", Port: led})
	issues := m.Validate()
	// led has roles.KindNone → "leds" requires led-animator → that's an
	// error (role mismatch), so this set should not be clean.
	if !HasErrors(issues) {
		t.Errorf("expected errors for unattached claimed port, got %v", issues)
	}
}

func TestApplyPreset(t *testing.T) {
	bp, br := sampleTopology()
	m := BuildModel(bp, br)
	p, ok := PresetByName("LightFX: 4-channel landing lights")
	if !ok {
		t.Fatal("preset not found")
	}
	assigns, warnings, err := m.ApplyPreset(p)
	if err != nil {
		t.Fatalf("ApplyPreset failed: %v", err)
	}
	if len(warnings) != 0 {
		t.Errorf("unexpected warnings: %v", warnings)
	}
	if len(assigns) != 4 {
		t.Fatalf("expected 4 role assigns, got %d", len(assigns))
	}
	if got := m.ClaimsForDomain(DomainLandingLights); len(got) != 4 {
		t.Fatalf("expected 4 landing-light claims, got %d", len(got))
	}
	if issues := m.Validate(); HasErrors(issues) {
		t.Errorf("preset result should validate clean, got %v", issues)
	}
}

func TestApplyPresetSkipsMissingBoard(t *testing.T) {
	// Hub-only topology — the lightfx preset should resolve nothing and
	// return warnings, not an error.
	bp := []topology.BoardPorts{
		{GUID: "", DeviceName: "HubFx-AAAA", Ports: ports.PortList{
			Inputs: []ports.PortDescriptor{{Index: 0, Flags: ports.InputFlagPulse}},
		}},
	}
	m := BuildModel(bp, nil)
	p, _ := PresetByName("LightFX: 4-channel landing lights")
	assigns, warnings, err := m.ApplyPreset(p)
	if err != nil {
		t.Fatalf("expected no hard error, got %v", err)
	}
	if len(assigns) != 0 {
		t.Errorf("expected 0 assigns on hub-only system, got %d", len(assigns))
	}
	if len(warnings) == 0 {
		t.Error("expected resolution warnings for absent lightfx board")
	}
}

func TestAvailableDomainsGatedByCaps(t *testing.T) {
	// Only the LandingLights cap present → only that domain (+ any 0-cap).
	got := AvailableDomains(core.CapLandingLights)
	for _, d := range got {
		if d.ID == DomainGun {
			t.Error("gun domain should be hidden without its cap")
		}
	}
	found := false
	for _, d := range got {
		if d.ID == DomainLandingLights {
			found = true
		}
	}
	if !found {
		t.Error("landing-lights domain should be available with its cap")
	}
}
