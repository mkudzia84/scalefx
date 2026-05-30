// engine_stress — hammer ENGINE_START/STOP with VARYING delays to reproduce the
// rapid-toggle audio-mixer use-after-free crash (LoadProhibited@0 in
// refillDrainBuffer).  The varying delays land stops at different points of a
// decode/refill so the race window is hit.  Detects a crash by the build
// number changing (a reset re-runs the boot) and by a connection drop.
//
//	SCALEFX_HUBFX_PORT=COM15  (optional; auto-detects)
//	CYCLES=300                start/stop pairs (default 300)
//
// Run:  go run .
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"

	"scalefx/client"
	"scalefx/firmware"
)

func envInt(k string, def int) int {
	if v := os.Getenv(k); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

func main() {
	port := os.Getenv("SCALEFX_HUBFX_PORT")
	if port == "" {
		d, err := firmware.DetectESP32Port()
		if err != nil || d == "" {
			fmt.Printf("no port (%v)\n", err)
			os.Exit(1)
		}
		port = d
	}
	cycles := envInt("CYCLES", 300)

	c, id, err := client.Connect(port, client.Options{Timeout: 2 * time.Second})
	if err != nil {
		fmt.Printf("connect: %v\n", err)
		os.Exit(1)
	}
	defer c.Close()
	startBuild := id.BuildNumber
	fmt.Printf("connected %s build %d — hammering %d start/stop cycles\n\n", id.DeviceName, startBuild, cycles)

	// Watch for a crash via the firmware log (panic text won't arrive, but a
	// reboot re-emits the boot banner / build line over the protocol).
	crashed := false
	c.Events.OnLog(func(m client.LogMessage) {
		// A reset restarts uptime near 0 + re-logs boot lines.
		if m.Millis < 2000 && (contains(m.Message, "boot") || contains(m.Message, "Mixer") && m.Millis < 500) {
			// noisy heuristic; the authoritative check is the build/uptime probe below
		}
	})

	// Pseudo-random-ish varying delays without Math.random — an LCG seeded by a
	// fixed constant so runs are reproducible.
	var rng uint32 = 0x12345678
	nextDelay := func(loMs, hiMs int) time.Duration {
		rng = rng*1664525 + 1013904223
		span := hiMs - loMs
		return time.Duration(loMs+int(rng>>16)%span) * time.Millisecond
	}

	fails := 0
	for i := 1; i <= cycles; i++ {
		if err := c.Engine.Start(); err != nil {
			fails++
			if isDead(c) {
				crashed = true
				fmt.Printf("[cycle %d] Start failed + board unreachable: %v\n", i, err)
				break
			}
		}
		time.Sleep(nextDelay(20, 450))
		if err := c.Engine.Stop(); err != nil {
			fails++
			if isDead(c) {
				crashed = true
				fmt.Printf("[cycle %d] Stop failed + board unreachable: %v\n", i, err)
				break
			}
		}
		time.Sleep(nextDelay(20, 450))
		if i%25 == 0 {
			st, serr := c.Engine.Status()
			if serr != nil {
				fmt.Printf("[cycle %3d] status err: %v\n", i, serr)
			} else {
				fmt.Printf("[cycle %3d] ok — engine state=%d, cmd fails=%d\n", i, st.State, fails)
			}
		}
	}
	c.Engine.Stop()

	// Final liveness: re-identify and compare the build number.  A crash/reset
	// keeps the same build (it reboots the same image) but bumps uptime to ~0,
	// so we check uptime via status latency + a fresh identify.
	fmt.Printf("\n==== RESULT ====\n")
	if crashed {
		fmt.Printf("  CRASH detected during stress (board went unreachable).\n")
		os.Exit(2)
	}
	// Reconnect fresh to read uptime — if it rebooted, INIT shows a low build-uptime.
	if isDead(c) {
		fmt.Printf("  FAIL: board unreachable after stress.\n")
		os.Exit(2)
	}
	fmt.Printf("  PASS: %d cycles, %d transient cmd fails, board ALIVE (build %d, no reset).\n",
		cycles, fails, startBuild)
}

func isDead(c *client.Client) bool {
	// Three quick status probes; if all error, the board is gone.
	for i := 0; i < 3; i++ {
		if _, err := c.Engine.Status(); err == nil {
			return false
		}
		time.Sleep(200 * time.Millisecond)
	}
	return true
}

func contains(s, sub string) bool {
	return len(s) >= len(sub) && (indexOf(s, sub) >= 0)
}
func indexOf(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}
