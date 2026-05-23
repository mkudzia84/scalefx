package main

// Engine bindings — the Effects tab's EngineFx panel.
//
// EngineFx config lives in /enginefx.yaml on flash; the runtime control
// surface (Start, Stop, Status, async state events) is on the wire.  This
// file wraps both: GetEngineConfig downloads + parses the YAML,
// SetEngineConfig serialises + uploads + reloads, and the state stream is
// forwarded to the frontend as `engine:state` events.

import (
	"fmt"
	"os"
	"time"

	"scalefx/client"
	"scalefx/protocol/enginefx"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
	"gopkg.in/yaml.v3"
)

// ─── DTO (mirrors enginefx_config.h schema) ───────────────────────────

type EngineToggle struct {
	Input        string `yaml:"input,omitempty"  json:"input"`
	ThresholdUs  uint16 `yaml:"threshold_us"     json:"thresholdUs"`
	HysteresisUs uint16 `yaml:"hysteresis_us"    json:"hysteresisUs"`
	Failsafe     string `yaml:"failsafe"         json:"failsafe"`
}

type EngineTransitions struct {
	StartingOffsetMs uint32 `yaml:"starting_offset_ms" json:"startingOffsetMs"`
	StoppingOffsetMs uint32 `yaml:"stopping_offset_ms" json:"stoppingOffsetMs"`
	StartFadeInMs    uint16 `yaml:"start_fade_in_ms"   json:"startFadeInMs"`
	StopFadeOutMs    uint16 `yaml:"stop_fade_out_ms"   json:"stopFadeOutMs"`
}

type EngineSounds struct {
	Starting    string            `yaml:"starting,omitempty" json:"starting"`
	Running     string            `yaml:"running,omitempty"  json:"running"`
	Stopping    string            `yaml:"stopping,omitempty" json:"stopping"`
	Transitions EngineTransitions `yaml:"transitions"        json:"transitions"`
}

type EngineConfig struct {
	SchemaVersion int          `yaml:"schema_version" json:"schemaVersion"`
	Enabled       bool         `yaml:"enabled"        json:"enabled"`
	Type          string       `yaml:"type"           json:"type"`
	Output        string       `yaml:"output"         json:"output"`
	Toggle        EngineToggle `yaml:"toggle"         json:"toggle"`
	Sounds        EngineSounds `yaml:"sounds"         json:"sounds"`
}

// EngineStatusDTO mirrors enginefx.Status for the frontend.
type EngineStatusDTO struct {
	State     byte   `json:"state"`     // 0 Stopped · 1 Starting · 2 Running · 3 Stopping
	StateName string `json:"stateName"`
	Engaged   bool   `json:"engaged"`   // RC toggle currently held over threshold
}

func defaultEngineConfig() EngineConfig {
	return EngineConfig{
		SchemaVersion: 1,
		Enabled:       false,
		Type:          "turbine",
		Output:        "both",
		Toggle:        EngineToggle{ThresholdUs: 1500, HysteresisUs: 50, Failsafe: "force_low"},
	}
}

// ─── Config I/O (YAML round-trip) ─────────────────────────────────────

const enginePath = "/enginefx.yaml"

// GetEngineConfig downloads /enginefx.yaml from flash and returns the
// parsed config.  A missing file is NOT an error — returns sensible
// defaults so the UI can be filled out fresh.
func (a *App) GetEngineConfig() (EngineConfig, error) {
	defer a.diag.Around("GetEngineConfig", nil)()
	c := a.snapshotClient()
	if c == nil {
		return defaultEngineConfig(), fmt.Errorf("not connected")
	}
	res, err := c.Storage.FileDownloadFrom(enginePath, client.TargetFlash, 5*time.Second)
	if err != nil {
		// File-not-found or any download error → defaults (first-time use).
		a.diag.Info("ENGINE", "no /enginefx.yaml yet (%v) — returning defaults", err)
		return defaultEngineConfig(), nil
	}
	cfg := defaultEngineConfig()
	if err := yaml.Unmarshal(res.Data, &cfg); err != nil {
		return defaultEngineConfig(), fmt.Errorf("parse enginefx.yaml: %w", err)
	}
	if cfg.SchemaVersion == 0 {
		cfg.SchemaVersion = 1
	}
	return cfg, nil
}

// SetEngineConfig serialises the config, uploads it to /enginefx.yaml
// on flash, and asks the hub to reload that single store.
func (a *App) SetEngineConfig(cfg EngineConfig) error {
	defer a.diag.Around("SetEngineConfig", nil)()
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
	// FileUpload reads from disk, so stage the YAML to a temp file.
	tmp, err := os.CreateTemp("", "enginefx-*.yaml")
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
		Path: enginePath, Target: client.TargetFlash, Mode: client.UploadSync,
	}); err != nil {
		return fmt.Errorf("upload enginefx.yaml: %w", err)
	}
	if err := c.Config.ReloadPath(enginePath); err != nil {
		return fmt.Errorf("reload: %w", err)
	}
	a.diag.Info("ENGINE", "saved + reloaded /enginefx.yaml")
	return nil
}

// ─── Runtime control ──────────────────────────────────────────────────

func (a *App) EngineStart() error {
	defer a.diag.Around("EngineStart", nil)()
	a.diag.Info("ENGINE", "EngineStart (manual run)")
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("ENGINE", "EngineStart: not connected")
		return fmt.Errorf("not connected")
	}
	if err := c.Engine.Start(); err != nil {
		a.diag.Error("ENGINE", "EngineStart failed: %v", err)
		return err
	}
	return nil
}

func (a *App) EngineStop() error {
	defer a.diag.Around("EngineStop", nil)()
	a.diag.Info("ENGINE", "EngineStop")
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("ENGINE", "EngineStop: not connected")
		return fmt.Errorf("not connected")
	}
	if err := c.Engine.Stop(); err != nil {
		a.diag.Error("ENGINE", "EngineStop failed: %v", err)
		return err
	}
	return nil
}

// EngineStatus is polled by the UI on a fast cadence — keep it at
// DEBUG level so the console isn't drowned in heartbeat lines.
func (a *App) EngineStatus() (EngineStatusDTO, error) {
	c := a.snapshotClient()
	if c == nil {
		return EngineStatusDTO{}, fmt.Errorf("not connected")
	}
	s, err := c.Engine.Status()
	if err != nil {
		a.diag.Debug("ENGINE", "EngineStatus failed: %v", err)
		return EngineStatusDTO{}, err
	}
	return EngineStatusDTO{
		State:     s.State,
		StateName: enginefx.StateName(s.State),
		Engaged:   s.ToggleEngaged,
	}, nil
}

// ─── File existence check (sound-file validation) ─────────────────────

// FileCheck is the result of a single existence probe.
type FileCheck struct {
	Path   string `json:"path"`
	Exists bool   `json:"exists"`
	Size   uint32 `json:"size"`
	Err    string `json:"err,omitempty"`
}

// CheckFiles probes a batch of paths on SD (where audio assets live) and
// reports per-path existence.  Empty paths are skipped (valid "none").
// Used by the EngineFx panel to mark missing sound files red.
func (a *App) CheckFiles(paths []string) ([]FileCheck, error) {
	defer a.diag.Around("CheckFiles", map[string]any{"n": len(paths)})()
	c := a.snapshotClient()
	if c == nil {
		return nil, fmt.Errorf("not connected")
	}
	out := make([]FileCheck, 0, len(paths))
	for _, p := range paths {
		if p == "" {
			continue
		}
		info, err := c.Storage.FileInfo(p)
		if err != nil {
			// A NACK (FILE_NOT_FOUND) is the common case — surface as "doesn't exist".
			out = append(out, FileCheck{Path: p, Exists: false, Err: err.Error()})
			continue
		}
		out = append(out, FileCheck{Path: p, Exists: info.Exists, Size: info.Size})
	}
	return out, nil
}

// installEngineStream forwards engine state-change events to the
// frontend as `engine:state`.  Called once per connect.
func (a *App) installEngineStream() {
	if a.c == nil {
		return
	}
	a.c.Events.OnEngineState(func(ev enginefx.StateChange) {
		if a.ctx == nil {
			return
		}
		wailsRT.EventsEmit(a.ctx, "engine:state", EngineStatusDTO{
			State:     ev.State,
			StateName: enginefx.StateName(ev.State),
		})
	})
}
