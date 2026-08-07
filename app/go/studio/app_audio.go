package main

// Wails-bound audio diagnostics surface.  Currently exposes the PSRAM
// AssetCache snapshot — the per-path residency table fed by each
// effect's config-apply (see AudioAssetCache::setPreloadsForOwner on
// the firmware side).  Frontend renders it in the AudioPreloadPanel.

import (
	"fmt"
	"math"

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

// AudioCodecPower is the live codec power-telemetry snapshot rendered
// by the Firmware tab's "Audio power" card.  Sourced from the
// CODEC_STATUS power tail (fw ≥ 2.42.0): the chip's own PVDD ADC, the
// auto-chosen analog gain, and the mixed-output peak since the last
// query.  The watts numbers are sine-average ESTIMATES into nominal
// loads computed from level × digital × analog gain — true output
// power measurement needs the TAS5825M IV-sense pipeline (future).
type AudioCodecPower struct {
	Available   bool    `json:"available"` // codec up + power tail present (fw ≥ 2.42.0)
	Model       string  `json:"model"`
	DieId       uint8   `json:"dieId"`     // 0x95 = TAS5825M silicon
	PvddVolts   float64 `json:"pvddVolts"` // measured amp rail
	AgainDb     float64 `json:"againDb"`   // auto-chosen analog attenuation (positive number)
	FullScaleVp float64 `json:"fullScaleVp"`
	OutPeakPct  float64 `json:"outPeakPct"`  // 0..100, peak since last poll
	SpeakerOhms float64 `json:"speakerOhms"` // load the estimate assumes
	EstWatts    float64 `json:"estWatts"`    // sine-avg estimate into SpeakerOhms
	Muted       bool    `json:"muted"`
	Faults      uint8   `json:"faults"` // GLOBAL_FAULT1 bits
}

// speakerOhms is the load the wattage estimate assumes.  The ScaleFX
// speaker BOM is 4 Ω; make this configurable if that ever diversifies.
const speakerOhms = 4.0

// CodecPower queries CODEC_STATUS and derives the display values.
// Polled by the Firmware tab (~2 s) — deliberately silent (no console
// echo), same as QueryDeviceStatus.
func (a *App) CodecPower() (AudioCodecPower, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	out := AudioCodecPower{}
	if a.c == nil {
		return out, fmt.Errorf("not connected")
	}
	if a.id.Capabilities&core.CapAudio == 0 {
		return out, nil // Available stays false
	}
	s, err := a.c.Audio.CodecStatus()
	if err != nil {
		return out, err
	}
	if !s.Initialized || !s.HasPower {
		return out, nil
	}
	out.Available = true
	out.Model = s.CodecName
	out.DieId = s.DieId
	out.Muted = s.Muted
	out.Faults = s.FaultStatus
	out.PvddVolts = float64(s.PvddMv) / 1000
	out.AgainDb = float64(s.AgainReg) * 0.5
	out.FullScaleVp = 29.5 * math.Pow(10, -out.AgainDb/20)
	level := float64(s.OutPeak) / 32767
	out.OutPeakPct = level * 100
	digDb := -0.5 * (float64(s.DigitalVol) - 0x30)
	if s.DigitalVol == 0xFF {
		level = 0 // codec-level mute
	}
	vpk := out.FullScaleVp * level * math.Pow(10, digDb/20)
	out.SpeakerOhms = speakerOhms
	out.EstWatts = vpk * vpk / (2 * speakerOhms)
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
