// jeti_probe — diagnose "no Jeti channel output in the GUI".
//
// Attaches the Jeti EX input role on IN_1 (input idx 0), subscribes the wire
// broadcast (what Studio's live bars consume), and prints every InputValue
// frame the host decodes — so we can see whether the firmware emits channels
// at all, whether they're flagged valid, and whether the subscribe ACK'd.
//
//	SCALEFX_HUBFX_PORT=COM15  (optional; auto-detects)
//	IN_IDX=0                  input port index (default 0 = IN_1)
//	SECS=8                    capture duration
//
// Run:  go run .
package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"

	"scalefx/client"
	"scalefx/firmware"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
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
	inIdx := byte(envInt("IN_IDX", 0))
	secs := envInt("SECS", 8)

	c, id, err := client.Connect(port, client.Options{Timeout: 2 * time.Second})
	if err != nil {
		fmt.Printf("connect: %v\n", err)
		os.Exit(1)
	}
	defer c.Close()
	fmt.Printf("connected %s build %d on %s\n", id.DeviceName, id.BuildNumber, port)

	// Attach the Jeti EX input role (idempotent if already attached).
	if err := c.Topology.AttachRole("", ports.KindInput, inIdx, roles.KindJetiExInput, nil); err != nil {
		fmt.Printf("AttachRole jeti-ex-input idx=%d: %v\n", inIdx, err)
	} else {
		fmt.Printf("AttachRole jeti-ex-input idx=%d: OK\n", inIdx)
	}

	// Capture decoded input frames (the live-bar feed).
	var mu sync.Mutex
	var frames, validFrames int
	var lastCount int
	var lastValid bool
	var lastCh []client.ChannelValue
	c.Events.OnInputValue(func(v client.InputValue) {
		mu.Lock()
		frames++
		if len(v.Channels) > 0 && v.Channels[0].Valid {
			validFrames++
		}
		lastCount = len(v.Channels)
		if len(v.Channels) > 0 {
			lastValid = v.Channels[0].Valid
		}
		lastCh = v.Channels
		mu.Unlock()
	})

	// Subscribe the wire broadcast — the exact call Studio makes for the bars.
	if err := c.Input.SetJetiBroadcastHz(inIdx, 50); err != nil {
		fmt.Printf("SetJetiBroadcastHz(%d,50): %v   <-- subscribe FAILED (no wire frames)\n", inIdx, err)
	} else {
		fmt.Printf("SetJetiBroadcastHz(%d,50): ACK\n", inIdx)
	}

	conn := c.Conn()
	conn.ResetStats()
	fmt.Printf("\ncapturing %ds — InputValue frames (firmware → host live bars):\n", secs)
	for s := 1; s <= secs; s++ {
		time.Sleep(time.Second)
		mu.Lock()
		st := conn.Stats()
		chStr := ""
		for i := 0; i < len(lastCh) && i < 6; i++ {
			chStr += fmt.Sprintf(" ch%d=%d", lastCh[i].Channel, lastCh[i].Us)
		}
		fmt.Printf("[%ds] inputFrames=%d valid=%d | lastCount=%d lastValid=%v |%s | async=%d dropped=%d\n",
			s, frames, validFrames, lastCount, lastValid, chStr, st.RxAsync, st.AsyncDropped)
		mu.Unlock()
	}

	c.Input.SetJetiBroadcastHz(inIdx, 0)
	mu.Lock()
	fmt.Printf("\n==== RESULT ====\n")
	fmt.Printf("  total InputValue frames: %d (valid ch0: %d)\n", frames, validFrames)
	if frames == 0 {
		fmt.Printf("  -> NO wire frames reached the host: subscribe failed OR firmware not emitting (wireEnabled/hostVerbose gate).\n")
	} else if validFrames == 0 {
		fmt.Printf("  -> Frames arrive but channels are INVALID/empty: no Jeti signal on IN_1 (floating / noise-watchdog disabled the drain / receiver off).\n")
	} else {
		fmt.Printf("  -> Valid channels ARE flowing to the host: if the GUI shows nothing, the issue is Studio-side (event wiring / GUID remap / tab).\n")
	}
	mu.Unlock()
}
