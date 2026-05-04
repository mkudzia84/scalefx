// Virtual-board discovery — write/read advertisement files in a
// well-known temp directory so scalefx-cli and Studio can list active
// virtual boards alongside real serial ports.
//
// Each running virtual_board process writes a JSON file at:
//
//   <TempDir>/scalefx-virtual/<pid>.json
//
// containing its TCP address, board kind, and device name. The mtime is
// refreshed every 2 s while the process is alive; readers (the protocol
// package) consider any file older than 5 s stale. On graceful exit the
// process removes its own file.

package virtualdiscovery

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// Entry is the JSON shape persisted in the discovery directory.
type Entry struct {
	Pid       int    `json:"pid"`
	Address   string `json:"address"`     // "tcp://host:port" — value put on the wire
	BoardKind string `json:"board_kind"`  // "lightfx" | "gearcontrol" | …
	Name      string `json:"name"`        // device-name advertised in INIT_READY
}

const (
	subdir       = "scalefx-virtual"
	staleAfter   = 5 * time.Second
	heartbeatGap = 2 * time.Second
)

// Dir returns the discovery directory under the OS temp dir.
func Dir() string {
	return filepath.Join(os.TempDir(), subdir)
}

// Advertise writes an advertisement file and starts a heartbeat
// goroutine that refreshes the file's mtime every 2 s. The returned
// stop func removes the file and stops the heartbeat.
func Advertise(e Entry) (stop func(), err error) {
	if err := os.MkdirAll(Dir(), 0o755); err != nil {
		return func() {}, err
	}
	path := pathFor(e.Pid)
	if e.Pid == 0 {
		e.Pid = os.Getpid()
		path = pathFor(e.Pid)
	}
	if err := writeFile(path, e); err != nil {
		return func() {}, err
	}

	stopCh := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		t := time.NewTicker(heartbeatGap)
		defer t.Stop()
		for {
			select {
			case <-stopCh:
				return
			case <-t.C:
				_ = os.Chtimes(path, time.Now(), time.Now())
			}
		}
	}()

	return func() {
		close(stopCh)
		wg.Wait()
		_ = os.Remove(path)
	}, nil
}

// List returns all live virtual-board entries (mtime within staleAfter).
func List() []Entry {
	dir := Dir()
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	out := make([]Entry, 0, len(entries))
	now := time.Now()
	for _, ent := range entries {
		if ent.IsDir() || !strings.HasSuffix(ent.Name(), ".json") {
			continue
		}
		path := filepath.Join(dir, ent.Name())
		info, err := ent.Info()
		if err != nil {
			continue
		}
		if now.Sub(info.ModTime()) > staleAfter {
			// Stale — probably a crashed process. Sweep it.
			_ = os.Remove(path)
			continue
		}
		e, err := readFile(path)
		if err != nil {
			continue
		}
		out = append(out, e)
	}
	return out
}

func pathFor(pid int) string {
	return filepath.Join(Dir(), fmt.Sprintf("%d.json", pid))
}

func writeFile(path string, e Entry) error {
	data, err := json.MarshalIndent(e, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o644)
}

func readFile(path string) (Entry, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return Entry{}, err
	}
	var e Entry
	if err := json.Unmarshal(data, &e); err != nil {
		return Entry{}, err
	}
	return e, nil
}

