package main

// PortWatcher polls the serial-port list on a fixed cadence and reports
// changes.  It is deliberately ignorant of connection state: it knows how
// to enumerate ports and how to notice that a *named* port has vanished,
// nothing more.  The App registers the port it currently holds via
// SetWatched(); when that port disappears the watcher fires OnVanished and
// forgets it (so the callback fires once per disconnect).  This inverts
// the old coupling where the watcher loop reached into the live client.

import (
	"sort"
	"strings"
	"sync"
	"time"

	"scalefx/protocol"
)

type PortWatcher struct {
	interval   time.Duration
	onChange   func(items []PortInfo, added, removed []string)
	onVanished func(port string)

	mu      sync.Mutex
	watched string

	stop chan struct{}
}

// NewPortWatcher builds a watcher.  onChange fires whenever the set of
// attached ports changes; onVanished fires when the port passed to
// SetWatched stops being present.  Either callback may be nil.
func NewPortWatcher(interval time.Duration, onChange func([]PortInfo, []string, []string), onVanished func(string)) *PortWatcher {
	return &PortWatcher{interval: interval, onChange: onChange, onVanished: onVanished}
}

// SetWatched names the port the watcher should report as vanished.  Pass
// "" to watch nothing (e.g. after a deliberate disconnect).
func (w *PortWatcher) SetWatched(port string) {
	w.mu.Lock()
	w.watched = port
	w.mu.Unlock()
}

// Start launches the polling goroutine.  Idempotent guard is the caller's
// responsibility (App starts it once at startup).
func (w *PortWatcher) Start() {
	w.stop = make(chan struct{})
	go w.loop()
}

// Stop ends the polling goroutine.
func (w *PortWatcher) Stop() {
	if w.stop != nil {
		close(w.stop)
		w.stop = nil
	}
}

func (w *PortWatcher) loop() {
	var lastKey string
	for {
		select {
		case <-w.stop:
			return
		case <-time.After(w.interval):
		}

		detailed := protocol.ListPortsDetailed()
		names := make([]string, len(detailed))
		for i, p := range detailed {
			names[i] = p.Name
		}
		sort.Strings(names)
		key := strings.Join(names, ",")

		if key != lastKey {
			added, removed := diffPortLists(lastKey, key)
			lastKey = key
			if w.onChange != nil {
				byName := make(map[string]string, len(detailed))
				for _, p := range detailed {
					byName[p.Name] = p.Description
				}
				items := make([]PortInfo, len(names))
				for i, n := range names {
					items[i] = PortInfo{Name: n, Description: byName[n]}
				}
				w.onChange(items, added, removed)
			}
		}

		// Watched-port disappearance check — fire once, then forget.
		w.mu.Lock()
		watched := w.watched
		present := stringSetCSV(key)
		vanished := watched != "" && !present[watched]
		if vanished {
			w.watched = ""
		}
		w.mu.Unlock()
		if vanished && w.onVanished != nil {
			w.onVanished(watched)
		}
	}
}

func diffPortLists(prev, next string) (added, removed []string) {
	p := stringSetCSV(prev)
	n := stringSetCSV(next)
	for v := range n {
		if !p[v] {
			added = append(added, v)
		}
	}
	for v := range p {
		if !n[v] {
			removed = append(removed, v)
		}
	}
	sort.Strings(added)
	sort.Strings(removed)
	return added, removed
}

func stringSetCSV(s string) map[string]bool {
	out := map[string]bool{}
	if s == "" {
		return out
	}
	for _, v := range strings.Split(s, ",") {
		out[v] = true
	}
	return out
}
