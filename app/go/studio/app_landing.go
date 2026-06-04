package main

// Landing-light bindings — the Lighting tab's right column.
//
// Landing-light DEFINITIONS live in /landing.yaml on flash; the
// runtime control surface (deploy / retract via SetState + List /
// Status / async phase events) is on the wire (protocol/landing).
// This file wraps both — Get/SetLandingConfig handle the YAML
// round-trip, LandingActivate / LandingDeactivate hit SetState,
// and the LL_PHASE_EVENT broadcast is fanned out to the frontend
// as `landing:phase` events.

import (
	"fmt"
	"os"
	"time"

	"scalefx/client"
	"scalefx/protocol/landing"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
	"gopkg.in/yaml.v3"
)

// ─── DTOs (mirror /landing.yaml + protocol/landing) ─────────────────

// (PortRefDTO is defined in app_gunfx.go and shared across every
// effect-config file.  Landing-light port pickers narrow to either
// `kind: servo` or `kind: pwm` so the operator can't pick the wrong
// physical jack.)

// LandingServoDTO — one entry in /landing.yaml's servos[] block
// (multi-servo support 2026-05-24; groups with one servo still work
// via the legacy `servo:` form on the firmware side).
type LandingServoDTO struct {
	Port PortRefDTO `yaml:"port" json:"port"`
}

// LandingLedDTO — one entry in /landing.yaml's leds[] block.
// BrightnessPct on the FIRST led wins on the firmware side; we still
// carry it per-row so the YAML round-trips faithfully.
type LandingLedDTO struct {
	Port          PortRefDTO `yaml:"port"           json:"port"`
	BrightnessPct uint8      `yaml:"brightness_pct" json:"brightnessPct"`
}

// LandingActivationSource — what triggers deploy / retract for this
// group when the firmware drives it automatically.  Two operator-
// facing modes (the firmware itself just exposes the wire `SetState`
// + the per-program `landing_bindings:` block; Studio offers the
// dropdown so the operator never has to hand-edit YAML to swap
// between channel-driven and program-driven control).
type LandingActivationSource struct {
	Mode string `yaml:"mode"                 json:"mode"`           // "manual" | "input_channel" | "program"
	// input_channel mode
	Input        string `yaml:"input,omitempty"        json:"input"`         // named channel from /hubfx.yaml inputs[]
	ThresholdUs  uint16 `yaml:"threshold_us,omitempty" json:"thresholdUs"`
	HysteresisUs uint16 `yaml:"hysteresis_us,omitempty" json:"hysteresisUs"`
	// program mode
	Program     string `yaml:"program,omitempty"      json:"program"`         // program name from /lightfx.yaml
	WhenProgram string `yaml:"when_program,omitempty" json:"whenProgram"`     // "active" (when running) | "inactive"
}

type LandingLightDTO struct {
	ID            uint8                   `yaml:"id"                       json:"id"`
	Name          string                  `yaml:"name,omitempty"           json:"name"`
	Owner         string                  `yaml:"owner"                    json:"owner"`          // "lightfx" | "gunfx" | "gearcontrol" | "landing-light"
	Servos        []LandingServoDTO       `yaml:"servos,omitempty"         json:"servos"`         // multi-servo form (preferred)
	OpenUs        uint16                  `yaml:"open_us,omitempty"        json:"openUs"`
	CloseUs       uint16                  `yaml:"close_us,omitempty"       json:"closeUs"`
	Leds          []LandingLedDTO         `yaml:"leds"                     json:"leds"`
	// FadeInMs ramps the LEDs 0→brightness over this many ms once the
	// servo has fully deployed (0 = hard on).  Consumed by the firmware's
	// LandingLight::commandLedsOn (a [FadeIn, On] LedAnimator queue).
	FadeInMs      uint16                  `yaml:"fade_in_ms,omitempty"     json:"fadeInMs"`
	Activation    LandingActivationSource `yaml:"activation,omitempty"     json:"activation"`
}

type LandingConfigDTO struct {
	SchemaVersion int               `yaml:"schema_version" json:"schemaVersion"`
	Lights        []LandingLightDTO `yaml:"lights"         json:"lights"`
}

// LandingPhaseDTO mirrors `landing.PhaseChange` for the frontend.
type LandingPhaseDTO struct {
	ID    byte   `json:"id"`
	Phase byte   `json:"phase"`
	Name  string `json:"name"`
}

// LandingStatusDTO mirrors `landing.LightStatus` for the frontend.
type LandingStatusDTO struct {
	ID        byte   `json:"id"`
	Phase     byte   `json:"phase"`
	PhaseName string `json:"phaseName"`
	Owner     byte   `json:"owner"`
}

func defaultLandingConfig() LandingConfigDTO {
	return LandingConfigDTO{SchemaVersion: 1, Lights: []LandingLightDTO{}}
}

// ─── Config I/O (YAML round-trip) ─────────────────────────────────────

const landingPath = "/landing.yaml"

// GetLandingConfig downloads /landing.yaml from flash + parses.  A
// missing file is NOT an error → returns an empty defaults block so
// the UI can author the first group from scratch.
func (a *App) GetLandingConfig() (LandingConfigDTO, error) {
	defer a.diag.Around("GetLandingConfig", nil)()
	c := a.snapshotClient()
	if c == nil {
		return defaultLandingConfig(), fmt.Errorf("not connected")
	}
	res, err := c.Storage.FileDownloadFrom(landingPath, client.TargetFlash, 5*time.Second)
	if err != nil {
		a.diag.Info("LANDING", "no /landing.yaml yet (%v) — returning defaults", err)
		return defaultLandingConfig(), nil
	}
	cfg := defaultLandingConfig()
	if err := yaml.Unmarshal(res.Data, &cfg); err != nil {
		return defaultLandingConfig(), fmt.Errorf("parse landing.yaml: %w", err)
	}
	if cfg.SchemaVersion == 0 {
		cfg.SchemaVersion = 1
	}
	if cfg.Lights == nil {
		cfg.Lights = []LandingLightDTO{}
	}
	a.diag.Info("LANDING", "loaded /landing.yaml: %d light(s)", len(cfg.Lights))
	return cfg, nil
}

// SetLandingConfig serialises + uploads + reloads.  Triggers the
// firmware's applyLandingConfig translator via CONFIG_RELOAD; the new
// def table takes effect on the next loop pass.
func (a *App) SetLandingConfig(cfg LandingConfigDTO) error {
	defer a.diag.Around("SetLandingConfig", nil)()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	if cfg.SchemaVersion == 0 {
		cfg.SchemaVersion = 1
	}
	// The firmware's RC auto-deploy driver keys purely off a non-empty
	// `activation.input`.  Clear it for any mode that isn't input_channel
	// so a "manual" / "program" group never accidentally channel-activates
	// (the `mode` / `program` fields are Studio-only metadata — program
	// attachment lives in the program YAML's `landing_bindings:`).
	for i := range cfg.Lights {
		if cfg.Lights[i].Activation.Mode != "input_channel" {
			cfg.Lights[i].Activation.Input = ""
		}
	}
	data, err := yaml.Marshal(&cfg)
	if err != nil {
		return fmt.Errorf("serialise: %w", err)
	}
	tmp, err := os.CreateTemp("", "landing-*.yaml")
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
		Path: landingPath, Target: client.TargetFlash, Mode: client.UploadSync,
	}); err != nil {
		return fmt.Errorf("upload landing.yaml: %w", err)
	}
	if err := c.Config.ReloadPath(landingPath); err != nil {
		return fmt.Errorf("reload: %w", err)
	}
	a.diag.Info("LANDING", "saved + reloaded /landing.yaml (%d light(s))", len(cfg.Lights))
	return nil
}

// ─── Runtime control ──────────────────────────────────────────────────

// LandingActivate sends LL_SET_STATE(id, ON) — equivalent to the
// owner effect calling `landingLight.deploy()`.  Studio bypasses the
// owner check by calling through the master's special-case path.
func (a *App) LandingActivate(id uint8) error {
	defer a.diag.Around("LandingActivate", map[string]any{"id": id})()
	a.diag.Info("LANDING", "Activate id=%d", id)
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	if err := c.LandingLights.On(id); err != nil {
		return fmt.Errorf("landing activate %d: %w", id, err)
	}
	return nil
}

// LandingDeactivate sends LL_SET_STATE(id, OFF).
func (a *App) LandingDeactivate(id uint8) error {
	defer a.diag.Around("LandingDeactivate", map[string]any{"id": id})()
	a.diag.Info("LANDING", "Deactivate id=%d", id)
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	if err := c.LandingLights.Off(id); err != nil {
		return fmt.Errorf("landing deactivate %d: %w", id, err)
	}
	return nil
}

// LandingStatus returns one entry per registered landing light —
// current phase + owner.  Polled when the panel mounts; live updates
// flow through the `landing:phase` event stream below.
func (a *App) LandingStatus() ([]LandingStatusDTO, error) {
	defer a.diag.Around("LandingStatus", nil)()
	c := a.snapshotClient()
	if c == nil {
		return nil, fmt.Errorf("not connected")
	}
	st, err := c.LandingLights.Status()
	if err != nil {
		return nil, err
	}
	out := make([]LandingStatusDTO, len(st))
	for i, s := range st {
		out[i] = LandingStatusDTO{
			ID:        s.ID,
			Phase:     s.Phase,
			PhaseName: landing.PhaseName(s.Phase),
			// Owner isn't on the wire LL_STATUS_RESP today; the
			// frontend infers it from /landing.yaml's `owner:` field.
			Owner: 0,
		}
	}
	return out, nil
}

// ─── Async event bridge ───────────────────────────────────────────────

// installLandingStream wires the wire-side phase event into the
// frontend `landing:phase` event so the Lighting tab can update its
// per-group state pill live (Retracted / Deploying / Deployed / …).
func (a *App) installLandingStream() {
	if a.c == nil {
		return
	}
	a.c.Events.OnLandingLightPhase(func(ev landingPhaseChange) {
		if a.ctx == nil {
			return
		}
		wailsRT.EventsEmit(a.ctx, "landing:phase", LandingPhaseDTO{
			ID:    ev.ID,
			Phase: ev.Phase,
			Name:  landing.PhaseName(ev.Phase),
		})
	})
}

// Type alias to keep the OnLandingLightPhase signature satisfied
// without importing protocol/landing into every file that touches it.
type landingPhaseChange = client.LandingPhaseChange
