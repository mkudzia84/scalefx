package upload_test

// Integration tests for the storage upload protocol (sync + stream),
// driving a real connected HubFX over USB serial.
//
// Skips cleanly when no hardware is reachable (per Rule 51) — see
// tests/host/ports/hubfx_port.go.
//
// Primary regression target: the 2026-05-28 four-bug stream-upload
// crash at 512 KB (firmware builds 486 → 506).  The fix landed in
// commit 12d8c69 with four cooperating changes:
//
//   1. vTaskDelay(1) in upload-active loop branch — TWDT defence
//   2. 32 KB DMA-cap upload buffer — fast SD write path
//   3. NativeUartStream::readBytes bulk override — fast UART drain
//   4. STREAM_SEGMENT_SIZE 512 KB → 16 KB — segment ≤ buffer
//
// Each test below would have caught at least one of those bugs.

import (
	"crypto/md5"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"scalefx/client"
	"scalefx/tests/host/ports"
)

// Test files we upload.  Picked because they exercise the size
// regimes that matter:
//   - Small (~50 KB): fits in 1 segment / 1 SD write, no boundaries.
//   - Medium (~180 KB): multiple SD writes, no segment boundary.
//   - Large (~1.4 MB): many segments, hits SD GC spikes, exercises
//                      per-segment ACK throttling.
const (
	smallFile  = "media/sounds/sys/lightfx_fw_error.mp3" // ~50 KB
	mediumFile = "media/sounds/KA50/engine_loop.mp3"     // ~180 KB
	largeFile  = "media/sounds/KA50/engine_start.mp3"    // ~1.4 MB
)

// repoFile resolves a path relative to the repo root from this test
// file's location (tests/host/go_integration/upload_test/) — four
// directory levels up.
func repoFile(rel string) string {
	_, here, _, ok := runtime.Caller(0)
	if !ok {
		return rel
	}
	root := filepath.Join(filepath.Dir(here), "..", "..", "..", "..")
	return filepath.Join(root, rel)
}

// requireFile skips the test if a fixture file is missing locally.
// Avoids false FAILs on shallow clones / repos missing /media.
func requireFile(t *testing.T, path string) string {
	t.Helper()
	abs := repoFile(path)
	if _, err := os.Stat(abs); err != nil {
		t.Skipf("fixture %s missing: %v", path, err)
	}
	return abs
}

// localMD5 hashes the file on disk for an independent verification of
// the firmware-echoed digest.
func localMD5(t *testing.T, path string) [16]byte {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	return md5.Sum(data)
}

// cleanup deletes a remote path; logs (not fails) if deletion fails so
// a subsequent test run doesn't trip on a leftover file.
func cleanup(t *testing.T, c *client.Client, remotePath string, target byte) {
	t.Helper()
	if err := c.Storage.FileDelete(remotePath, target, 0); err != nil {
		t.Logf("cleanup of %s failed (test still passed): %v", remotePath, err)
	}
}

// ensureRemoteDir runs `mkdir -p` semantics on the target so uploads
// to a fresh /test/ subtree don't trip on missing parents.  Idempotent.
func ensureRemoteDir(t *testing.T, c *client.Client, dir string, target byte) {
	t.Helper()
	if err := c.Storage.FileMkdir(dir, target, client.MkdirFlagParents); err != nil {
		// Pre-existing dir is fine.
		t.Logf("mkdir %s: %v (probably already exists)", dir, err)
	}
}

// TestMain delegates to the shared TestMain helper in
// tests/host/ports/ — see Rule 51 + Phase 5 commit body for why we
// share one client across all tests instead of per-test connect.
func TestMain(m *testing.M) {
	os.Exit(ports.RunWithSharedClient(m, "upload_test"))
}

// requireSharedClient is the per-test entry point.  Skips cleanly
// when no HW is reachable (or -short was passed).
func requireSharedClient(t *testing.T) *client.Client {
	return ports.RequireSharedClient(t)
}

// ─── Sync mode regression net ──────────────────────────────────────────

// Sync mode (per-chunk ACK) was confirmed working at all sizes during
// the 2026-05-28 bug hunt.  This test locks in that property — if
// sync ever breaks we want to know immediately because every other
// upload path falls back to it.
func TestSyncUploadSmallFile(t *testing.T) {
	c := requireSharedClient(t)
	ensureRemoteDir(t, c, "/test", client.TargetSD)

	local := requireFile(t, smallFile)
	remote := "/test/sync_small.mp3"

	res, err := c.Storage.FileUpload(local, client.UploadOptions{
		Path:   remote,
		Target: client.TargetSD,
		Mode:   client.UploadSync,
	})
	defer cleanup(t, c, remote, client.TargetSD)

	if err != nil {
		t.Fatalf("sync upload failed: %v", err)
	}
	want := localMD5(t, local)
	if res.RemoteMD5 != want {
		t.Errorf("remote MD5 %x ≠ local %x", res.RemoteMD5, want)
	}
	if !res.MD5Match {
		t.Errorf("MD5Match=false despite matching hashes — client logic broken")
	}
	t.Logf("sync %.1f KB in %s @ %.1f KB/s",
		float64(res.BytesSent)/1024, res.Elapsed, res.SpeedKBs)
}

// Same as above but on the flash backend — proves the flash path's
// allocator + write loop work end-to-end.
func TestSyncUploadToFlash(t *testing.T) {
	c := requireSharedClient(t)

	local := requireFile(t, smallFile)
	remote := "/test_sync_flash.mp3"

	res, err := c.Storage.FileUpload(local, client.UploadOptions{
		Path:   remote,
		Target: client.TargetFlash,
		Mode:   client.UploadSync,
	})
	defer cleanup(t, c, remote, client.TargetFlash)

	if err != nil {
		t.Fatalf("sync flash upload failed: %v", err)
	}
	want := localMD5(t, local)
	if res.RemoteMD5 != want {
		t.Errorf("remote MD5 %x ≠ local %x", res.RemoteMD5, want)
	}
}

// ─── Stream mode regression nets (the 512 KB four-bug fix) ────────────

// TestStreamUploadSmallFile — sanity check that stream mode works
// AT ALL.  This is the "small uploads work" baseline that, paired
// with TestStreamUploadLargeFile failing, narrowed the 2026-05-28
// diagnosis to stream-only.
func TestStreamUploadSmallFile(t *testing.T) {
	c := requireSharedClient(t)

	ensureRemoteDir(t, c, "/test", client.TargetSD)
	local := requireFile(t, smallFile)
	remote := "/test/stream_small.mp3"

	res, err := c.Storage.FileUpload(local, client.UploadOptions{
		Path:   remote,
		Target: client.TargetSD,
		Mode:   client.UploadStream,
	})
	defer cleanup(t, c, remote, client.TargetSD)

	if err != nil {
		t.Fatalf("stream upload (small) failed: %v", err)
	}
	want := localMD5(t, local)
	if res.RemoteMD5 != want {
		t.Errorf("remote MD5 %x ≠ local %x", res.RemoteMD5, want)
	}
}

// TestStreamUploadLargeFile — THE regression test for the 2026-05-28
// four-bug fix.  Pre-fix this rebooted the firmware at exactly 512 KB
// (one STREAM_SEGMENT_SIZE).  Post-fix it completes in ~3 s with
// matching MD5.  If this fails, ANY of the four fixes regressed:
//
//   * TWDT panic — would show as connection drop + reboot
//   * PSRAM source 160 KB/s SD writes — overall throughput collapses
//   * Single-byte UART reads — loop drains too slowly, timeout
//   * Segment > buffer — first segment-boundary kills the transfer
func TestStreamUploadLargeFile_RegressionNetFor512KB(t *testing.T) {
	c := requireSharedClient(t)

	ensureRemoteDir(t, c, "/test", client.TargetSD)
	local := requireFile(t, largeFile)
	remote := "/test/stream_large.mp3"

	start := time.Now()
	res, err := c.Storage.FileUpload(local, client.UploadOptions{
		Path:   remote,
		Target: client.TargetSD,
		Mode:   client.UploadStream,
	})
	defer cleanup(t, c, remote, client.TargetSD)
	elapsed := time.Since(start)

	if err != nil {
		t.Fatalf("stream upload (1.4 MB) failed: %v — DID THE 512 KB FIX REGRESS?", err)
	}
	want := localMD5(t, local)
	if res.RemoteMD5 != want {
		t.Errorf("remote MD5 %x ≠ local %x — UART overflow during SD GC spike?",
			res.RemoteMD5, want)
	}
	// Throughput floor: the post-fix tests measured ~430-465 KB/s
	// consistently.  300 KB/s is a generous floor that flags an order-
	// of-magnitude regression (e.g. PSRAM source bouncer back in
	// effect = ~160 KB/s).  Strict enough to catch the bug, loose
	// enough to absorb SD-card variance.
	const floorKBs = 300.0
	if res.SpeedKBs < floorKBs {
		t.Errorf("stream throughput %.1f KB/s < %v floor — PSRAM SD-write bouncer regressed?",
			res.SpeedKBs, floorKBs)
	}
	// Wall-clock timeout: 1.4 MB at 300 KB/s = ~5 s; pad to 20 s to
	// catch a hung upload that returns success but took absurdly long.
	if elapsed > 20*time.Second {
		t.Errorf("stream upload took %v — flow control broken?", elapsed)
	}
	t.Logf("stream 1.4 MB in %v @ %.1f KB/s (MD5 %x)",
		res.Elapsed, res.SpeedKBs, res.RemoteMD5)
}

// ─── Repeat for stability ──────────────────────────────────────────────

// TestStreamUploadRepeated runs the large-file stream upload three
// times back-to-back.  Catches:
//   - State leakage between uploads (audio not properly resumed →
//     suspended on next upload begin = wedge)
//   - SD card lock not released on cancel paths
//   - DMA-cap buffer not freed (heap_caps_malloc fails on 2nd attempt)
//   - Decoder/producer task leak across upload cycles
func TestStreamUploadRepeatedStability(t *testing.T) {
	if testing.Short() {
		t.Skip("repeated test; skipped under -short")
	}
	c := requireSharedClient(t)

	ensureRemoteDir(t, c, "/test", client.TargetSD)
	local := requireFile(t, mediumFile)
	const runs = 3

	for i := 0; i < runs; i++ {
		remote := fmt.Sprintf("/test/stream_repeat_%d.mp3", i)
		res, err := c.Storage.FileUpload(local, client.UploadOptions{
			Path:   remote,
			Target: client.TargetSD,
			Mode:   client.UploadStream,
		})
		if err != nil {
			t.Fatalf("run %d failed: %v", i, err)
		}
		want := localMD5(t, local)
		if res.RemoteMD5 != want {
			t.Errorf("run %d MD5 mismatch: got %x want %x", i, res.RemoteMD5, want)
		}
		cleanup(t, c, remote, client.TargetSD)
	}
}

// ─── Mixed-mode sanity ─────────────────────────────────────────────────

// TestStreamThenSync verifies that stream uploads don't leave the
// firmware in a state that breaks subsequent sync uploads (and
// vice-versa).  Mode switching must be transparent.
func TestStreamThenSync(t *testing.T) {
	c := requireSharedClient(t)
	ensureRemoteDir(t, c, "/test", client.TargetSD)

	local := requireFile(t, smallFile)

	// Stream first.
	r1, err := c.Storage.FileUpload(local, client.UploadOptions{
		Path: "/test/mix_stream.mp3", Target: client.TargetSD, Mode: client.UploadStream,
	})
	defer cleanup(t, c, "/test/mix_stream.mp3", client.TargetSD)
	if err != nil {
		t.Fatalf("stream leg: %v", err)
	}

	// Then sync.  Must succeed without a reboot or audio-state wedge.
	r2, err := c.Storage.FileUpload(local, client.UploadOptions{
		Path: "/test/mix_sync.mp3", Target: client.TargetSD, Mode: client.UploadSync,
	})
	defer cleanup(t, c, "/test/mix_sync.mp3", client.TargetSD)
	if err != nil {
		t.Fatalf("sync leg after stream: %v — upload state not cleaned?", err)
	}

	want := localMD5(t, local)
	if r1.RemoteMD5 != want || r2.RemoteMD5 != want {
		t.Errorf("MD5 mismatch — stream %x / sync %x / want %x",
			r1.RemoteMD5, r2.RemoteMD5, want)
	}
}
