package main

// GearControl bindings — the Gear / Undercarriage tab (instructions/29
// §3, firmware landed 2026-06-07).
//
// Config DEFINITIONS live in /gearcontrol.yaml on flash (schema v2:
// coord + per-gear motor / doors / door_mode / door_delay_ms /
// close_policy).  The runtime control surface (deploy / retract / stop /
// all / reset + status + async phase events) is on the wire
// (protocol/gear).  This file wraps both — LoadGearConfig /
// SaveGearConfig round-trip the YAML, GearDeploy / GearRetract / … hit
// the client, and the GEAR_PHASE_EVENT broadcast is fanned out to the
// frontend as a `gear:phase` event so each channel shows live state.

import (
	"fmt"
	"os"
	"time"

	"scalefx/client"
	"scalefx/protocol/gear"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
	"gopkg.in/yaml.v3"
)

// ─── DTOs (mirror /gearcontrol.yaml v2 + protocol/gear) ───────────────
//
// PortRefDTO is defined in app_gunfx.go and shared across every effect-
// config file.  Gear motors narrow to `kind: hbridge` (BiDcMotor role);
// door servos to `kind: servo` (ServoActuator role).

// GearDoorDTO — one entry in a gear's doors[] block.  open / close are
// NORMALISED positions [0..10000] (Rule: servo intent is normalised);
// the servo role honours its own REV flag + calibration.
type GearDoorDTO struct {
	Port  PortRefDTO `yaml:"port"  json:"port"`
	Open  uint16     `yaml:"open"  json:"open"`
	Close uint16     `yaml:"close" json:"close"`
}

// GearChannelDTO mirrors the firmware GearDef.  door_mode maps onto the
// firmware DoorMode enum (sync | delay | sequence | single | none);
// close_policy onto ClosePolicy (both | first | none).
type GearChannelDTO struct {
	ID          uint8         `yaml:"id"                       json:"id"`
	Name        string        `yaml:"name,omitempty"           json:"name"`
	Motor       PortRefDTO    `yaml:"motor"                    json:"motor"`
	DeployDuty  int16         `yaml:"deploy_duty"              json:"deployDuty"`
	RetractDuty int16         `yaml:"retract_duty"             json:"retractDuty"`
	TimeoutMs   uint32        `yaml:"timeout_ms"               json:"timeoutMs"`
	Doors       []GearDoorDTO `yaml:"doors,omitempty"          json:"doors"`
	DoorMode    string        `yaml:"door_mode"                json:"doorMode"`     // sync | delay | sequence
	DoorDelayMs uint16        `yaml:"door_delay_ms,omitempty"  json:"doorDelayMs"`  // delay mode only
	ClosePolicy string        `yaml:"close_policy"             json:"closePolicy"`  // both | first | none
}

// GearInputDTO — the OPTIONAL one-channel RC up/down binding (`input:`
// block).  Name is a NAMED channel from /hubfx.yaml inputs[] (Rule 43);
// above the threshold = retract (gear up), Invert flips.  Empty name =
// manual-only (Studio / CLI).
type GearInputDTO struct {
	Name         string `yaml:"name,omitempty"          json:"name"`
	ThresholdUs  uint16 `yaml:"threshold_us,omitempty"  json:"thresholdUs"`
	HysteresisUs uint16 `yaml:"hysteresis_us,omitempty" json:"hysteresisUs"`
	Invert       bool   `yaml:"invert,omitempty"        json:"invert"`
}

// GearSoundsDTO — the OPTIONAL transit sounds (`sounds:` block).  The
// matching WAV loops on the dedicated Gear mixer channel while any gear
// is moving in that direction; empty path = silent.
type GearSoundsDTO struct {
	Deploy     string `yaml:"deploy,omitempty"      json:"deploy"`
	Retract    string `yaml:"retract,omitempty"     json:"retract"`
	OutputMask uint8  `yaml:"output_mask,omitempty" json:"outputMask"` // 1=L 2=R 3=both
}

// GearConfig mirrors /gearcontrol.yaml (schema v2 + the append-only
// optional `input:` / `sounds:` blocks, 2026-06-11).
type GearConfig struct {
	SchemaVersion int              `yaml:"schema_version"    json:"schemaVersion"`
	Enabled       bool             `yaml:"enabled"           json:"enabled"`
	Coord         string           `yaml:"coord"             json:"coord"` // independent | door_sync | full_sync | sequenced
	Input         GearInputDTO     `yaml:"input,omitempty"   json:"input"`
	Sounds        GearSoundsDTO    `yaml:"sounds,omitempty"  json:"sounds"`
	Gears         []GearChannelDTO `yaml:"gears"             json:"gears"`
}

// GearStatusEntry is one row of GearStatus() — the live per-channel
// phase + sub-phase the panel renders in its status pill.
type GearStatusEntry struct {
	ID           uint8  `json:"id"`
	Phase        byte   `json:"phase"`
	PhaseName    string `json:"phaseName"`
	SubPhase     byte   `json:"subPhase"`
	SubPhaseName string `json:"subPhaseName"`
}

// GearPhaseDTO mirrors gear.PhaseChange for the `gear:phase` event.
type GearPhaseDTO struct {
	ID           byte   `json:"id"`
	Phase        byte   `json:"phase"`
	PhaseName    string `json:"phaseName"`
	SubPhase     byte   `json:"subPhase"`
	SubPhaseName string `json:"subPhaseName"`
}

func defaultGearConfig() GearConfig {
	return GearConfig{
		SchemaVersion: 2,
		Enabled:       false,
		Coord:         "independent",
		Input:         GearInputDTO{ThresholdUs: 1500, HysteresisUs: 50},
		Sounds:        GearSoundsDTO{OutputMask: 3},
		Gears:         []GearChannelDTO{},
	}
}

// ─── Config I/O (YAML round-trip) ─────────────────────────────────────

const gearPath = "/gearcontrol.yaml"

// LoadGearConfig downloads /gearcontrol.yaml from flash + parses.  A
// missing file is NOT an error → returns an empty-but-valid defaults
// block so the panel can author the first channel from scratch.
//
// The frontend gates this call on controllerType === 'hubfx'; nothing is
// gated here.
func (a *App) LoadGearConfig() (GearConfig, error) {
	defer a.diag.Around("LoadGearConfig", nil)()
	a.diag.Info("GEAR", "LoadGearConfig: downloading %s …", gearPath)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GEAR", "LoadGearConfig: not connected")
		return defaultGearConfig(), fmt.Errorf("not connected")
	}
	res, err := c.Storage.FileDownloadFrom(gearPath, client.TargetFlash, 5*time.Second)
	if err != nil {
		a.diag.Info("GEAR", "LoadGearConfig: %s not present (%v) — returning defaults", gearPath, err)
		return defaultGearConfig(), nil
	}
	cfg := defaultGearConfig()
	if err := yaml.Unmarshal(res.Data, &cfg); err != nil {
		a.diag.Error("GEAR", "LoadGearConfig: yaml.Unmarshal: %v", err)
		return defaultGearConfig(), fmt.Errorf("parse gearcontrol.yaml: %w", err)
	}
	if cfg.SchemaVersion == 0 {
		cfg.SchemaVersion = 2
	}
	if cfg.Coord == "" {
		cfg.Coord = "independent"
	}
	if cfg.Gears == nil {
		cfg.Gears = []GearChannelDTO{}
	}
	// Normalise the optional input/sounds blocks so the panel always sees
	// sensible values (a pre-2026-06 file has neither block).
	if cfg.Input.ThresholdUs == 0 {
		cfg.Input.ThresholdUs = 1500
	}
	if cfg.Input.HysteresisUs == 0 {
		cfg.Input.HysteresisUs = 50
	}
	if cfg.Sounds.OutputMask == 0 || cfg.Sounds.OutputMask > 3 {
		cfg.Sounds.OutputMask = 3
	}
	// Canonicalise hub-identity port-ref GUIDs → "" (instructions/31) so the
	// output-port dropdowns resolve against the canonical device model.
	folds := 0
	foldRef := func(label string, p *PortRefDTO) {
		if c, was := a.canonGuid(p.Guid); was {
			a.diag.Info("GEAR", "[gap] %s ref guid=%q kind=%q idx=%d → canon \"\" (hub=%q)",
				label, p.Guid, p.Kind, p.Idx, a.id.GUID)
			p.Guid = c
			folds++
		}
	}
	for i := range cfg.Gears {
		g := &cfg.Gears[i]
		foldRef(fmt.Sprintf("gear[%d].motor", i), &g.Motor)
		for j := range g.Doors {
			foldRef(fmt.Sprintf("gear[%d].door[%d]", i, j), &g.Doors[j].Port)
		}
	}
	a.diag.Info("GEAR", "LoadGearConfig: ok — enabled=%v coord=%s gears=%d; folded %d hub-identity port ref(s) → \"\" (hub=%q)",
		cfg.Enabled, cfg.Coord, len(cfg.Gears), folds, a.id.GUID)
	return cfg, nil
}

// SaveGearConfig serialises the config to canonical block YAML (Rule 27),
// uploads it to /gearcontrol.yaml on flash, and asks the hub to reload
// that single store.  The reload runs `applyGearControlConfig` on the
// firmware side, so this is what makes a draft take effect.
func (a *App) SaveGearConfig(cfg GearConfig) error {
	defer a.diag.Around("SaveGearConfig", nil)()
	a.diag.Info("GEAR", "SaveGearConfig: enabled=%v coord=%s gears=%d → %s",
		cfg.Enabled, cfg.Coord, len(cfg.Gears), gearPath)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GEAR", "SaveGearConfig: not connected")
		return fmt.Errorf("not connected")
	}
	if cfg.SchemaVersion == 0 {
		cfg.SchemaVersion = 2
	}
	if cfg.Coord == "" {
		cfg.Coord = "independent"
	}
	// gopkg.in/yaml.v3 emits indented block sequences by default (Rule 27);
	// never flow/compact form.  A nil Doors slice marshals to `doors: []`
	// which the firmware parser accepts (empty sequence → numDoors 0).
	data, err := yaml.Marshal(&cfg)
	if err != nil {
		a.diag.Error("GEAR", "SaveGearConfig: yaml.Marshal: %v", err)
		return fmt.Errorf("serialise: %w", err)
	}
	tmp, err := os.CreateTemp("", "gearcontrol-*.yaml")
	if err != nil {
		a.diag.Error("GEAR", "SaveGearConfig: tempfile: %v", err)
		return err
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		a.diag.Error("GEAR", "SaveGearConfig: write tempfile: %v", err)
		return err
	}
	tmp.Close()

	upStart := time.Now()
	if _, err := c.Storage.FileUpload(tmpPath, client.UploadOptions{
		Path: gearPath, Target: client.TargetFlash, Mode: client.UploadSync,
	}); err != nil {
		a.diag.Error("GEAR", "SaveGearConfig: upload %s: %v", gearPath, err)
		return fmt.Errorf("upload gearcontrol.yaml: %w", err)
	}
	a.diag.Info("GEAR", "SaveGearConfig: uploaded %d bytes in %d ms",
		len(data), time.Since(upStart).Milliseconds())

	if err := c.Config.ReloadPath(gearPath); err != nil {
		a.diag.Error("GEAR", "SaveGearConfig: reload %s NACKed: %v — file is on flash but firmware did NOT re-apply",
			gearPath, err)
		return fmt.Errorf("firmware refused reload of %s: %w (file is saved on flash but not applied)", gearPath, err)
	}
	a.diag.Info("GEAR", "SaveGearConfig: ✓ uploaded + reloaded %s — firmware re-applied", gearPath)
	return nil
}

// ─── Runtime control (wire surface — protocol/gear) ───────────────────

func (a *App) logGearErr(method string, fields map[string]any, err error) error {
	if err == nil {
		return nil
	}
	if fields == nil {
		fields = map[string]any{}
	}
	fields["error"] = err.Error()
	a.diag.With(LvlError, "GEAR", method+" failed", fields)
	return err
}

// GearDeploy lowers gear `id`.  Returns GEAR_IN_ERROR_STATE if the unit
// is in an error state — call GearReset() to clear it first.
func (a *App) GearDeploy(id uint8) error {
	defer a.diag.Around("GearDeploy", map[string]any{"id": id})()
	a.diag.Info("GEAR", "GearDeploy id=%d", id)
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearDeploy", map[string]any{"id": id}, c.Gear.Deploy(id))
}

// GearRetract raises gear `id`.
func (a *App) GearRetract(id uint8) error {
	defer a.diag.Around("GearRetract", map[string]any{"id": id})()
	a.diag.Info("GEAR", "GearRetract id=%d", id)
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearRetract", map[string]any{"id": id}, c.Gear.Retract(id))
}

// GearStop halts gear `id` mid-motion (motor brake).  Does NOT clear an
// error — use GearReset() for that.
func (a *App) GearStop(id uint8) error {
	defer a.diag.Around("GearStop", map[string]any{"id": id})()
	a.diag.Info("GEAR", "GearStop id=%d", id)
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearStop", map[string]any{"id": id}, c.Gear.Stop(id))
}

// GearAll applies one action to EVERY configured channel (decision #4 —
// the fleet trigger).  action: 0 = stop, 1 = deploy, 2 = retract.
func (a *App) GearAll(action int) error {
	defer a.diag.Around("GearAll", map[string]any{"action": action})()
	a.diag.Info("GEAR", "GearAll action=%d (%s)", action, gear.AllActionName(byte(action)))
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearAll", map[string]any{"action": action}, c.Gear.All(byte(action)))
}

// GearReset clears gear `id`'s error state (ERROR → Retracted) so it
// accepts deploy/retract again.
func (a *App) GearReset(id uint8) error {
	defer a.diag.Around("GearReset", map[string]any{"id": id})()
	a.diag.Info("GEAR", "GearReset id=%d", id)
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearReset", map[string]any{"id": id}, c.Gear.Reset(id))
}

// GearStatus returns the current phase + sub-phase for every configured
// channel.  Polled when the panel mounts (live updates flow through the
// `gear:phase` event stream below) so the pill self-heals after a dropped
// async event (Rule 53 lossy queue).
func (a *App) GearStatus() ([]GearStatusEntry, error) {
	defer a.diag.Around("GearStatus", nil)()
	c := a.snapshotClient()
	if c == nil {
		return nil, fmt.Errorf("not connected")
	}
	in, err := c.Gear.Status()
	if err != nil {
		return nil, a.logGearErr("GearStatus", nil, err)
	}
	out := make([]GearStatusEntry, len(in))
	for i, s := range in {
		out[i] = GearStatusEntry{
			ID:           s.ID,
			Phase:        s.Phase,
			PhaseName:    gear.PhaseName(s.Phase),
			SubPhase:     s.SubPhase,
			SubPhaseName: gear.SubPhaseName(s.SubPhase),
		}
	}
	a.diag.Debug("GEAR", "GearStatus: %d channel(s)", len(out))
	return out, nil
}

// ─── Async event bridge ───────────────────────────────────────────────

// installGearStream wires the wire-side GEAR_PHASE_EVENT into the
// frontend `gear:phase` event so each channel card updates its live
// phase + sub-phase pill without polling.  Called once per connect.
func (a *App) installGearStream() {
	if a.c == nil {
		return
	}
	a.c.Events.OnGearPhase(func(ev gear.PhaseChange) {
		if a.ctx == nil {
			return
		}
		wailsRT.EventsEmit(a.ctx, "gear:phase", GearPhaseDTO{
			ID:           ev.ID,
			Phase:        ev.Phase,
			PhaseName:    gear.PhaseName(ev.Phase),
			SubPhase:     ev.SubPhase,
			SubPhaseName: gear.SubPhaseName(ev.SubPhase),
		})
	})
}
