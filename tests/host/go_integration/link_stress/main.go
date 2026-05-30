// link_stress — a standalone diagnostic that reproduces the Studio
// "intermittent command timeout under broadcast flood" symptom and prints
// the protocol-layer link instrumentation (Connection.Stats()).
//
// It turns ON the hub's input wire broadcast (the 50 Hz flood Studio sees on
// the IO tab), optionally chains an artificial delay onto the async callback
// to simulate a SLOW consumer (Studio's Wails event emit, which runs INLINE
// in the reader goroutine), then hammers a synchronous query (flash.status)
// and reports how many time out.
//
// The experiment: run with ASYNC_DELAY_MS=0 (control) vs a non-zero delay.
// If timeouts climb with the delay while the wire is otherwise healthy, the
// reader goroutine is being starved by inline async dispatch — which is the
// architectural fix target (decouple async delivery from the reader).
//
//	SCALEFX_HUBFX_PORT=COM15  (optional; auto-detects CH343 otherwise)
//	ASYNC_DELAY_MS=0          per-async-frame sleep injected (default 0)
//	STRESS_SECS=20            run duration (default 20)
//	BCAST_HZ=50               input broadcast rate to request (default 50; 0=off)
//	POLL_MS=100               flash.status poll interval (default 100)
//
// Run:  go run .   (needs the HubFX connected)
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"

	"scalefx/client"
	"scalefx/firmware"
	"scalefx/protocol"
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
			fmt.Printf("no HubFX port (set SCALEFX_HUBFX_PORT; DetectESP32Port: %q / %v)\n", d, err)
			os.Exit(1)
		}
		port = d
	}
	asyncDelayMs := envInt("ASYNC_DELAY_MS", 0)
	secs := envInt("STRESS_SECS", 20)
	bcastHz := envInt("BCAST_HZ", 50)
	pollMs := envInt("POLL_MS", 100)

	c, id, err := client.Connect(port, client.Options{Timeout: 2 * time.Second})
	if err != nil {
		fmt.Printf("connect %s: %v\n", port, err)
		os.Exit(1)
	}
	defer c.Close()
	fmt.Printf("connected %s v%s build %d on %s\n", id.DeviceName, id.FirmwareVersion, id.BuildNumber, port)
	fmt.Printf("params: asyncDelay=%dms bcastHz=%d dur=%ds poll=%dms\n\n", asyncDelayMs, bcastHz, secs, pollMs)

	conn := c.Conn()

	// Simulate a slow async consumer (Studio's Wails emit) by chaining a sleep
	// onto whatever callback the client installed (the Events decoder).
	if asyncDelayMs > 0 {
		prev := conn.GetCallback()
		conn.SetCallback(func(r *protocol.Response) {
			if prev != nil {
				prev(r)
			}
			time.Sleep(time.Duration(asyncDelayMs) * time.Millisecond)
		})
	}

	// Turn on the broadcast flood on IN_1 (idx 0).  Jeti first; PPM fallback.
	if bcastHz > 0 {
		if err := c.Input.SetJetiBroadcastHz(0, byte(bcastHz)); err != nil {
			if err2 := c.Input.SetBroadcastHz(0, byte(bcastHz)); err2 != nil {
				fmt.Printf("warn: could not enable broadcast (jeti:%v ppm:%v)\n", err, err2)
			}
		}
	}

	conn.ResetStats()
	start := time.Now()
	deadline := start.Add(time.Duration(secs) * time.Second)
	nextReport := start.Add(time.Second)
	var sent, ok, to int
	var maxLat time.Duration
	var lastStats protocol.LinkStats

	for time.Now().Before(deadline) {
		t0 := time.Now()
		_, ferr := c.Storage.FlashStatus()
		lat := time.Since(t0)
		sent++
		if ferr != nil {
			to++
		} else {
			ok++
			if lat > maxLat {
				maxLat = lat
			}
		}
		if time.Now().After(nextReport) {
			s := conn.Stats()
			d := s.RxAsync - lastStats.RxAsync
			lastStats = s
			fmt.Printf("[%2.0fs] sent=%-4d ok=%-4d timeout=%-3d | async/s=%-4d rxF=%-6d tagged=%-4d unmatched=%-3d | asyncCBmax=%-8v dispMax=%v\n",
				time.Since(start).Seconds(), sent, ok, to, d, s.RxFrames, s.RxTagged, s.RxUnmatched, s.AsyncCBMax, s.DispatchMax)
			nextReport = nextReport.Add(time.Second)
		}
		time.Sleep(time.Duration(pollMs) * time.Millisecond)
	}

	if bcastHz > 0 {
		c.Input.SetJetiBroadcastHz(0, 0)
		c.Input.SetBroadcastHz(0, 0)
	}

	s := conn.Stats()
	pct := 0.0
	if sent > 0 {
		pct = 100 * float64(to) / float64(sent)
	}
	fmt.Printf("\n==== RESULT (asyncDelay=%dms, bcastHz=%d) ====\n", asyncDelayMs, bcastHz)
	fmt.Printf("  flash.status: sent=%d ok=%d TIMEOUT=%d (%.1f%%)  maxLatency=%v\n", sent, ok, to, pct, maxLat)
	fmt.Printf("  rx: frames=%d async=%d tagged=%d unmatched=%d  tx=%d\n", s.RxFrames, s.RxAsync, s.RxTagged, s.RxUnmatched, s.TxPackets)
	fmt.Printf("  asyncCB: total=%v max=%v   dispatchMax=%v   timeouts(stat)=%d\n", s.AsyncCBTotal, s.AsyncCBMax, s.DispatchMax, s.Timeouts)
}
