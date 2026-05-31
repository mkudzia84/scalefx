package protocol_test

// Concurrency regression net for the shared Connection state hardened on
// 2026-05-31 (Rule 56).  Studio drives the wire from many goroutines at once —
// the config-apply upload, status/telemetry pollers, the keepalive loop, per-RPC
// Wails handlers.  These tests are meaningful ONLY under `go test -race`: the
// pre-fix bug was an unlocked read-modify-write of nextTag that handed two
// concurrent commands the SAME correlation tag (one ACK landed in the other's
// waiter → the periodic "upload chunk @0 (seq=0): timeout").  Run via the
// go_unit suite, which the pre-merge gate executes with -race.

import (
	"sync"
	"testing"

	"scalefx/protocol"
)

// NextTag under heavy concurrency must never return the reserved 0 tag and must
// not trip the race detector.  Tags wrap 1..255, so global uniqueness can't be
// asserted across > 255 calls; the invariant we guard is "never 0" + race-clean.
func TestNextTagConcurrentNeverZero(t *testing.T) {
	c := protocol.NewConnection("test", 0, false)

	const goroutines = 32
	const perGoroutine = 500

	var wg sync.WaitGroup
	bad := make(chan byte, goroutines*perGoroutine)
	for g := 0; g < goroutines; g++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < perGoroutine; i++ {
				if tag := c.NextTag(); tag == 0 {
					bad <- tag
				}
			}
		}()
	}
	wg.Wait()
	close(bad)

	if n := len(bad); n != 0 {
		t.Fatalf("NextTag returned reserved tag 0 %d times under concurrency", n)
	}
}

// Within any window of 255 consecutive NextTag calls the values must be unique —
// a single goroutine exercises that monotonic-wrap invariant directly (the
// per-call lock must not corrupt the counter).
func TestNextTagSequentialUniqueWindow(t *testing.T) {
	c := protocol.NewConnection("test", 0, false)

	seen := make(map[byte]bool, 255)
	for i := 0; i < 255; i++ {
		tag := c.NextTag()
		if tag == 0 {
			t.Fatalf("tag 0 at i=%d", i)
		}
		if seen[tag] {
			t.Fatalf("duplicate tag %d within a 255-window at i=%d", tag, i)
		}
		seen[tag] = true
	}
	if len(seen) != 255 {
		t.Fatalf("expected 255 distinct tags in one wrap, got %d", len(seen))
	}
}

// The upload-phase flag is toggled by the upload goroutine and read by the
// keepalive goroutine + background pollers; it must be race-clean and observe
// the last write.
func TestUploadPhaseConcurrentAccess(t *testing.T) {
	c := protocol.NewConnection("test", 0, false)

	if c.UploadActive() {
		t.Fatal("upload phase should start inactive")
	}

	var wg sync.WaitGroup
	// Writer: flips the phase like FileUpload's begin/defer-end.
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 2000; i++ {
			c.SetUploadPhase(true)
			c.SetUploadPhase(false)
		}
	}()
	// Readers: like the keepalive gate + a poller checking UploadActive().
	for r := 0; r < 4; r++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < 2000; i++ {
				_ = c.UploadActive()
			}
		}()
	}
	wg.Wait()

	if c.UploadActive() {
		t.Fatal("upload phase should be inactive after writer finished on false")
	}
}
