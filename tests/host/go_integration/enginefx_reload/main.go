// enginefx_reload — validate the /enginefx.yaml save+reload round-trip end to
// end (the exact path Studio's Apply uses: upload file → config.reload).
//
// Downloads /enginefx.yaml, flips starting_offset_ms to a fresh test value,
// uploads it, reloads the path, then re-downloads and confirms the new value
// persisted on the device.  Watch the firmware diag in parallel for the
// "[enginefx-config] applied" line to confirm the reload callback fired.
//
//	SCALEFX_HUBFX_PORT=COM15  (optional; auto-detects)
//
// Run:  go run .
package main

import (
	"bytes"
	"fmt"
	"os"
	"strconv"
	"time"

	"scalefx/client"
	"scalefx/firmware"

	"gopkg.in/yaml.v3"
)

const enginePath = "/enginefx.yaml"

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
	c, id, err := client.Connect(port, client.Options{Timeout: 5 * time.Second})
	if err != nil {
		fmt.Printf("connect: %v\n", err)
		os.Exit(1)
	}
	defer c.Close()
	fmt.Printf("connected %s build %d\n\n", id.DeviceName, id.BuildNumber)

	// 1) Download current /enginefx.yaml.
	dl, err := c.Storage.FileDownloadFrom(enginePath, client.TargetFlash, 10*time.Second)
	if err != nil {
		fmt.Printf("download %s: %v\n", enginePath, err)
		os.Exit(1)
	}
	var doc map[string]any
	if err := yaml.Unmarshal(dl.Data, &doc); err != nil {
		fmt.Printf("parse: %v\n  raw:\n%s\n", err, string(dl.Data))
		os.Exit(1)
	}
	cur := readOffset(doc)
	next := uint64(1234)
	if cur == 1234 {
		next = 5678 // ensure a real change each run
	}
	if v := os.Getenv("SET_OFFSET"); v != "" { // explicit restore/set, no flip
		if n, err := strconv.ParseUint(v, 10, 32); err == nil {
			next = n
		}
	}
	fmt.Printf("current starting_offset_ms = %d -> writing %d\n", cur, next)

	// 2) Mutate starting_offset_ms in sounds.transitions and re-marshal.
	setOffset(doc, next)
	out, err := yaml.Marshal(doc)
	if err != nil {
		fmt.Printf("marshal: %v\n", err)
		os.Exit(1)
	}
	tmp, _ := os.CreateTemp("", "enginefx-*.yaml")
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	tmp.Write(out)
	tmp.Close()

	// 3) Upload + reload (Studio's SetEngineConfig path).
	if _, err := c.Storage.FileUpload(tmpPath, client.UploadOptions{
		Path: enginePath, Target: client.TargetFlash, Mode: client.UploadSync,
	}); err != nil {
		fmt.Printf("upload: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("uploaded /enginefx.yaml")
	if err := c.Config.ReloadPath(enginePath); err != nil {
		fmt.Printf("reload: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("config.reload /enginefx.yaml — ACK")

	// 4) Re-download and verify persistence.
	time.Sleep(300 * time.Millisecond)
	dl2, err := c.Storage.FileDownloadFrom(enginePath, client.TargetFlash, 10*time.Second)
	if err != nil {
		fmt.Printf("re-download: %v\n", err)
		os.Exit(1)
	}
	var doc2 map[string]any
	yaml.Unmarshal(dl2.Data, &doc2)
	got := readOffset(doc2)
	fmt.Printf("\nre-downloaded starting_offset_ms = %d\n", got)
	if got == next {
		fmt.Printf("\n==== PASS: enginefx save+reload round-trip works ====\n")
		fmt.Printf("(if the firmware diag also shows \"[enginefx-config] applied\", the running service was reconfigured)\n")
	} else {
		fmt.Printf("\n==== FAIL: wrote %d but device has %d (reload/persist broken) ====\n", next, got)
		if bytes.Equal(dl.Data, dl2.Data) {
			fmt.Printf("(file unchanged on device — upload didn't land)\n")
		}
		os.Exit(2)
	}
}

func readOffset(doc map[string]any) uint64 {
	tr := transitions(doc, false)
	if tr == nil {
		return 0
	}
	switch v := tr["starting_offset_ms"].(type) {
	case int:
		return uint64(v)
	case uint64:
		return v
	case float64:
		return uint64(v)
	}
	return 0
}

func setOffset(doc map[string]any, v uint64) {
	tr := transitions(doc, true)
	tr["starting_offset_ms"] = int(v)
}

// transitions returns sounds.transitions, creating the chain when create=true.
func transitions(doc map[string]any, create bool) map[string]any {
	sounds, _ := doc["sounds"].(map[string]any)
	if sounds == nil {
		if !create {
			return nil
		}
		sounds = map[string]any{}
		doc["sounds"] = sounds
	}
	tr, _ := sounds["transitions"].(map[string]any)
	if tr == nil {
		if !create {
			return nil
		}
		tr = map[string]any{}
		sounds["transitions"] = tr
	}
	return tr
}
