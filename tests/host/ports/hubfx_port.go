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
