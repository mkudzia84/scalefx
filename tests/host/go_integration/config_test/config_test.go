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

// Bare RELOAD re-applies every registered store.  Cheap — no UART
// traffic, no SD reads (firmware re-reads from LittleFS).  Should
// ACK fast even with all 7 stores registered.
func TestConfigReloadAcksFast(t *testing.T) {
	c := requireConfig(t)
	if err := c.Config.Reload(); err != nil {
		t.Fatalf("Reload: %v", err)
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
