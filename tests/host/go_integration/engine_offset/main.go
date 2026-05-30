// engine_offset — drive the engine through cold + warm starts and capture the
// firmware diag, to diagnose "starting offset doesn't take effect".
//
// Sequence:
//   1. config.reload /enginefx.yaml   -> "[engine] configure: startingOffset=…"
//   2. ENGINE_START (from Stopped)     -> "[engine] forceStart COLD … offset=0"
//   3. ENGINE_STOP                     -> enters Stopping (stop sound plays)
//   4. ENGINE_START (during Stopping)  -> "[engine] forceStart WARM … offset=N"
//                                         "[mixer] seek …" OR "… EXCEEDS file"
//   5. ENGINE_STOP (cleanup)
// All [engine]/[mixer]/[enginefx-config] diag lines are printed with timing.
//
//	SCALEFX_HUBFX_PORT=COM15  (optional; auto-detects)
//	WARM_DELAY_MS=600         delay between STOP and the warm START (default 600)
//
// Run:  go run .
package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"scalefx/client"
	"scalefx/firmware"
)

const enginePath = "/enginefx.yaml"

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
	warmDelay := time.Duration(envInt("WARM_DELAY_MS", 600)) * time.Millisecond

	c, id, err := client.Connect(port, client.Options{Timeout: 3 * time.Second})
	if err != nil {
		fmt.Printf("connect: %v\n", err)
		os.Exit(1)
	}
	defer c.Close()
	fmt.Printf("connected %s build %d\n\n", id.DeviceName, id.BuildNumber)

	// Capture engine/mixer/config diag lines with a wall-clock-ish stamp.
	var mu sync.Mutex
	start := time.Now()
	var lines []string
	c.Events.OnLog(func(m client.LogMessage) {
		msg := m.Message
		if strings.Contains(msg, "[engine]") || strings.Contains(msg, "[mixer]") ||
			strings.Contains(msg, "[enginefx-config]") || strings.Contains(msg, "engine") {
			mu.Lock()
			lines = append(lines, fmt.Sprintf("  +%5dms  %s", time.Since(start).Milliseconds(), strings.TrimRight(msg, "\r\n")))
			mu.Unlock()
		}
	})

	step := func(label string, fn func() error) {
		fmt.Printf(">> %s\n", label)
		if err := fn(); err != nil {
			fmt.Printf("   (%s: %v)\n", label, err)
		}
	}

	st, _ := c.Engine.Status()
	fmt.Printf("initial engine state=%d\n\n", st.State)

	step("config.reload /enginefx.yaml (expect configure log w/ startingOffset)", func() error { return c.Config.ReloadPath(enginePath) })
	time.Sleep(400 * time.Millisecond)

	step("ENGINE_STOP (ensure Stopped baseline)", func() error { return c.Engine.Stop() })
	time.Sleep(2500 * time.Millisecond) // let any stop sound finish → Stopped

	step("ENGINE_START  (COLD — from Stopped, expect offset=0)", func() error { return c.Engine.Start() })
	time.Sleep(1500 * time.Millisecond)
	if s, _ := c.Engine.Status(); true {
		fmt.Printf("   state after cold start = %d\n", s.State)
	}

	step("ENGINE_STOP   (enter Stopping — stop sound plays)", func() error { return c.Engine.Stop() })
	time.Sleep(warmDelay) // stay within the stop sound so the next start is WARM

	if s, _ := c.Engine.Status(); true {
		fmt.Printf("   state just before warm start = %d (want 3=Stopping)\n", s.State)
	}
	step("ENGINE_START  (WARM — during Stopping, expect offset=N + seek)", func() error { return c.Engine.Start() })
	time.Sleep(1500 * time.Millisecond)

	step("ENGINE_STOP   (cleanup)", func() error { return c.Engine.Stop() })
	time.Sleep(500 * time.Millisecond)

	mu.Lock()
	fmt.Printf("\n==== captured diag (%d lines) ====\n", len(lines))
	for _, l := range lines {
		fmt.Println(l)
	}
	mu.Unlock()
	fmt.Printf("\nInterpretation:\n")
	fmt.Printf("  - configure startingOffset=0   -> Studio's value never reached the service (reload/dirty issue)\n")
	fmt.Printf("  - forceStart WARM offset=0      -> warm/cold detection wrong (state not Stopping at re-engage)\n")
	fmt.Printf("  - mixer '… EXCEEDS file'        -> offset > start-sound length, seek skipped (use a smaller offset)\n")
	fmt.Printf("  - mixer 'seek … -> frame'       -> offset applied correctly\n")
}
