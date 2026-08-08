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

// GearDoorDTO — one entry in a gear's doors[] block.  A door drives its
// servo to the calibrated MAX end on open and the MIN end on close (the
// servo's own REV flag flips direction) — the travel lives in the servo
// calibration, NOT here (same parametrisation as a landing-light servo).
type GearDoorDTO struct {
	Port PortRefDTO `yaml:"port" json:"port"`
	// ABSOLUTE µs positions (2.46.0 — set with the endpoints widget).
	// 0xFFFF/0 = "calibrated end" defaults (the servo role clamps).
	OpenUs  uint16 `yaml:"open_us,omitempty"  json:"openUs"`
	CloseUs uint16 `yaml:"close_us,omitempty" json:"closeUs"`
}

// GearGuardDTO — persisted stall-guard calibration for a strut's motor
// (Studio's "Save to strut").  Mirrors the firmware GearDef guard fields +
// BIMOTOR_SET_GUARD; the gear effect pushes it to the motor before each seek
// so a saved calibration drives REAL deploy/retract (not the role default).
// `mode` = "live" (LiveRatio) | "fixed".  ratio_x100 = ratio×100 (250 = 2.5×).
type GearGuardDTO struct {
	Mode        string `yaml:"mode"          json:"mode"`
	RatioX100   uint16 `yaml:"ratio_x100"    json:"ratioX100"`
	SampleMs    uint16 `yaml:"sample_ms"     json:"sampleMs"`
	WindowMs    uint16 `yaml:"window_ms"     json:"windowMs"`
	ThresholdMa uint16 `yaml:"threshold_ma"  json:"thresholdMa"`
	CeilingMa   uint16 `yaml:"ceiling_ma"    json:"ceilingMa"`
}

// GearChannelDTO mirrors the firmware GearDef.  door_mode maps onto the
// firmware DoorMode enum (sync | delay | sequence | single | none);
// close_policy onto ClosePolicy (both | first | none).
type GearChannelDTO struct {
	ID      uint8      `yaml:"id"             json:"id"`
	Name    string     `yaml:"name,omitempty" json:"name"`
	Motor   PortRefDTO `yaml:"motor,omitempty" json:"motor"`
	// Strut drive customization (2.45.0): in `servo` strut mode the strut is
	// an integrated retract controller on its OWN servo channel; travel_ms is
	// the FIXED stroke duration (black box — doors wait for it).
	StrutServo PortRefDTO `yaml:"strut_servo,omitempty" json:"strutServo"`
	TravelMs   uint32     `yaml:"travel_ms,omitempty"   json:"travelMs"`
	// ABSOLUTE deploy/retract pulses for the per-strut servo (2.46.0).
	DeployUs   uint16     `yaml:"deploy_us,omitempty"   json:"deployUs"`
	RetractUs  uint16     `yaml:"retract_us,omitempty"  json:"retractUs"`
	// Voltage-first drive (2.44.0): deploy/retract seek at full scale and
	// the motor role's cap delivers exactly MotorVoltageMv at the motor on
	// any pack.  Reverse flips the deploy direction.  The raw
	// deploy_duty/retract_duty keys are RETIRED and never written.
	Reverse        bool   `yaml:"reverse"          json:"reverse"`
	TimeoutMs      uint32 `yaml:"timeout_ms"       json:"timeoutMs"`
	MotorVoltageMv uint16 `yaml:"motor_voltage_mv" json:"motorVoltageMv"`
	Guard       *GearGuardDTO `yaml:"guard,omitempty"          json:"guard,omitempty"` // persisted stall guard (append-only)
	Doors       []GearDoorDTO `yaml:"doors,omitempty"          json:"doors"`
	DoorMode    string        `yaml:"door_mode"                json:"doorMode"`     // sync | delay | sequence
	DoorDelayMs uint16        `yaml:"door_delay_ms,omitempty"  json:"doorDelayMs"`  // delay mode only
	ClosePolicy string        `yaml:"close_policy"             json:"closePolicy"`  // both | first | none
}

// GearInputDTO — the OPTIONAL one-channel RC up/down binding (`input:`
// block).  Name is a NAMED channel from /hubfx.yaml inputs[] (Rule 43);
// above the threshold (switch ON) = deploy (gear down), Invert flips.  Empty name =
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

// GearStrutSharedDTO — the `strut_shared:` block (servo_shared mode): ONE
// servo channel drives the whole undercarriage; travel_ms = fixed stroke.
type GearStrutSharedDTO struct {
	Port     PortRefDTO `yaml:"port"      json:"port"`
	TravelMs uint32     `yaml:"travel_ms" json:"travelMs"`
	// ABSOLUTE deploy/retract pulses (2.46.0) — one pair for the ONE channel.
	DeployUs  uint16 `yaml:"deploy_us,omitempty"  json:"deployUs"`
	RetractUs uint16 `yaml:"retract_us,omitempty" json:"retractUs"`
}

// GearConfig mirrors /gearcontrol.yaml (schema v2 + the append-only
// optional `input:` / `sounds:` blocks, 2026-06-11; `strut_mode` /
// `strut_shared` 2.45.0).
type GearConfig struct {
	SchemaVersion int              `yaml:"schema_version"    json:"schemaVersion"`
	Enabled       bool             `yaml:"enabled"           json:"enabled"`
	Coord         string           `yaml:"coord"             json:"coord"` // independent | door_sync | full_sync | sequenced
	// How the strut stage moves: hbridge (DC motor per strut) | servo (one
	// PWM channel per strut) | servo_shared (one channel for ALL struts).
	StrutMode   string              `yaml:"strut_mode,omitempty"   json:"strutMode"`
	StrutShared *GearStrutSharedDTO `yaml:"strut_shared,omitempty" json:"strutShared,omitempty"`
	// item 6: emergency-deploy the gear when an input link drops (the
	// InputDispatcher's CONNECTION DOWN signal).
	DeployOnConnectionLoss bool             `yaml:"deploy_on_connection_loss,omitempty" json:"deployOnConnectionLoss"`
	Input                  GearInputDTO     `yaml:"input,omitempty"   json:"input"`
	Sounds                 GearSoundsDTO    `yaml:"sounds,omitempty"  json:"sounds"`
	Gears                  []GearChannelDTO `yaml:"gears"             json:"gears"`
}

// GearStatusEntry is one row of GearStatus() — the live per-channel
// phase + sub-phase the panel renders in its status pill.  ErrReason +
// ErrReasonTag carry the fault behind an Error phase (timeout / no motor).
type GearStatusEntry struct {
	ID           uint8  `json:"id"`
	Phase        byte   `json:"phase"`
	PhaseName    string `json:"phaseName"`
	SubPhase     byte   `json:"subPhase"`
	SubPhaseName string `json:"subPhaseName"`
	ErrReason    byte   `json:"errReason"`
	ErrReasonTag string `json:"errReasonTag"`
	DoorsOpen    bool   `json:"doorsOpen"`  // every door at its open end (manual-control gate)
	StrutState   byte   `json:"strutState"` // GearStrutState (Up/Out/Moving/Unknown)
	StrutName    string `json:"strutName"`
}

// GearPhaseDTO mirrors gear.PhaseChange for the `gear:phase` event.
type GearPhaseDTO struct {
	ID           byte   `json:"id"`
	Phase        byte   `json:"phase"`
	PhaseName    string `json:"phaseName"`
	SubPhase     byte   `json:"subPhase"`
	SubPhaseName string `json:"subPhaseName"`
	ErrReason    byte   `json:"errReason"`
	ErrReasonTag string `json:"errReasonTag"`
	DoorsOpen    bool   `json:"doorsOpen"`
	StrutState   byte   `json:"strutState"`
	StrutName    string `json:"strutName"`
}

func defaultGearConfig() GearConfig {
	return GearConfig{
		SchemaVersion: 2,
		Enabled:       false,
		Coord:         "independent",
		StrutMode:     "hbridge",
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
	if cfg.StrutMode == "" {
		cfg.StrutMode = "hbridge"
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
		foldRef(fmt.Sprintf("gear[%d].strutServo", i), &g.StrutServo)
		// yaml-absent open/deploy µs load as 0, but 0 means "close/retract
		// end" — normalise the OPEN-side zeros to the 0xFFFF "calibrated
		// end" sentinel so the UI and the firmware agree (a literal 0 µs
		// pulse is not a real servo position).
		if g.DeployUs == 0 {
			g.DeployUs = 0xFFFF
		}
		for j := range g.Doors {
			foldRef(fmt.Sprintf("gear[%d].door[%d]", i, j), &g.Doors[j].Port)
			if g.Doors[j].OpenUs == 0 {
				g.Doors[j].OpenUs = 0xFFFF
			}
		}
	}
	if cfg.StrutShared != nil && cfg.StrutShared.DeployUs == 0 {
		cfg.StrutShared.DeployUs = 0xFFFF
	}
	if cfg.StrutShared != nil {
		foldRef("strutShared.port", &cfg.StrutShared.Port)
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

// GearEStop emergency-holds gear `id` (brake + freeze in place → HELD).
func (a *App) GearEStop(id uint8) error {
	defer a.diag.Around("GearEStop", map[string]any{"id": id})()
	a.diag.Info("GEAR", "GearEStop id=%d", id)
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearEStop", map[string]any{"id": id}, c.Gear.EStop(id))
}

// GearEStopAll emergency-holds EVERY strut in place (brake + freeze → HELD).
func (a *App) GearEStopAll() error {
	defer a.diag.Around("GearEStopAll", nil)()
	a.diag.Info("GEAR", "GearEStopAll")
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearEStopAll", nil, c.Gear.EStopAll())
}

// GearStep advances gear `id` ONE leg toward `target` (0 = up, 1 = down)
// then parks at the next boundary — the manual "do one item in the
// sequence" command.  Auto-clears a prior error first.
func (a *App) GearStep(id uint8, target uint8) error {
	defer a.diag.Around("GearStep", map[string]any{"id": id, "target": target})()
	a.diag.Info("GEAR", "GearStep id=%d target=%d", id, target)
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearStep", map[string]any{"id": id, "target": target}, c.Gear.Step(id, target))
}

// ── Manual / maintenance: doors + strut, per-leg + fleet ──────────────────
// The firmware enforces the interlock and NACKs a violation (the GUI also gates
// the buttons to mirror it).  open/down booleans pass straight to the wire.

func (a *App) GearDoors(id uint8, open bool) error {
	defer a.diag.Around("GearDoors", map[string]any{"id": id, "open": open})()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearDoors", map[string]any{"id": id, "open": open}, c.Gear.Doors(id, open))
}

func (a *App) GearMoveStrut(id uint8, down bool) error {
	defer a.diag.Around("GearMoveStrut", map[string]any{"id": id, "down": down})()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearMoveStrut", map[string]any{"id": id, "down": down}, c.Gear.MoveStrut(id, down))
}

func (a *App) GearDoorsAll(open bool) error {
	defer a.diag.Around("GearDoorsAll", map[string]any{"open": open})()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearDoorsAll", map[string]any{"open": open}, c.Gear.DoorsAll(open))
}

func (a *App) GearMoveStrutAll(down bool) error {
	defer a.diag.Around("GearMoveStrutAll", map[string]any{"down": down})()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return a.logGearErr("GearMoveStrutAll", map[string]any{"down": down}, c.Gear.MoveStrutAll(down))
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
			ErrReason:    s.ErrReason,
			ErrReasonTag: gear.ErrorReasonTag(s.ErrReason),
			DoorsOpen:    s.DoorsOpen,
			StrutState:   s.StrutState,
			StrutName:    gear.StrutStateName(s.StrutState),
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
			ErrReason:    ev.ErrReason,
			ErrReasonTag: gear.ErrorReasonTag(ev.ErrReason),
			DoorsOpen:    ev.DoorsOpen,
			StrutState:   ev.StrutState,
			StrutName:    gear.StrutStateName(ev.StrutState),
		})
	})
}
