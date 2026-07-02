package main

// Post-flash default-config seeding.  A brand-new / fs-wiped HubFX boots with
// NO /hubfx.yaml — the firmware falls back to a generic role layout (LedAnimator
// on every PWM port) so the board is functional, but the operator then has
// nothing concrete to open + tweak in Studio.  This uploads the bundled
// sensible default (media/presets/hubfx/minimal.yaml) to /hubfx.yaml, but ONLY
// when the device has none, so:
//   * an initial flash populates a real starting config the operator can edit;
//   * a reflash of an already-configured board NEVER clobbers its config —
//     LittleFS data survives a reflash, and we skip any file already present.
// Best-effort: every failure logs a warning and never fails the flash.

import (
	"os"
	"path/filepath"
	"time"

	"scalefx/client"
	fw "scalefx/firmware"
)

// hubFxDefaultConfigs maps an on-device path → its bundled preset (relative to
// media/presets/).  Only the master's /hubfx.yaml is seeded today; the effect
// sub-files (/alerts.yaml, /lightfx.yaml, …) fall back to schema defaults on
// the firmware side, so a bare board still boots functional without them.
var hubFxDefaultConfigs = []struct {
	dev    string // on-device path
	preset string // file under media/presets/ (slash-separated)
}{
	{dev: "/hubfx.yaml", preset: "hubfx/minimal.yaml"},
}

// seedDefaultConfigs uploads the bundled default config(s) to a freshly-flashed
// board when it has none.  `controller` gates which board family is seeded;
// `port` may be "" (auto-detected).  Safe to call after every flash.
func seedDefaultConfigs(controller, port string) {
	if controller != "hubfx" {
		return // only the master carries /hubfx.yaml today
	}
	root := findPresetsRoot()
	if root == "" {
		printWarning("seed config: media/presets not found — skipping")
		return
	}
	if port == "" {
		p, derr := fw.DetectESP32Port()
		if derr != nil || p == "" {
			printWarning("seed config: no device port (%v) — skipping", derr)
			return
		}
		port = p
	}

	// A board that JUST rebooted out of the flasher is still mounting its
	// LittleFS + re-enumerating USB; give it a beat before opening the wire so
	// the first IDENTIFY lands on a settled link (the 2026-06 fresh-flash
	// upload-wedge lesson — don't hit a just-booted board with file ops).
	time.Sleep(2 * time.Second)

	printInfo("Seeding default config(s) (only where the board has none) …")
	c, _, cerr := client.Connect(port, client.Options{Timeout: 6 * time.Second})
	if cerr != nil {
		printWarning("seed config: connect %s failed: %v — skipping", port, cerr)
		return
	}
	defer c.Close()

	// Make sure LittleFS is actually up before touching files.  On a brand-new
	// board the first boot formats the partition; if that boot-time init failed
	// (or is still settling), FLASH_STATUS_REQ triggers the firmware's
	// self-recovery (re-begin + format, firmware ≥ 2.35.1).  Without this, an
	// initial flash silently skipped seeding with NOT_INITIALIZED.
	if !ensureFlashReady(c) {
		printWarning("seed config: flash storage (LittleFS) unavailable — skipping. " +
			"If this is a board previously flashed with a different/test firmware, " +
			"its partition table may be stale: reflash (writes the factory image " +
			"incl. partition table) and run `scalefx-flash seed-config`.")
		return
	}

	seeded, preserved := 0, 0
	for _, f := range hubFxDefaultConfigs {
		local := filepath.Join(root, filepath.FromSlash(f.preset))
		if st, err := os.Stat(local); err != nil || st.IsDir() {
			printWarning("  %s: preset %s missing — skipping", f.dev, f.preset)
			continue
		}
		// Seed ONLY when we POSITIVELY confirm the board has no usable config.
		// Fail SAFE: a FileInfo error (transient wire hiccup on a just-rebooted
		// board — the post-flash verify itself often needs a retry) must NOT be
		// read as "absent", or we'd clobber the operator's existing config.  We
		// only seed when the query succeeds AND the file is genuinely absent or
		// 0-byte.
		info, ierr := fileInfoRetry(c, f.dev, 3)
		if ierr != nil {
			printWarning("  %s: could not verify (%v) — NOT seeding (won't risk "+
				"overwriting an existing config)", f.dev, ierr)
			preserved++
			continue
		}
		if info.Exists && info.Size > 0 {
			printInfo("  ✓ %s already present (%d bytes) — preserved", f.dev, info.Size)
			preserved++
			continue
		}
		if _, uerr := c.Storage.FileUpload(local, client.UploadOptions{
			Path: f.dev, Target: client.TargetFlash, Mode: client.UploadSync,
		}); uerr != nil {
			printWarning("  %s: upload failed: %v", f.dev, uerr)
			continue
		}
		seeded++
		printOK("  ✓ seeded %s ← %s", f.dev, f.preset)
	}
	if seeded > 0 {
		printOK("seed config: %d default(s) written, %d preserved", seeded, preserved)
	} else {
		printInfo("seed config: nothing to seed (%d preserved)", preserved)
	}
}

// ensureFlashReady confirms the LittleFS backend is initialized, giving the
// firmware a few chances to self-heal.  Each FLASH_STATUS_REQ makes the
// firmware retry `FlashModule::begin()` (which falls back to an explicit
// format) when the boot-time init failed — so a fresh chip whose first-boot
// format was slow/interrupted comes up here instead of failing the seed.
func ensureFlashReady(c *client.Client) bool {
	for i := 0; i < 3; i++ {
		st, err := c.Storage.FlashStatus()
		if err == nil && st.Initialized {
			if i > 0 {
				printOK("  flash storage recovered (%d KB total)", st.TotalBytes/1024)
			}
			return true
		}
		if err != nil {
			printWarning("  flash status query failed (attempt %d/3): %v", i+1, err)
		} else {
			printWarning("  flash storage not initialized (attempt %d/3) — firmware is retrying", i+1)
		}
		// A LittleFS format of the ~4 MB partition takes a moment.
		time.Sleep(1500 * time.Millisecond)
	}
	return false
}

// fileInfoRetry queries device file metadata, retrying a few times — the first
// wire op on a just-rebooted board can glitch (same reason the post-flash
// verify retries IDENTIFY).  Returns the last error only if EVERY attempt
// failed, so the caller can fail safe (skip seeding) rather than risk a clobber.
func fileInfoRetry(c *client.Client, path string, attempts int) (client.FileInfoResult, error) {
	var lastErr error
	for i := 0; i < attempts; i++ {
		// Config files live on FLASH — query that backend explicitly.  The
		// plain FileInfo defaults to SD, where /hubfx.yaml does not exist, so
		// it would read as "absent" and we'd clobber the real config.
		if info, err := c.Storage.FileInfoOn(path, client.TargetFlash); err == nil {
			return info, nil
		} else {
			lastErr = err
		}
		time.Sleep(500 * time.Millisecond)
	}
	return client.FileInfoResult{}, lastErr
}

// findPresetsRoot locates the media/presets directory relative to the working
// dir or the executable (the binary lives at app/go/scalefx-flash.exe; the
// media tree is at the repo root).  Mirrors findLightFxProgramsDir.
func findPresetsRoot() string {
	const rel = "media/presets"
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
