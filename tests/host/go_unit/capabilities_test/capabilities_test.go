package capabilities_test

// Unit tests for the IDENTIFY capability bitmask (controllers/lib/
// sfx_board/server/board_server.h ↔ app/go/protocol/core/core.go).
//
// Locks in:
//   - Each CapXxx constant's bit position (firmware MUST emit the same
//     bit shift for the matching kCapabilityBits)
//   - HasCapability subset semantics
//   - CapabilityNames mapping (Studio's "features" pretty-printer)
//   - That CapSlaveBus is the legacy alias of CapExpanderBus (renamed
//     during the generic-expander refactor)
//
// Regression target: every C++ ServicePolicy's `static constexpr
// uint32_t kCapabilityBits` value must agree with the matching Go
// constant.  When a new policy is added to BoardServer<...> on the
// firmware side, the Go side gets the new CapXxx constant added in
// the same commit; a stale build would mis-pretty-print the
// `features:` column of `system-info`.

import (
	"strings"
	"testing"

	"scalefx/protocol/core"
)

// ─── Capability bit positions ──────────────────────────────────────────

func TestCapBitPositions(t *testing.T) {
	// Each row pins (constant, expected single-bit value).
	tests := []struct {
		name string
		got  uint32
		want uint32
	}{
		{"Flash", core.CapFlash, 1 << 0},
		{"Sd", core.CapSd, 1 << 1},
		{"Audio", core.CapAudio, 1 << 2},
		{"UsbHost", core.CapUsbHost, 1 << 3},
		{"Engine", core.CapEngine, 1 << 4},
		{"Config", core.CapConfig, 1 << 5},
		{"ExpanderBus", core.CapExpanderBus, 1 << 6},
		{"Battery", core.CapBattery, 1 << 7},
		{"Ports", core.CapPorts, 1 << 8},
		{"Roles", core.CapRoles, 1 << 9},
		{"Topology", core.CapTopology, 1 << 10},
		{"Alerts", core.CapAlerts, 1 << 11},
		{"LightFx", core.CapLightFx, 1 << 12},
		{"LandingLights", core.CapLandingLights, 1 << 13},
		{"GearCtrl", core.CapGearCtrl, 1 << 14},
		{"GunFx", core.CapGunFx, 1 << 15},
		{"HasServoPorts", core.CapHasServoPorts, 1 << 16},
		{"HasPwmPorts", core.CapHasPwmPorts, 1 << 17},
		{"HasHBridgePorts", core.CapHasHBridgePorts, 1 << 18},
		{"HasInputPorts", core.CapHasInputPorts, 1 << 19},
	}
	for _, tc := range tests {
		if tc.got != tc.want {
			t.Errorf("Cap%s = 0x%08X (bit %d), want 0x%08X (bit %d) — "+
				"firmware kCapabilityBits MUST match",
				tc.name, tc.got, bitIdx(tc.got), tc.want, bitIdx(tc.want))
		}
	}
}

// CapSlaveBus = CapExpanderBus — legacy alias from the generic-expander
// refactor (CLAUDE.md: "Slave/SlaveType still in live code; rename
// lands atomically").  Tests document the alias is real.
func TestCapSlaveBusAliasOfExpanderBus(t *testing.T) {
	if core.CapSlaveBus != core.CapExpanderBus {
		t.Errorf("CapSlaveBus = 0x%08X, want CapExpanderBus = 0x%08X",
			core.CapSlaveBus, core.CapExpanderBus)
	}
}

// ─── HasCapability subset semantics ────────────────────────────────────

func TestHasCapabilitySingleBit(t *testing.T) {
	caps := core.CapFlash | core.CapSd | core.CapAudio
	if !core.HasCapability(caps, core.CapFlash) {
		t.Error("HasCapability(flash|sd|audio, flash) = false")
	}
	if !core.HasCapability(caps, core.CapSd) {
		t.Error("HasCapability(flash|sd|audio, sd) = false")
	}
	if core.HasCapability(caps, core.CapEngine) {
		t.Error("HasCapability(flash|sd|audio, engine) = true — bit not set")
	}
}

func TestHasCapabilityMultiBit(t *testing.T) {
	caps := core.CapFlash | core.CapSd | core.CapAudio | core.CapEngine
	if !core.HasCapability(caps, core.CapFlash|core.CapSd) {
		t.Error("HasCapability subset (flash+sd) of (flash|sd|audio|engine) = false")
	}
	// Subset check is bitwise — all bits in `want` must be set in `caps`.
	if core.HasCapability(caps, core.CapFlash|core.CapAlerts) {
		t.Error("HasCapability (flash+alerts) of (flash|sd|audio|engine) = true — alerts missing")
	}
}

func TestHasCapabilityZero(t *testing.T) {
	// Edge case: HasCapability(anything, 0) returns true because
	// (caps & 0) == 0 == want.  Documents the convention; callers
	// asking "does the peer support nothing?" get true, which is
	// trivially correct.
	if !core.HasCapability(core.CapFlash, 0) {
		t.Error("HasCapability(flash, 0) = false; bitwise subset always succeeds with empty want")
	}
}

// ─── CapabilityNames pretty-printer ────────────────────────────────────

func TestCapabilityNamesAllBits(t *testing.T) {
	// Every capability bit should have a name.  An "UNKNOWN_bit_N"
	// entry would indicate the names table is out of sync with the
	// constants.
	all := core.CapFlash | core.CapSd | core.CapAudio | core.CapUsbHost |
		core.CapEngine | core.CapConfig | core.CapExpanderBus |
		core.CapBattery | core.CapPorts | core.CapRoles | core.CapTopology |
		core.CapAlerts | core.CapLightFx | core.CapLandingLights |
		core.CapGearCtrl | core.CapGunFx | core.CapHasServoPorts |
		core.CapHasPwmPorts | core.CapHasHBridgePorts | core.CapHasInputPorts

	names := core.CapabilityNames(all)
	if len(names) == 0 {
		t.Fatal("CapabilityNames(all bits) returned empty")
	}
	for _, n := range names {
		if strings.HasPrefix(n, "UNKNOWN") || strings.HasPrefix(n, "unknown") {
			t.Errorf("CapabilityNames returned UNKNOWN entry %q — table out of sync", n)
		}
	}
}

func TestCapabilityNamesEmptyMask(t *testing.T) {
	names := core.CapabilityNames(0)
	if len(names) != 0 {
		t.Errorf("CapabilityNames(0) = %v, want empty", names)
	}
}

func TestCapabilityNamesContainsKnown(t *testing.T) {
	// Spot-check that the well-known features appear under their
	// expected strings (matches the Studio "features" column +
	// scalefx-cli system-info output).
	caps := core.CapFlash | core.CapAudio | core.CapEngine
	names := core.CapabilityNames(caps)

	want := map[string]bool{"FLASH": false, "AUDIO": false, "ENGINE": false}
	for _, n := range names {
		if _, ok := want[n]; ok {
			want[n] = true
		}
	}
	for k, found := range want {
		if !found {
			t.Errorf("expected name %q in %v, missing", k, names)
		}
	}
}

// ─── No bit collisions ─────────────────────────────────────────────────

// Every Cap constant must be a single distinct bit (except the
// CapSlaveBus alias which is intentionally equal to CapExpanderBus).
func TestNoCapabilityBitCollisions(t *testing.T) {
	caps := map[string]uint32{
		"Flash": core.CapFlash, "Sd": core.CapSd, "Audio": core.CapAudio,
		"UsbHost": core.CapUsbHost, "Engine": core.CapEngine,
		"Config": core.CapConfig, "ExpanderBus": core.CapExpanderBus,
		"Battery": core.CapBattery, "Ports": core.CapPorts,
		"Roles": core.CapRoles, "Topology": core.CapTopology,
		"Alerts": core.CapAlerts, "LightFx": core.CapLightFx,
		"LandingLights": core.CapLandingLights, "GearCtrl": core.CapGearCtrl,
		"GunFx": core.CapGunFx, "HasServoPorts": core.CapHasServoPorts,
		"HasPwmPorts": core.CapHasPwmPorts, "HasHBridgePorts": core.CapHasHBridgePorts,
		"HasInputPorts": core.CapHasInputPorts,
	}
	seen := map[uint32]string{}
	for name, bit := range caps {
		if other, dup := seen[bit]; dup {
			t.Errorf("bit collision: Cap%s and Cap%s both = 0x%08X",
				name, other, bit)
		}
		seen[bit] = name

		// Each must be a single bit set (power of two).
		if bit == 0 || (bit&(bit-1)) != 0 {
			t.Errorf("Cap%s = 0x%08X is not a single bit", name, bit)
		}
	}
}

// ─── BoardStateDisplay ──────────────────────────────────────────────────

// BoardStateDisplay disambiguates the SLAVE byte by the board's role:
// a hub-capable board (HubFX) renders as "host-driven" while a pure
// expander (GunFx / LightFx / GearControl on the bus) renders as
// "hub-driven."  The same wire byte → different human-friendly label.

func TestBoardStateDisplay_NonSlaveStatesIgnoreCaps(t *testing.T) {
	// Non-SLAVE states should produce the same label regardless of caps.
	for _, tc := range []struct {
		name  string
		state byte
		want  string
	}{
		{"idle", core.BoardStateIdle, "idle"},
		{"standalone", core.BoardStateStandalone, "standalone"},
		{"direct", core.BoardStateDirect, "direct"},
	} {
		got1 := core.BoardStateDisplay(tc.state, 0)
		got2 := core.BoardStateDisplay(tc.state, core.CapExpanderBus)
		if got1 != tc.want || got2 != tc.want {
			t.Errorf("%s: caps-blind label drifted: 0caps=%q expanderBus=%q want %q",
				tc.name, got1, got2, tc.want)
		}
	}
}

func TestBoardStateDisplay_SlaveOnHubCapableBoardIsHostDriven(t *testing.T) {
	// HubFX advertises CapExpanderBus; when it reports SLAVE the
	// upstream peer is the PC (CLI / Studio / scalefx-flash).
	hubFxCaps := core.CapFlash | core.CapSd | core.CapAudio | core.CapExpanderBus
	got := core.BoardStateDisplay(core.BoardStateSlave, hubFxCaps)
	if got != "host-driven" {
		t.Errorf("SLAVE on hub-capable board: got %q, want %q", got, "host-driven")
	}
}

func TestBoardStateDisplay_SlaveOnExpanderIsHubDriven(t *testing.T) {
	// Expanders (GunFX / LightFX / GearControl) don't advertise
	// CapExpanderBus.  SLAVE on these boards means "the upstream hub
	// master is driving me."  (Also fires for an expander plugged
	// directly into a PC for flashing — the wire byte alone can't
	// distinguish, but the operator knows from the cable.)
	expanderCaps := core.CapFlash | core.CapGunFx
	got := core.BoardStateDisplay(core.BoardStateSlave, expanderCaps)
	if got != "hub-driven" {
		t.Errorf("SLAVE on expander board: got %q, want %q", got, "hub-driven")
	}
}

func TestBoardStateDisplay_UnknownStateFallsBackToHex(t *testing.T) {
	// Any byte value the firmware hasn't defined yet must round-trip
	// through BoardStateName's hex fallback so the display still shows
	// SOMETHING informative.
	got := core.BoardStateDisplay(0xAA, 0)
	if got != "0xAA" {
		t.Errorf("unknown state 0xAA: got %q, want %q", got, "0xAA")
	}
}

// ─── Helpers ───────────────────────────────────────────────────────────

func bitIdx(v uint32) int {
	for i := 0; i < 32; i++ {
		if v == (uint32(1) << i) {
			return i
		}
	}
	return -1
}
