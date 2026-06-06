package error_collisions_test

// Guards the documented "error codes collide silently in Go's errorNames map
// (last init() wins → wrong name shown)" hazard (CLAUDE.md error ranges +
// Rule 8: every error lives in its spec range, one namespace per range).
//
// Each protocol sub-package's init() calls protocol.RegisterErrorNames(...).
// If two modules register the SAME numeric code under DIFFERENT names, the
// flat map keeps only the last writer and the CLI/Studio show the wrong name
// for one of them. RegisterErrorNames now records every distinct name per
// code; this test imports every sub-package (so all init()s run) and asserts
// CheckErrorNameCollisions() is empty.
//
// When this fails: the offending code is registered in two ranges — move one
// error into its correct spec range (see the error-range table in CLAUDE.md).

import (
	"sort"
	"testing"

	"scalefx/protocol"

	// Blank-import every sub-package so its init() registers its error names.
	_ "scalefx/protocol/alerts"
	_ "scalefx/protocol/audio"
	_ "scalefx/protocol/core"
	_ "scalefx/protocol/enginefx"
	_ "scalefx/protocol/expanders"
	_ "scalefx/protocol/gear"
	_ "scalefx/protocol/gunfx"
	_ "scalefx/protocol/landing"
	_ "scalefx/protocol/lightfx"
	_ "scalefx/protocol/ports"
	_ "scalefx/protocol/roles"
	_ "scalefx/protocol/storage"
	_ "scalefx/protocol/topology"
)

func TestNoErrorNameCollisions(t *testing.T) {
	cols := protocol.CheckErrorNameCollisions()
	if len(cols) == 0 {
		return
	}
	sort.Slice(cols, func(i, j int) bool { return cols[i].Code < cols[j].Code })
	for _, c := range cols {
		t.Errorf("error code 0x%02X registered under %d names %v — collision across "+
			"error ranges (one namespace per range; see CLAUDE.md error-range table)",
			byte(c.Code), len(c.Names), c.Names)
	}
}
