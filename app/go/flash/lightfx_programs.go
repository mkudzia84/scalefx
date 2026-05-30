package main

// Post-flash lightfx program deploy.  After the firmware is flashed + verified,
// a freshly-flashed HubFX has no on-device lightfx programs (the LittleFS data
// partition is preserved across reflashes, but a brand-new board / wiped fs has
// none).  This uploads the bundled factory program YAMLs
// (media/presets/lightfx/programs/*.yaml) to /lightfx/programs/ on flash so the
// board comes up with the catalogue available.  Best-effort: a failure here
// logs a warning but never fails the flash.

import (
	"os"
	"path/filepath"
	"strings"
	"time"

	"scalefx/client"
	fw "scalefx/firmware"
)

const lightFxProgramsDevDir = "/lightfx/programs"

// deployLightFxPrograms uploads every bundled lightfx program to the device.
// `port` may be "" — the device is auto-detected.  No-op (with a warning) when
// the source dir or the board can't be found.
func deployLightFxPrograms(port string) {
	dir := findLightFxProgramsDir()
	if dir == "" {
		printWarning("lightfx programs: source dir not found — skipping deploy")
		return
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		printWarning("lightfx programs: read %s: %v — skipping", dir, err)
		return
	}
	var files []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(strings.ToLower(e.Name()), ".yaml") {
			files = append(files, e.Name())
		}
	}
	if len(files) == 0 {
		printWarning("lightfx programs: none found in %s — skipping", dir)
		return
	}

	if port == "" {
		p, derr := fw.DetectESP32Port()
		if derr != nil || p == "" {
			printWarning("lightfx programs: no device port (%v) — skipping deploy", derr)
			return
		}
		port = p
	}

	printInfo("Deploying %d lightfx program(s) → %s …", len(files), lightFxProgramsDevDir)
	c, _, cerr := client.Connect(port, client.Options{Timeout: 5 * time.Second})
	if cerr != nil {
		printWarning("lightfx programs: connect %s failed: %v — skipping", port, cerr)
		return
	}
	defer c.Close()

	// mkdir -p /lightfx/programs (ignore "already exists").
	_ = c.Storage.FileMkdir(lightFxProgramsDevDir, client.TargetFlash, client.MkdirFlagParents)

	ok := 0
	for _, name := range files {
		local := filepath.Join(dir, name)
		dst := lightFxProgramsDevDir + "/" + name
		if _, uerr := c.Storage.FileUpload(local, client.UploadOptions{
			Path: dst, Target: client.TargetFlash, Mode: client.UploadSync,
		}); uerr != nil {
			printWarning("  %s: %v", name, uerr)
			continue
		}
		ok++
		printInfo("  ✓ %s", name)
	}
	if ok == len(files) {
		printOK("lightfx programs: %d/%d deployed", ok, len(files))
	} else {
		printWarning("lightfx programs: %d/%d deployed (%d failed)", ok, len(files), len(files)-ok)
	}
}

// findLightFxProgramsDir locates media/presets/lightfx/programs relative to the
// working dir or the executable (the binary lives at app/go/scalefx-flash.exe;
// the media tree is at the repo root).
func findLightFxProgramsDir() string {
	const rel = "media/presets/lightfx/programs"
	var candidates []string
	if wd, err := os.Getwd(); err == nil {
		candidates = append(candidates, filepath.Join(wd, rel))
	}
	if exe, err := os.Executable(); err == nil {
		d := filepath.Dir(exe) // …/app/go
		candidates = append(candidates,
			filepath.Join(d, "..", "..", rel),       // repo root from app/go
			filepath.Join(d, "..", "..", "..", rel), // one deeper, just in case
		)
	}
	for _, c := range candidates {
		if st, err := os.Stat(c); err == nil && st.IsDir() {
			return c
		}
	}
	return ""
}
