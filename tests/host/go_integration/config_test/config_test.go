package config_test

// Integration tests for the ConfigServicePolicy wire surface
// (CONFIG_RELOAD / CONFIG_STATUS / CONFIG_SAVE_PATH).
//
// HubFX runs ConfigServicePolicy with N config-store slots
// (/hubfx.yaml, /alerts.yaml, /enginefx.yaml, /gunfx.yaml,
// /landing.yaml, /gearcontrol.yaml, /lightfx.yaml).  These tests
// verify the wire path is sound; per-schema YAML parsing is covered
// by the native C++ suite (test_yaml_parser.cpp).

import (
	"os"
	"testing"

	"scalefx/client"
	"scalefx/protocol/core"
	"scalefx/tests/host/ports"
)

func TestMain(m *testing.M) {
	os.Exit(ports.RunWithSharedClient(m, "config_test"))
}

func requireConfig(t *testing.T) *client.Client {
	t.Helper()
	c := ports.RequireSharedClient(t)
	id := ports.SharedIdentity()
	if !core.HasCapability(id.Capabilities, core.CapConfig) {
		t.Skipf("device %s doesn't advertise CONFIG capability", id.DeviceName)
	}
	return c
}

// ─── CONFIG_RELOAD ────────────────────────────────────────────────────

// Bare RELOAD re-applies every registered store (HubFX has 7).  Each
// apply* callback runs synchronously — YAML parse + fan-out into the
// effect services — and must complete within the protocol-layer 5 s
// budget.  HubFX build 511 reproducibly TIMED OUT here (and rebooted
// when /gunfx.yaml's apply was reached) because applyGunFxConfig
// stack-copied a ~4 KB GunFxYamlConfig under the deeper board.process()
// call chain, overflowing loopTask.  Build 530+ heap-allocates the
// copy and bumps CONFIG_ARDUINO_LOOP_STACK_SIZE 12 K → 16 K; reload
// completes in ~800 ms.
//
// Locks in: the wire path returns success AND a follow-up STATUS
// confirms the bus is alive AND uptime monotonically advanced (a
// reboot would reset it to ≈ boot grace ~3-5 s).
func TestConfigReloadDoesNotWedgeTheBus(t *testing.T) {
	c := requireConfig(t)
	s0, sErr := c.Hub.Status()
	if sErr != nil {
		t.Fatalf("baseline Status: %v", sErr)
	}
	if err := c.Config.Reload(); err != nil {
		t.Fatalf("Reload: %v — was the 4 KB stack-copy regression re-introduced?", err)
	}
	s1, sErr := c.Hub.Status()
	if sErr != nil {
		t.Fatalf("Status after Reload: %v — Reload wedged the bus", sErr)
	}
	if s1.UptimeMs <= s0.UptimeMs {
		t.Errorf("uptime regressed across Reload: before=%d ms, after=%d ms — "+
			"firmware rebooted mid-apply", s0.UptimeMs, s1.UptimeMs)
	}
}

// ─── CONFIG_RELOAD_PATH ───────────────────────────────────────────────

// Path-routed RELOAD with a known file (/hubfx.yaml is the board master
// and is always present).  Should ACK; an "unknown path" is the only
// expected failure mode and would be a regression in the routing logic.
func TestConfigReloadHubfxYamlSucceeds(t *testing.T) {
	c := requireConfig(t)
	if err := c.Config.ReloadPath("/hubfx.yaml"); err != nil {
		t.Fatalf("ReloadPath(/hubfx.yaml): %v", err)
	}
}

// Reload of a path the firmware doesn't have a registered store for
// must NACK cleanly with a path-not-found error (NOT a crash / no
// reply / silent ack).
func TestConfigReloadUnknownPathNacks(t *testing.T) {
	c := requireConfig(t)
	err := c.Config.ReloadPath("/does_not_exist_unit_test.yaml")
	if err == nil {
		t.Error("ReloadPath(unknown): want NACK, got success — routing accepted bogus path")
	}
}

// ─── CONFIG_STATUS ────────────────────────────────────────────────────

// CONFIG_STATUS reports per-store metadata: loaded / file size / validation.
// We don't pin the file size here (varies per-device) but we DO check
// that the response decodes into the struct shape and at least one of
// the booleans is meaningful.
func TestConfigStatusDecodes(t *testing.T) {
	c := requireConfig(t)
	s, err := c.Config.Status()
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	t.Logf("config status: loaded=%v fileSize=%d validOk=%v",
		s.Loaded, s.FileSize, s.ValidOk)
	// Loaded should be true on a HubFX that booted successfully —
	// /hubfx.yaml is always present (the board ships with a default).
	if !s.Loaded {
		t.Error("ConfigStatus.Loaded=false on a booted HubFX — store didn't apply?")
	}
}
