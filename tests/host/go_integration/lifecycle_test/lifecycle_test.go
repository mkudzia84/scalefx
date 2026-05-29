package lifecycle_test

// Integration tests for the HubFX lifecycle wire surface
// (IDENTIFY / INIT / STATUS / Keepalive).  Verifies the master's
// bring-up sequence end-to-end against real hardware.
//
// Honours Rule 51's skip contract via tests/host/ports.

import (
	"os"
	"strings"
	"testing"
	"time"

	"scalefx/protocol/core"
	"scalefx/tests/host/ports"
)

func TestMain(m *testing.M) {
	os.Exit(ports.RunWithSharedClient(m, "lifecycle_test"))
}

// ─── IDENTIFY ──────────────────────────────────────────────────────────

// IDENTIFY is called by ports.RunWithSharedClient as part of the
// Connect() bring-up.  This test inspects the cached Identity for
// shape: every field the host gates on must be populated.  Pre-fix
// for the 2026-05-28 protocol refactor, several IDENTIFY fields drifted
// (controllerType moved, capabilities byte width changed) and the host
// silently fell back to defaults.
func TestIdentityShape(t *testing.T) {
	c := ports.RequireSharedClient(t)
	_ = c // keep linter quiet

	id := ports.SharedIdentity()
	if id.DeviceName == "" {
		t.Error("DeviceName empty")
	}
	if !strings.HasPrefix(strings.ToLower(id.DeviceName), "hubfx") {
		t.Errorf("DeviceName = %q, want hubfx-prefix", id.DeviceName)
	}
	if id.GUID == "" || len(id.GUID) != 4 {
		t.Errorf("GUID = %q, want 4 hex chars (Rule 14 board GUID format)", id.GUID)
	}
	if id.FirmwareVersion == "" {
		t.Error("FirmwareVersion empty")
	}
	if id.Platform == "" {
		t.Error("Platform empty (want 'ESP32-S3' for HubFX)")
	}
	if id.BuildNumber == 0 {
		t.Error("BuildNumber = 0 — increment-on-flash convention broken")
	}
	if id.Capabilities == 0 {
		t.Error("Capabilities mask = 0 — empty caps means dispatcher routes nothing")
	}
	// Every HubFX firmware advertises FLASH + CONFIG at minimum.
	for _, want := range []uint32{core.CapFlash, core.CapConfig} {
		if !core.HasCapability(id.Capabilities, want) {
			t.Errorf("Capabilities 0x%08X missing required bit 0x%08X",
				id.Capabilities, want)
		}
	}
}

// HubFX uses an ESP32-S3 — confirm the platform string matches so a
// future board swap (different MCU) doesn't silently flip the host
// detection path.
func TestIdentityPlatformIsESP32S3(t *testing.T) {
	c := ports.RequireSharedClient(t)
	_ = c
	id := ports.SharedIdentity()
	if !strings.Contains(id.Platform, "ESP32") {
		t.Errorf("Platform = %q, want ESP32-prefixed", id.Platform)
	}
}

// ─── INIT (NO-OP on HubFX — auto-inits at boot) ───────────────────────

// HubFX auto-inits at boot — IDENTIFY suffices to bring it online.
// An explicit INIT after auto-init doesn't NACK or ACK on the live
// firmware; the client times out at the protocol-layer default (5 s).
// That's a deliberate no-op contract documented in CLAUDE.md.
//
// We accept the timeout error as the expected outcome and just verify
// the wire layer didn't permanently wedge — a follow-up STATUS_REQ
// should ACK quickly to prove the bus is still alive.
func TestInitIsNoOpAfterAutoInit(t *testing.T) {
	c := ports.RequireSharedClient(t)
	start := time.Now()
	err := c.Hub.Init()
	elapsed := time.Since(start)
	t.Logf("Init() returned %v in %v (timeout is expected on HubFX)", err, elapsed)

	// Liveness check: STATUS_REQ should still ACK fast.
	statusStart := time.Now()
	if _, sErr := c.Hub.Status(); sErr != nil {
		t.Fatalf("Status after Init: %v — wire layer wedged?", sErr)
	}
	if d := time.Since(statusStart); d > 500*time.Millisecond {
		t.Errorf("Status after Init took %v — bus stalled", d)
	}
}

// ─── STATUS ────────────────────────────────────────────────────────────

// STATUS gives the host a uniform health snapshot.  Header size is
// fixed at 22 bytes (CLAUDE.md "STATUS payload = 22-byte core header").
// Decoding into the Status struct must succeed regardless of how many
// per-module callbacks contributed payload tail.
func TestStatusReturnsValid(t *testing.T) {
	c := ports.RequireSharedClient(t)
	s, err := c.Hub.Status()
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if s.UptimeMs == 0 {
		t.Error("uptime = 0 — firmware just booted with no millis() ticks?")
	}
	if s.FreeRAMBytes == 0 {
		t.Error("FreeRAMBytes = 0 — heap query broken?")
	}
	// BoardState is one of IDLE / STANDALONE / SLAVE / DIRECT.  HubFX
	// running with a host-attached CLI / Studio reports SLAVE — it's
	// "slave to the host," not "slave board on the expander bus."
	// Verified against the CLI's `status` command which also shows
	// SLAVE.  The only state we genuinely don't want here is IDLE
	// (means the wire layer never came up).
	if s.BoardState == core.BoardStateIdle {
		t.Errorf("BoardState = IDLE — wire layer didn't initialise?")
	}
	t.Logf("STATUS: uptime=%dms freeRAM=%dKB state=%d",
		s.UptimeMs, s.FreeRAMBytes/1024, s.BoardState)
}

// Two STATUS calls in a row should yield monotonically advancing uptime
// (the firmware can't go back in time).  Locks in basic STATUS sanity
// — a stale STATUS broadcaster would show frozen uptime.
func TestStatusUptimeAdvances(t *testing.T) {
	c := ports.RequireSharedClient(t)
	s1, err := c.Hub.Status()
	if err != nil {
		t.Fatalf("Status #1: %v", err)
	}
	time.Sleep(150 * time.Millisecond)
	s2, err := c.Hub.Status()
	if err != nil {
		t.Fatalf("Status #2: %v", err)
	}
	if s2.UptimeMs <= s1.UptimeMs {
		t.Errorf("uptime didn't advance: s1=%d s2=%d", s1.UptimeMs, s2.UptimeMs)
	}
}

// ─── Keepalive (one-shot wire ping) ───────────────────────────────────

// Keepalive is a single wire-rate request the host can use to probe
// liveness without side effects.  Should ACK fast.
func TestKeepaliveAcksFast(t *testing.T) {
	c := ports.RequireSharedClient(t)
	start := time.Now()
	if err := c.Hub.Keepalive(); err != nil {
		t.Fatalf("Keepalive: %v", err)
	}
	elapsed := time.Since(start)
	if elapsed > 200*time.Millisecond {
		t.Errorf("Keepalive took %v — wire latency or scheduler stall?", elapsed)
	}
}
