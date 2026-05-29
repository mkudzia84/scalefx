// Package ports gives integration tests under tests/host/go_integration/
// a one-line way to grab an authenticated, identified HubFX client —
// or to skip cleanly when no hardware is reachable.
//
// Resolution order (per Rule 51):
//
//  1. SCALEFX_HUBFX_PORT env var (verbatim — "COM14", "/dev/ttyUSB0", …)
//  2. firmware.DetectESP32Port() — auto-detect the first port whose
//     VID/PID matches the CH343 bridge populated on HubFX rev 1, OR
//     the native ESP32-S3 USB-JTAG endpoint.
//  3. t.Skip("...") if neither resolves — CI without hardware exits clean.
//
// All integration tests MUST honour testing.Short() too — the helper
// fires t.Skip when -short is set, before touching the wire, so a
// disconnected dev machine running `go test -short ./...` produces
// SKIP not FAIL.
package ports

import (
	"flag"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"scalefx/client"
	"scalefx/firmware"
)

// OpenHubFX returns a connected, IDENTIFY'd HubFX client + the IDENTIFY
// payload (firmware version, build, capabilities).  Calls t.Skip if no
// hardware is reachable OR if `-short` was passed.
//
// The caller MUST `defer c.Close()` to release the serial port.
func OpenHubFX(t *testing.T) (*client.Client, client.Identity) {
	t.Helper()
	if testing.Short() {
		t.Skip("integration test; -short skips the whole tree")
	}

	port := os.Getenv("SCALEFX_HUBFX_PORT")
	if port == "" {
		detected, err := firmware.DetectESP32Port()
		if err != nil || detected == "" {
			t.Skipf("no HubFX detected (env SCALEFX_HUBFX_PORT unset, " +
				"DetectESP32Port returned %q / %v) — skipping integration test", detected, err)
		}
		port = detected
	}

	c, id, err := client.Connect(port, client.Options{
		Timeout: 5 * time.Second,
	})
	if err != nil {
		t.Skipf("connect %s failed: %v — skipping integration test", port, err)
	}
	// Sanity-check this is actually a HubFX, not a stray ESP32 board on
	// the same workstation (e.g. a PicoFX expander plugged in via CDC).
	// DeviceName looks like "HubFx-6DA4" (Prefix-GUIDSuffix per Rule 14).
	if !strings.HasPrefix(strings.ToLower(id.DeviceName), "hubfx") {
		c.Close()
		t.Skipf("device on %s is %q (not HubFX) — skipping HubFX integration test",
			port, id.DeviceName)
	}
	t.Logf("HubFX integration: %s v%s build %d on %s",
		id.DeviceName, id.FirmwareVersion, id.BuildNumber, port)
	return c, id
}

// ─── Shared-client TestMain pattern (Phase 5 / Phase D) ───────────────
//
// Per-test connect/disconnect against the firmware proved unreliable:
// each Close() kicks the firmware's UPLOAD lifecycle + audio resume +
// asset-cache loader catch-up just as the next test's IDENTIFY
// arrives, producing non-deterministic segment-ACK timeouts whose
// victim shifts between runs.  One connection eliminates that churn.
//
// SharedClient owns a singleton *Client opened by SetupSharedClient
// from a TestMain.  Per-test entry point is RequireSharedClient(t)
// which Skip()s cleanly if the shared client wasn't acquired.
//
// Usage pattern (every go_integration suite):
//
//   func TestMain(m *testing.M) {
//       os.Exit(ports.RunWithSharedClient(m, "audio_test"))
//   }
//
//   func TestSomething(t *testing.T) {
//       c := ports.RequireSharedClient(t)
//       // use c.Audio / c.Storage / c.Hub / etc.
//   }

var (
	sharedClient *client.Client
	sharedID     client.Identity
	sharedSkip   string
)

// RunWithSharedClient is the canonical TestMain helper.  Opens one
// shared client (env var / auto-detect / skip), runs all tests, and
// closes it.  `suiteName` is logged once at boot so a multi-suite
// stdout makes clear which suite is running against which HubFX.
//
// Returns the m.Run() exit code so the caller can pass it to os.Exit.
func RunWithSharedClient(m *testing.M, suiteName string) int {
	// testing.Short() panics if called before flag.Parse(), and we want
	// to honour -short up here so a CI without HW doesn't burn 5 s on
	// the connect attempt.
	flag.Parse()
	if testing.Short() {
		sharedSkip = "-short skips integration tests"
		return m.Run()
	}

	c, id, err := openSharedClientLowLevel()
	if err != nil {
		sharedSkip = err.Error()
		return m.Run()
	}
	sharedClient = c
	sharedID = id
	fmt.Printf("HubFX integration: %s %s v%s build %d (%s) on %s\n",
		suiteName, id.DeviceName, id.FirmwareVersion, id.BuildNumber,
		id.Platform, sharedClient.PortName())
	defer sharedClient.Close()
	return m.Run()
}

// RequireSharedClient is the per-test entry point — replaces the
// older per-test OpenHubFX(t) when the suite uses TestMain + shared
// client.  Skip()s cleanly when the shared client wasn't available.
func RequireSharedClient(t *testing.T) *client.Client {
	t.Helper()
	if testing.Short() {
		t.Skip("integration test; -short skips the whole tree")
	}
	if sharedSkip != "" {
		t.Skipf("shared client unavailable: %s", sharedSkip)
	}
	if sharedClient == nil {
		t.Skip("shared client unavailable (TestMain didn't open one)")
	}
	return sharedClient
}

// SharedIdentity returns the IDENTIFY payload from the shared client.
// Useful when a test wants to probe capabilities or firmware version
// before running.  Returns zero Identity when no shared client is up.
func SharedIdentity() client.Identity { return sharedID }

func openSharedClientLowLevel() (*client.Client, client.Identity, error) {
	port := os.Getenv("SCALEFX_HUBFX_PORT")
	if port == "" {
		detected, err := firmware.DetectESP32Port()
		if err != nil || detected == "" {
			return nil, client.Identity{}, fmt.Errorf("no HubFX detected: %v", err)
		}
		port = detected
	}
	c, id, err := client.Connect(port, client.Options{Timeout: 5 * time.Second})
	if err != nil {
		return nil, client.Identity{}, fmt.Errorf("connect %s: %w", port, err)
	}
	if !strings.HasPrefix(strings.ToLower(id.DeviceName), "hubfx") {
		c.Close()
		return nil, client.Identity{}, fmt.Errorf(
			"device on %s is %q (not HubFX)", port, id.DeviceName)
	}
	return c, id, nil
}
