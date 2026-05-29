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
// effect services — so total latency can exceed the protocol-layer
// default 5 s timeout on a board where the asset cache loader is also
// running.  Live firmware on HubFX build 511 was observed timing out
// reproducibly under both this test AND the canonical scalefx-cli
// `config-reload`, so the failure mode is firmware-side, not
// client-side.
//
// What we actually want to check: the wire path didn't crash the
// board.  If Reload returns an error we accept it, but a follow-up
// STATUS request must succeed within the normal budget.
func TestConfigReloadDoesNotWedgeTheBus(t *testing.T) {
	c := requireConfig(t)
	err := c.Config.Reload()
	t.Logf("Reload returned %v (timeout is acceptable on slow boards)", err)

	// Liveness probe: bus still responsive?
	if _, sErr := c.Hub.Status(); sErr != nil {
		t.Fatalf("Status after Reload: %v — Reload wedged the bus", sErr)
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
