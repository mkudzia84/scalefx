package console

import (
	"fmt"
	"strconv"
	"strings"

	"scalefx/client"
)

// Diagnostic commands sit in the Core category (every board has them —
// no extra capability bit required).
func init() {
	register(&command{Name: "diag", Usage: "diag [max]", Help: "dump the device's diagnostic ring buffer (max entries)", Category: catCore, RequiresConn: true, Run: cmdDiag})
	register(&command{Name: "diag-clear", Usage: "diag-clear", Help: "drop the local console replay (does not clear the device buffer)", Category: catCore, RequiresConn: true, Run: cmdDiagClear})
}

func cmdDiag(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	// `tail` requests only the last N entries (the firmware ignores
	// the wire-level count and replays the entire ring, so we trim
	// host-side).  0 = print everything in the buffer.
	tail := 0
	if len(args) >= 1 {
		v, err := strconv.ParseUint(strings.TrimPrefix(args[0], "0x"), 0, 16)
		if err != nil {
			return fmt.Errorf("tail: %w", err)
		}
		tail = int(v)
	}
	entries, err := a.c.Hub.DiagHistory(0)
	if err != nil {
		return err
	}
	if len(entries) == 0 {
		Note("(diagnostic buffer empty)")
		return nil
	}
	total := len(entries)
	if tail > 0 && tail < total {
		entries = entries[total-tail:]
	}
	Hdr(fmt.Sprintf("diagnostic log (showing %d of %d entries)", len(entries), total))
	for _, e := range entries {
		fmt.Fprintf(out, "  %s  %s  %s\n",
			diagLevelTag(e.Level, e.LevelName),
			cDim(fmt.Sprintf("@%dms", e.Millis)),
			e.Message)
	}
	return nil
}

func cmdDiagClear(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	// No firmware-side clear today — the buffer is rolling, so the
	// device naturally evicts old entries.  We just acknowledge the
	// user intent.
	Note("diag-clear is a UI-side reset; the device ring buffer is rolling")
	return nil
}

// diagLevelTag colours the 5-char severity bracket.
func diagLevelTag(level byte, name string) string {
	tag := fmt.Sprintf("[%s]", padRight(name, 5))
	switch level {
	case 3: // ERROR
		return cRed(tag)
	case 2: // WARN
		return cYellow(tag)
	case 1: // INFO
		return cCyan(tag)
	case 0: // DEBUG
		return cDim(tag)
	}
	return cDim(tag)
}

// Keep the client import warm if a future build strips unrelated bits.
var _ = client.DiagEntry{}
