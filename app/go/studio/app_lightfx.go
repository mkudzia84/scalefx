package main

// LightFx bindings — the Lighting tab's left column.
//
// /lightfx.yaml carries the master config (master brightness +
// active program list + optional RC program selector with µs-band
// ranges).  Each program is its own YAML file under
// /lightfx/programs/<name>.yaml — those are author-time content
// (FAA-compliant flight, anti-collision strobe, …) and Studio
// surfaces a picker so the operator can add / remove them from the
// active list without hand-editing.  Per-program EVENT editing
// stays a follow-up (the firmware already loads them at apply
// time + Phase 2 of the Lighting tab will add the editor).
//
// This file owns the Wails surface; the TS side mirrors the DTOs in
// app/go/studio/frontend/src/lib/lightfx.ts.

import (
	"fmt"
	"os"
	"path"
	"sort"
	"strings"
	"time"

	"scalefx/client"
	"scalefx/protocol/roles"

	"gopkg.in/yaml.v3"
)

// ─── DTOs (mirror /lightfx.yaml schema) ───────────────────────────────

// ProgramSelectorRangeDTO — one band of the RC-driven program picker.
// Maps a µs window to a program NAME (basename of a program file
// without the .yaml extension).  Hysteresis on the parent stabilises
// the selector when the stick sits near a boundary.
type ProgramSelectorRangeDTO struct {
	FromUs  uint16 `yaml:"from_us"  json:"fromUs"`
	ToUs    uint16 `yaml:"to_us"    json:"toUs"`
	Program string `yaml:"program"  json:"program"`
}

// ProgramSelectorDTO — optional RC-channel program switcher.  When
// `Input` is empty, the active program is whatever Studio has loaded
// last (or the first one in `Programs[]` on boot).
type ProgramSelectorDTO struct {
	Enabled      bool                      `yaml:"enabled,omitempty"        json:"enabled"`
	Input        string                    `yaml:"input,omitempty"          json:"input"`
	HysteresisUs uint16                    `yaml:"hysteresis_us,omitempty"  json:"hysteresisUs"`
	Ranges       []ProgramSelectorRangeDTO `yaml:"ranges,omitempty"         json:"ranges"`
}

// LightFxChannelDTO — instance-owned LED channel binding.  Lifted out
// of per-program tracks 2026-05-27: instead of every program file
// independently naming + picking ports, the LightFx instance declares
// its channel set ONCE here.  Programs reference channels by Name;
// the firmware (Phase 2 work) reads this block to resolve names to
// physical output ports without falling back to /hubfx.yaml ports[].
//
// Port may be nil immediately after auto-derivation from pre-existing
// programs (when /lightfx.yaml predates schema_version 2 and Studio
// derived the channel list from program tracks but couldn't infer the
// physical port).  Operator picks the port in the Studio Channels card;
// until then the channel is silent.
type LightFxChannelDTO struct {
	Name                 string       `yaml:"name"                              json:"name"`
	Port                 *PortRefDTO  `yaml:"port,omitempty"                    json:"port"`
	DefaultBrightnessPct uint8        `yaml:"default_brightness_pct,omitempty"  json:"defaultBrightnessPct"`
}

// LightFxConfigDTO is the top-level /lightfx.yaml shape.  Programs
// are FULL LittleFS paths (e.g. "/lightfx/programs/helicopter_off.yaml"),
// not bare names, so the firmware can `fopen()` them directly.  Studio
// derives the display name from the basename minus .yaml.
type LightFxConfigDTO struct {
	SchemaVersion       int                 `yaml:"schema_version"          json:"schemaVersion"`
	Enabled             bool                `yaml:"enabled"                 json:"enabled"`
	MasterBrightnessPct uint8               `yaml:"master_brightness_pct"   json:"masterBrightnessPct"`
	// Channels[] — instance-owned LED channel pool (since v2).  Programs
	// reference channels by name; the firmware resolves each program
	// track's `channel:` against this list FIRST (lightfx_program_loader.h
	// resolveLightFxChannelPooled), falling back to /hubfx.yaml LedAnimator
	// labels only on a miss.  The hub's own GUID on a port collapses to
	// hub-local (portRefFromNode) so the resolved port matches the role.
	Channels            []LightFxChannelDTO `yaml:"channels,omitempty"      json:"channels"`
	Programs            []string            `yaml:"programs"                json:"programs"`
	ProgramSelector     ProgramSelectorDTO  `yaml:"program_selector,omitempty" json:"programSelector"`
}

// ProgramFileInfo — entry returned by ListAvailablePrograms so the
// Studio UI can populate the "add program" dropdown without the user
// having to type a path.  Discovered by walking the on-device
// /lightfx/programs/ directory.
type ProgramFileInfo struct {
	Path string `json:"path"` // full LittleFS path
	Name string `json:"name"` // basename minus .yaml — what selector ranges reference
}

func defaultLightFxConfig() LightFxConfigDTO {
	return LightFxConfigDTO{
		SchemaVersion:       1,
		Enabled:             true,
		MasterBrightnessPct: 100,
		Programs:            []string{},
	}
}

// ─── /lightfx.yaml round-trip ─────────────────────────────────────────

const lightfxPath = "/lightfx.yaml"

func (a *App) GetLightFxConfig() (LightFxConfigDTO, error) {
	defer a.diag.Around("GetLightFxConfig", nil)()
	c := a.snapshotClient()
	if c == nil {
		return defaultLightFxConfig(), fmt.Errorf("not connected")
	}
	res, err := c.Storage.FileDownloadFrom(lightfxPath, client.TargetFlash, 5*time.Second)
	if err != nil {
		a.diag.Info("LIGHTFX", "no /lightfx.yaml yet (%v) — returning defaults", err)
		return defaultLightFxConfig(), nil
	}
	cfg := defaultLightFxConfig()
	if err := yaml.Unmarshal(res.Data, &cfg); err != nil {
		return defaultLightFxConfig(), fmt.Errorf("parse lightfx.yaml: %w", err)
	}
	if cfg.SchemaVersion == 0 {
		cfg.SchemaVersion = 1
	}
	if cfg.Programs == nil {
		cfg.Programs = []string{}
	}
	// Canonicalise hub-identity channel-port GUIDs → "" (instructions/31) so the
	// channel-pool dropdowns resolve against the canonical device model.
	folds := 0
	for i := range cfg.Channels {
		p := cfg.Channels[i].Port
		if p == nil {
			continue
		}
		if c, was := a.canonGuid(p.Guid); was {
			a.diag.Info("LIGHTFX", "[gap] channel[%d] ref guid=%q kind=%q idx=%d → canon \"\" (hub=%q)",
				i, p.Guid, p.Kind, p.Idx, a.id.GUID)
			p.Guid = c
			folds++
		}
	}
	a.diag.Info("LIGHTFX", "loaded /lightfx.yaml: %d program(s), selector=%v; folded %d hub-identity port ref(s) → \"\" (hub=%q)",
		len(cfg.Programs), cfg.ProgramSelector.Enabled, folds, a.id.GUID)
	return cfg, nil
}

func (a *App) SetLightFxConfig(cfg LightFxConfigDTO) error {
	defer a.diag.Around("SetLightFxConfig", nil)()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	if cfg.SchemaVersion == 0 {
		cfg.SchemaVersion = 1
	}
	data, err := yaml.Marshal(&cfg)
	if err != nil {
		return fmt.Errorf("serialise: %w", err)
	}
	tmp, err := os.CreateTemp("", "lightfx-*.yaml")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	tmp.Close()
	if _, err := c.Storage.FileUpload(tmpPath, client.UploadOptions{
		Path: lightfxPath, Target: client.TargetFlash, Mode: client.UploadSync,
	}); err != nil {
		return fmt.Errorf("upload lightfx.yaml: %w", err)
	}
	if err := c.Config.ReloadPath(lightfxPath); err != nil {
		return fmt.Errorf("reload: %w", err)
	}
	a.diag.Info("LIGHTFX", "saved + reloaded /lightfx.yaml (%d program(s))", len(cfg.Programs))
	return nil
}

// ─── Per-program file ops ─────────────────────────────────────────────
//
// Two surfaces:
//   - GetLightFxProgram / SetLightFxProgram — raw YAML text.  Kept
//     for power-users + as a fallback when the structured editor
//     hits a shape it doesn't understand.
//   - GetLightFxProgramStructured / SetLightFxProgramStructured —
//     parsed into the DTOs below for the Phase 2 program editor
//     (channels + events + landing bindings).

// ─── Structured program DTOs (mirror lightfx/led_program.h +
//     light_event.h + the on-disk per-program YAML schema). ─────────

// ProgramEventDTO mirrors `LightEvent` with the firmware's snake_case
// YAML keys.  Every parameter is optional with a default — the
// firmware's per-kind decode only reads the fields the kind cares
// about (e.g. `flash_pct` is ignored for `kind: on`).
type ProgramEventDTO struct {
	Kind          string `yaml:"kind"                    json:"kind"`            // "on"|"off"|"flash"|"fade_in"|"fade_out"|"fading"|"beacon"
	DurationMs    uint16 `yaml:"duration_ms,omitempty"   json:"durationMs"`
	CycleMs       uint16 `yaml:"cycle_ms,omitempty"      json:"cycleMs"`
	BrightnessPct uint8  `yaml:"brightness_pct,omitempty" json:"brightnessPct"`
	MinPct        uint8  `yaml:"min_pct,omitempty"        json:"minPct"`
	MaxPct        uint8  `yaml:"max_pct,omitempty"        json:"maxPct"`
	FlashPct      uint8  `yaml:"flash_pct,omitempty"      json:"flashPct"`
}

// TrackDTO — one entry in a v2 program's `tracks:` list.  References
// the LED channel BY NAME — resolves at firmware load time against
// /hubfx.yaml's LedAnimator port labels (`PortMapping.label`), mirror
// of Rule 43's named-input pattern.  `Loop=true` sets the firmware's
// `LightEventFlags::Loop` bit on event[0] (phase-locked repeating
// pattern; period = sum of event durations).  `BrightnessPct` is the
// per-track scale on top of every event's brightness (overrides the
// channel default for this program).
type TrackDTO struct {
	Channel       string            `yaml:"channel"                 json:"channel"`
	BrightnessPct uint8             `yaml:"brightness_pct,omitempty" json:"brightnessPct"`
	Loop          bool              `yaml:"loop,omitempty"          json:"loop"`
	Events        []ProgramEventDTO `yaml:"events"                  json:"events"`
}

// ProgramChannelDTO — LEGACY v1 shape (inline port + events per
// channel).  Studio no longer EMITS this — every Save writes v2
// tracks[].  Still parsed on LOAD so old on-device YAMLs migrate
// transparently (normalizeProgram converts to tracks before the
// JS layer sees the data).  Field tags use `omitempty` so the v2
// emit path doesn't carry empty Channels.
//
// `Name` was the operator label for the channel — becomes the
// migrated track's `Channel` reference (the operator's old name now
// points to the same-named port label in /hubfx.yaml).
type ProgramChannelDTO struct {
	Name          string            `yaml:"name,omitempty"           json:"name"`
	Port          PortRefDTO        `yaml:"port,omitempty"           json:"port"`
	BrightnessPct uint8             `yaml:"brightness_pct,omitempty" json:"brightnessPct"`
	Loop          bool              `yaml:"loop,omitempty"           json:"loop"`
	Events        []ProgramEventDTO `yaml:"events,omitempty"         json:"events"`
}

// ProgramLandingBindingDTO references a landing-light by ID (from
// /landing.yaml) and asserts a target state for the duration of this
// program — "on" deploys (servo open → wait → LEDs on), "off"
// retracts.  Bindings fire when the program becomes active.
type ProgramLandingBindingDTO struct {
	ID    uint8  `yaml:"id"    json:"id"`
	State string `yaml:"state" json:"state"` // "on" | "off"
}

// ProgramDTO — top-level shape of /lightfx/programs/<name>.yaml.
//
// Studio always EMITS v2 (Tracks[] populated, Channels[] empty +
// omitted via `omitempty`).  Studio still READS both shapes:
// normalizeProgram migrates v1 Channels[] → Tracks[] using the
// channel's `name` field as the channel reference, so older on-device
// YAMLs load cleanly into the new model.  Unmigrated channels (no
// name) are dropped with a WARN — operator can rebuild from the
// template library.
type ProgramDTO struct {
	SchemaVersion   int                        `yaml:"schema_version"             json:"schemaVersion"`
	Tracks          []TrackDTO                 `yaml:"tracks,omitempty"           json:"tracks"`
	Channels        []ProgramChannelDTO        `yaml:"channels,omitempty"         json:"channels"`
	LandingBindings []ProgramLandingBindingDTO `yaml:"landing_bindings,omitempty" json:"landingBindings"`
}

func defaultProgram() ProgramDTO {
	return ProgramDTO{
		SchemaVersion:   2,
		Tracks:          []TrackDTO{},
		LandingBindings: []ProgramLandingBindingDTO{},
	}
}

// normalizeProgram converts a freshly-loaded ProgramDTO into the
// canonical v2 shape.  v1 channels[] migrate to tracks[] using each
// channel's `name` field as the channel reference (which becomes the
// /hubfx.yaml port label).  Channels without a name are dropped (the
// operator can re-add via a template or by hand once the
// corresponding port is labelled on the IO tab).  Idempotent — calling
// it on an already-v2 program returns the input untouched.
func normalizeProgram(p ProgramDTO) ProgramDTO {
	out := p
	if out.SchemaVersion == 0 {
		out.SchemaVersion = 1 // implies legacy
	}
	if len(out.Tracks) == 0 && len(out.Channels) > 0 {
		out.Tracks = make([]TrackDTO, 0, len(out.Channels))
		for _, ch := range out.Channels {
			if ch.Name == "" {
				// No operator label means we have no v2 channel
				// reference — drop with a log so the operator notices.
				continue
			}
			out.Tracks = append(out.Tracks, TrackDTO{
				Channel:       ch.Name,
				BrightnessPct: ch.BrightnessPct,
				Loop:          ch.Loop,
				Events:        ch.Events,
			})
		}
	}
	out.Channels = nil // canonical v2 emits tracks only
	if out.Tracks == nil {
		out.Tracks = []TrackDTO{}
	}
	if out.LandingBindings == nil {
		out.LandingBindings = []ProgramLandingBindingDTO{}
	}
	out.SchemaVersion = 2
	return out
}

// GetLightFxProgramStructured loads a per-program YAML and returns
// the typed DTO for the structured editor.  Same path semantics as
// GetLightFxProgram — missing file → defaults so the operator can
// start fresh.  Parse errors surface to the caller (the editor
// falls back to raw-text mode).
func (a *App) GetLightFxProgramStructured(path string) (ProgramDTO, error) {
	defer a.diag.Around("GetLightFxProgramStructured", map[string]any{"path": path})()
	c := a.snapshotClient()
	if c == nil {
		return defaultProgram(), fmt.Errorf("not connected")
	}
	res, err := c.Storage.FileDownloadFrom(path, client.TargetFlash, 5*time.Second)
	if err != nil {
		a.diag.Info("LIGHTFX", "program %s missing (%v) — returning defaults", path, err)
		return defaultProgram(), nil
	}
	prog := ProgramDTO{}
	if err := yaml.Unmarshal(res.Data, &prog); err != nil {
		return defaultProgram(), fmt.Errorf("parse %s: %w", path, err)
	}
	prog = normalizeProgram(prog)
	a.diag.Info("LIGHTFX", "loaded program %s: %d track(s), %d binding(s)",
		path, len(prog.Tracks), len(prog.LandingBindings))
	return prog, nil
}

// SetLightFxProgramStructured serialises the structured program back
// to YAML, uploads, and asks the firmware to reload /lightfx.yaml
// (which re-walks every active program file).  Use this from the
// structured editor; SetLightFxProgram is for the raw-text fallback.
func (a *App) SetLightFxProgramStructured(path string, prog ProgramDTO) error {
	defer a.diag.Around("SetLightFxProgramStructured",
		map[string]any{"path": path, "tracks": len(prog.Tracks)})()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	// Force canonical v2 emit — clears any straggler Channels[] from
	// an in-memory program that came from a v1 load and never went
	// through the editor's normalize.
	prog = normalizeProgram(prog)
	data, err := yaml.Marshal(&prog)
	if err != nil {
		return fmt.Errorf("serialise: %w", err)
	}
	tmp, err := os.CreateTemp("", "lightfx-prog-*.yaml")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	tmp.Close()
	if _, err := c.Storage.FileUpload(tmpPath, client.UploadOptions{
		Path: path, Target: client.TargetFlash, Mode: client.UploadSync,
	}); err != nil {
		return fmt.Errorf("upload %s: %w", path, err)
	}
	if err := c.Config.ReloadPath(lightfxPath); err != nil {
		return fmt.Errorf("reload: %w", err)
	}
	a.diag.Info("LIGHTFX", "saved + reloaded program %s (%d track(s))", path, len(prog.Tracks))
	return nil
}

// ─── Raw-text fallback (Phase 1 surface, kept for power-users) ──────

// GetLightFxProgram downloads one program YAML by full path + returns
// the raw text.  Studio renders it as-is in a preview dialog; future
// per-program editor will parse + structure-edit.
func (a *App) GetLightFxProgram(path string) (string, error) {
	defer a.diag.Around("GetLightFxProgram", map[string]any{"path": path})()
	c := a.snapshotClient()
	if c == nil {
		return "", fmt.Errorf("not connected")
	}
	res, err := c.Storage.FileDownloadFrom(path, client.TargetFlash, 5*time.Second)
	if err != nil {
		return "", fmt.Errorf("download %s: %w", path, err)
	}
	return string(res.Data), nil
}

// SetLightFxProgram uploads raw program YAML text to the given path
// and asks the firmware to reload the master config (so the
// per-program parser re-runs on every active program).
func (a *App) SetLightFxProgram(path, contents string) error {
	defer a.diag.Around("SetLightFxProgram", map[string]any{"path": path, "size": len(contents)})()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	tmp, err := os.CreateTemp("", "lightfx-prog-*.yaml")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if _, err := tmp.Write([]byte(contents)); err != nil {
		tmp.Close()
		return err
	}
	tmp.Close()
	if _, err := c.Storage.FileUpload(tmpPath, client.UploadOptions{
		Path: path, Target: client.TargetFlash, Mode: client.UploadSync,
	}); err != nil {
		return fmt.Errorf("upload %s: %w", path, err)
	}
	// Reload /lightfx.yaml re-walks the programs[] list and re-parses
	// every per-program file, so the edited program takes effect.
	if err := c.Config.ReloadPath(lightfxPath); err != nil {
		return fmt.Errorf("reload: %w", err)
	}
	return nil
}

// ─── Live preview (per-channel / per-program test play) ─────────────
//
// Used by the ProgramEditorDialog's Play / Stop buttons.  Sends a
// LED_QUEUE_LOAD + brightness + LED_START to one PWM port (per
// channel) or every channel at once (per program).  Strictly transient
// — does not touch the device's persisted program list; pressing Save
// in the dialog uploads the per-program YAML which the firmware then
// re-walks at apply time.

// LightEventInput is the Wails-facing event shape.  Field names match
// the ProgramEventDTO above so the TS layer can pass a channel's
// events list straight through without translation.
type LightEventInput struct {
	Kind          string `json:"kind"`           // "on"/"off"/"flash"/"fade_in"/"fade_out"/"fading"/"beacon"
	DurationMs    uint16 `json:"durationMs"`
	CycleMs       uint16 `json:"cycleMs"`
	BrightnessPct uint8  `json:"brightnessPct"`
	MinPct        uint8  `json:"minPct"`
	MaxPct        uint8  `json:"maxPct"`
	FlashPct      uint8  `json:"flashPct"`
}

func lightEventKind(name string) roles.LightEventKind {
	switch name {
	case "off":      return roles.LightEventOff
	case "flash":    return roles.LightEventFlash
	case "fade_in":  return roles.LightEventFadeIn
	case "fade_out": return roles.LightEventFadeOut
	case "fading":   return roles.LightEventFading
	case "beacon":   return roles.LightEventBeacon
	default:         return roles.LightEventOn
	}
}

// PreviewLightChannel queues + starts one channel's event list on a
// PWM port for live testing.  `loop=true` sets the LOOP flag on
// event[0] (phase-locked repeating pattern, period = sum of all
// event durations).  `brightnessPct` is the per-channel master scale
// (0..100), applied on top of each event's brightness.
func (a *App) PreviewLightChannel(
	portIdx uint8, brightnessPct uint8, loop bool, events []LightEventInput,
) error {
	defer a.diag.Around("PreviewLightChannel",
		map[string]any{"port": portIdx, "events": len(events), "loop": loop})()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	if len(events) == 0 {
		return fmt.Errorf("no events to preview")
	}
	wire := make([]roles.LightEvent, len(events))
	for i, e := range events {
		wire[i] = roles.LightEvent{
			Kind:          lightEventKind(e.Kind),
			DurationMs:    e.DurationMs,
			CycleMs:       e.CycleMs,
			BrightnessPct: e.BrightnessPct,
			MinPct:        e.MinPct,
			MaxPct:        e.MaxPct,
			FlashPct:      e.FlashPct,
		}
	}
	if loop {
		wire[0].Flags |= roles.LightEventFlagsLoop
	}
	if err := c.Roles.LedQueueLoad(portIdx, wire); err != nil {
		return fmt.Errorf("queue load: %w", err)
	}
	if err := c.Roles.LedSetBrightness(portIdx, brightnessPct); err != nil {
		return fmt.Errorf("set brightness: %w", err)
	}
	if err := c.Roles.LedStart(portIdx); err != nil {
		return fmt.Errorf("start: %w", err)
	}
	a.diag.Info("LIGHTFX", "PreviewLightChannel port=%d events=%d loop=%v ok", portIdx, len(events), loop)
	return nil
}

// StopLightChannel halts the queue on one PWM port and drives the
// LED off.  Counterpart to PreviewLightChannel; pair them in the
// dialog's per-channel Play / Stop buttons.
func (a *App) StopLightChannel(portIdx uint8) error {
	defer a.diag.Around("StopLightChannel", map[string]any{"port": portIdx})()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	if err := c.Roles.LedStop(portIdx); err != nil {
		return fmt.Errorf("stop: %w", err)
	}
	return nil
}

// LightFxStatusDTO — live LightFX program state on the hub.  ActiveIdx is -1
// when no program is running.  ActiveName is the on-device program name (the
// /lightfx/programs/<name>.yaml id) so Studio can highlight it in the list.
type LightFxStatusDTO struct {
	ActiveIdx           int    `json:"activeIdx"`
	ActiveName          string `json:"activeName"`
	MasterBrightnessPct int    `json:"masterBrightnessPct"`
}

// SelectLightFxProgram tells the firmware to PLAY the named program now (manual
// selection).  The program must already be ON THE DEVICE — Apply first.  When
// an RC selector is configured AND has a valid signal it drives the selection;
// with no signal the selector is idle and this manual choice holds.  This is the
// "in absence of RC, the select/preview button plays it" path.
func (a *App) SelectLightFxProgram(name string) error {
	defer a.diag.Around("SelectLightFxProgram", map[string]any{"name": name})()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	progs, err := c.LightFx.Programs()
	if err != nil {
		return fmt.Errorf("list device programs: %w", err)
	}
	for _, p := range progs {
		if p.Name == name {
			a.diag.Info("LIGHTFX", "select program %q (device idx %d)", name, p.Index)
			return c.LightFx.SelectProgram(p.Index)
		}
	}
	return fmt.Errorf("program %q is not on the device — press Apply to upload it first", name)
}

// ResetLightFxProgram drops the active program (all claimed LEDs off).
func (a *App) ResetLightFxProgram() error {
	defer a.diag.Around("ResetLightFxProgram", nil)()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.LightFx.ResetProgram()
}

// GetLightFxStatus returns the live active-program state (RC-driven or manual),
// resolving the active index to its on-device name so Studio can highlight the
// running program in the list.
func (a *App) GetLightFxStatus() (LightFxStatusDTO, error) {
	c := a.snapshotClient()
	if c == nil {
		return LightFxStatusDTO{ActiveIdx: -1}, fmt.Errorf("not connected")
	}
	st, err := c.LightFx.Status()
	if err != nil {
		return LightFxStatusDTO{ActiveIdx: -1}, err
	}
	out := LightFxStatusDTO{ActiveIdx: -1, MasterBrightnessPct: int(st.MasterBrightnessPct)}
	if st.ActiveIdx != 0xFF {
		out.ActiveIdx = int(st.ActiveIdx)
		if progs, err := c.LightFx.Programs(); err == nil {
			for _, p := range progs {
				if p.Index == st.ActiveIdx {
					out.ActiveName = p.Name
					break
				}
			}
		}
	}
	return out, nil
}

// ListAvailablePrograms walks /lightfx/programs/ on flash and returns
// every .yaml file with its full path + display name.  Lets the
// Studio "add program" picker offer concrete choices instead of
// asking the operator to remember paths.  Missing directory or any
// list error returns an empty list (not-yet-populated case).
//
// `c.Storage.FileList` returns a text blob (one entry per line) —
// the firmware formats it as `<name>  <size>` for files and
// `<name>/` for directories.  We tolerate either format by stripping
// trailing whitespace + size suffix and skipping any line that ends
// in a slash (directory).
func (a *App) ListAvailablePrograms() ([]ProgramFileInfo, error) {
	defer a.diag.Around("ListAvailablePrograms", nil)()
	c := a.snapshotClient()
	if c == nil {
		return nil, fmt.Errorf("not connected")
	}
	blob, err := c.Storage.FileList("/lightfx/programs", client.TargetFlash)
	if err != nil {
		a.diag.Info("LIGHTFX", "no /lightfx/programs/ dir yet (%v)", err)
		return []ProgramFileInfo{}, nil
	}
	out := []ProgramFileInfo{}
	for _, line := range strings.Split(blob, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		// Take the first whitespace-delimited token as the name; the
		// trailing field (if any) is the size.
		name := strings.Fields(line)[0]
		if strings.HasSuffix(name, "/") {
			continue                  // directory
		}
		if !strings.HasSuffix(name, ".yaml") {
			continue
		}
		full := path.Join("/lightfx/programs", name)
		out = append(out, ProgramFileInfo{
			Path: full,
			Name: strings.TrimSuffix(name, ".yaml"),
		})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}
