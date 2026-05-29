package main

// Wails-bound audio diagnostics surface.  Currently exposes the PSRAM
// AssetCache snapshot — the per-path residency table fed by each
// effect's config-apply (see AudioAssetCache::setPreloadsForOwner on
// the firmware side).  Frontend renders it in the AudioPreloadPanel.

import (
	"fmt"

	"scalefx/protocol/audio"
	"scalefx/protocol/core"
)

// AudioPreloadEntry is one cached-asset row.  Mirrors
// `audio.PreloadEntry` but with frontend-friendly JSON keys.
type AudioPreloadEntry = audio.PreloadEntry

// AudioPreloadStatus is the full snapshot returned to the GUI.
type AudioPreloadStatus struct {
	ResidentBytes uint32              `json:"residentBytes"`
	BudgetBytes   uint32              `json:"budgetBytes"`
	Ready         uint16              `json:"ready"`
	Loading       uint16              `json:"loading"`
	Failed        uint16              `json:"failed"`
	Pinned        uint16              `json:"pinned"`
	Entries       []AudioPreloadEntry `json:"entries"`
}

// DeviceStatus is the periodic STATUS payload — uptime, counters,
// memory breakdown.  Frontend polls this from a timer to show live
// DRAM / PSRAM headroom on the Firmware tab.
type DeviceStatus struct {
	UptimeMs        uint32 `json:"uptimeMs"`
	FreeRAMBytes    uint32 `json:"freeRamBytes"`     // total heap (DRAM+PSRAM)
	FreeDramBytes   uint32 `json:"freeDramBytes"`    // internal SRAM
	FreePsramBytes  uint32 `json:"freePsramBytes"`   // external octal PSRAM
	HasMemExtension bool   `json:"hasMemExtension"`  // false on older firmware
	KeepaliveCount  uint32 `json:"keepaliveCount"`
	// BoardStateName is the raw enum constant ("IDLE" / "STANDALONE" /
	// "SLAVE" / "DIRECT"), useful for log analysis.  BoardStateDisplay
	// is the contextualised human-friendly label ("host-driven" /
	// "hub-driven" / …) that resolves the SLAVE-on-hub-vs-expander
	// ambiguity using the board's capability bits.  Studio's status
	// strip renders BoardStateDisplay; tooltips can fall back to the
	// raw BoardStateName when an operator hovers for detail.
	BoardStateName    string `json:"boardStateName"`
	BoardStateDisplay string `json:"boardStateDisplay"`
}

// QueryDeviceStatus issues the STATUS_REQ packet and returns the
// decoded fields most relevant to the GUI (live counters + memory).
// Returns an empty struct + error on disconnect.
func (a *App) QueryDeviceStatus() (DeviceStatus, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	out := DeviceStatus{}
	if a.c == nil {
		return out, fmt.Errorf("not connected")
	}
	s, err := a.c.Hub.Status()
	if err != nil {
		return out, err
	}
	out.UptimeMs = s.UptimeMs
	out.FreeRAMBytes = s.FreeRAMBytes
	out.FreeDramBytes = s.FreeDramBytes
	out.FreePsramBytes = s.FreePsramBytes
	out.HasMemExtension = s.HasMemExtension
	out.KeepaliveCount = s.KeepaliveCount
	out.BoardStateName = s.BoardStateName
	out.BoardStateDisplay = s.Display(a.id.Capabilities)
	return out, nil
}

// AudioPreloads queries the firmware for the current asset cache
// snapshot.  Returns an empty result + error string in the response if
// the device isn't connected or doesn't advertise the AUDIO capability.
func (a *App) AudioPreloads() (AudioPreloadStatus, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	out := AudioPreloadStatus{}
	a.echoCommand("audio.preloads")
	if a.c == nil {
		a.echoError("not connected")
		return out, fmt.Errorf("not connected")
	}
	if a.id.Capabilities&core.CapAudio == 0 {
		a.echoOutput("audio: not advertised by board")
		return out, nil
	}

	state, err := a.c.Audio.PreloadStatus()
	if err != nil {
		a.echoError("audio.preloads failed: %v", err)
		return out, err
	}
	out.ResidentBytes = state.ResidentBytes
	out.BudgetBytes = state.BudgetBytes
	out.Ready = state.Ready
	out.Loading = state.Loading
	out.Failed = state.Failed
	out.Pinned = state.Pinned
	out.Entries = state.Entries

	a.echoOutput(fmt.Sprintf("preloads: %d entries (ready=%d loading=%d failed=%d) resident=%d/%d B",
		len(out.Entries), out.Ready, out.Loading, out.Failed,
		out.ResidentBytes, out.BudgetBytes))
	return out, nil
}
