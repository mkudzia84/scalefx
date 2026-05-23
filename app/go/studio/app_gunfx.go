package main

// GunFx bindings — the Effects tab's GunFx panel (Phase 1 of GunFX
// rollout, instructions/22).
//
// Wire surface:
//   - /gunfx.yaml on flash holds the config (Phase 2 reads it on the
//     firmware side; Phase 1 just round-trips it through Studio so the
//     UI can author + persist a draft).
//   - GunFire / GunStart / GunStop / GunSmokeArm / GunFxStatus —
//     existing primary surface (0xCC..0xD2), already implemented.
//   - GunManualSet / GunManualRelease / GunVerboseSubscribe — Phase 1
//     stubs (firmware NACKs with GUN_NOT_IMPLEMENTED until Phase 2).
//   - `gun:shot` / `gun:verbose` Wails events forwarded from the
//     async-packet dispatcher.

import (
	"fmt"
	"os"
	"time"

	"scalefx/client"
	"scalefx/protocol/gunfx"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
	"gopkg.in/yaml.v3"
)

// ─── DTO (mirrors gunfx_config.h schema) ──────────────────────────────
//
// Wails serialises these to TypeScript via the bindings generator; the
// Svelte panel reads/writes them as plain JSON.  YAML tags mirror the
// on-device file (`/gunfx.yaml`), which the firmware parses with its
// own YamlParser in Phase 2.

type PortRefDTO struct {
	Board string `yaml:"board,omitempty" json:"board"` // alias from /hubfx.yaml (or "hub")
	Guid  string `yaml:"guid,omitempty"  json:"guid"`  // optional override of board → guid
	Kind  string `yaml:"kind"            json:"kind"`  // servo / pwm / hbridge / input
	Idx   uint8  `yaml:"idx"             json:"idx"`
}

type ServoMotionProfileDTO struct {
	MinUs             uint16 `yaml:"min_us"               json:"minUs"`
	MaxUs             uint16 `yaml:"max_us"               json:"maxUs"`
	CenterUs          uint16 `yaml:"center_us"            json:"centerUs"`
	Inverted          bool   `yaml:"inverted"             json:"inverted"`
	MaxSpeedUsPerSec  uint16 `yaml:"max_speed_us_per_sec" json:"maxSpeedUsPerSec"`
	MaxAccelUsPerSec2 uint16 `yaml:"max_accel_us_per_sec2" json:"maxAccelUsPerSec2"`
	MaxJerkUsPerSec3  uint16 `yaml:"max_jerk_us_per_sec3" json:"maxJerkUsPerSec3"`
}

type RofItemDTO struct {
	Name      string `yaml:"name"               json:"name"`
	BandLoUs  uint16 `yaml:"band_lo_us"         json:"bandLoUs"`
	BandHiUs  uint16 `yaml:"band_hi_us"         json:"bandHiUs"`
	Rpm       uint16 `yaml:"rpm"                json:"rpm"`
	SoundPath string `yaml:"sound,omitempty"    json:"soundPath"`
}

// Rule 43 — channel-input references are NAMES from /hubfx.yaml's
// `inputs:` block, not raw port + channel.  The firmware resolves at
// apply time via `findInputByName(hub, ...)`.

type RofConfigDTO struct {
	Input string       `yaml:"input,omitempty" json:"input"` // named channel
	Items []RofItemDTO `yaml:"items"           json:"items"`
}

type TriggerConfigDTO struct {
	Input        string `yaml:"input,omitempty" json:"input"` // named channel
	ThresholdUs  uint16 `yaml:"threshold_us"    json:"thresholdUs"`
	HysteresisUs uint16 `yaml:"hysteresis_us"   json:"hysteresisUs"`
}

type MuzzleFlashDTO struct {
	Port       PortRefDTO `yaml:"port"             json:"port"`
	DurationMs uint16     `yaml:"duration_ms"      json:"durationMs"`
	Brightness uint8      `yaml:"brightness"       json:"brightness"`
}

type RecoilConfigDTO struct {
	Port    PortRefDTO            `yaml:"port"             json:"port"`
	Profile ServoMotionProfileDTO `yaml:"profile"          json:"profile"`
	JerkUs  uint16                `yaml:"jerk_us"          json:"jerkUs"`
	HoldMs  uint16                `yaml:"hold_ms"          json:"holdMs"`
}

type HeaterDTO struct {
	Port             PortRefDTO `yaml:"port"               json:"port"`
	ElementMv        uint16     `yaml:"element_mv"         json:"elementMv"`
	Mode             string     `yaml:"mode"               json:"mode"` // always_on / bang_bang / closed_loop
	TargetCx10       int16      `yaml:"target_cx10"        json:"targetCx10"`
	HystCx10         int16      `yaml:"hyst_cx10"          json:"hystCx10"`
	Scaling          string     `yaml:"scaling"            json:"scaling"` // linear / quadratic / constant_duty
	ConstantDutyPct  uint8      `yaml:"constant_duty_pct"  json:"constantDutyPct"`
}

type FanDTO struct {
	Port            PortRefDTO `yaml:"port"               json:"port"`
	ElementMv       uint16     `yaml:"element_mv"         json:"elementMv"`
	Mode            string     `yaml:"mode"               json:"mode"` // off / continuous / puff_per_shot / puff_on_fire_active
	PuffMs          uint16     `yaml:"puff_ms"            json:"puffMs"`
	Scaling         string     `yaml:"scaling"            json:"scaling"`
	ConstantDutyPct uint8      `yaml:"constant_duty_pct"  json:"constantDutyPct"`
}

type SmokeConfigDTO struct {
	Heater HeaterDTO `yaml:"heater" json:"heater"`
	Fan    FanDTO    `yaml:"fan"    json:"fan"`
}

type GunAxisDTO struct {
	Enabled   bool                  `yaml:"enabled"          json:"enabled"`
	ServoPort PortRefDTO            `yaml:"servo_port"       json:"servoPort"`
	// Rule 43 — named-channel reference, resolved at apply time.
	Input     string                `yaml:"input,omitempty"  json:"input"`
	NeutralUs uint16                `yaml:"neutral_us"       json:"neutralUs"`
	// Profile retained as an EMPTY-by-default placeholder so the Wails
	// type generator keeps the field for forward compat. Motion shape
	// (Rule 42) lives on ServoActuatorRole in /hubfx.yaml ports[], not
	// here; this field is no longer serialised.
	Profile   ServoMotionProfileDTO `yaml:"-"                json:"profile"`
}

type GunDTO struct {
	Id          uint8            `yaml:"id"          json:"id"`
	Name        string           `yaml:"name"        json:"name"`
	Trigger     TriggerConfigDTO `yaml:"trigger"     json:"trigger"`
	Rof         RofConfigDTO     `yaml:"rof"         json:"rof"`
	MuzzleFlash MuzzleFlashDTO   `yaml:"muzzle_flash" json:"muzzleFlash"`
	Recoil      RecoilConfigDTO  `yaml:"recoil"      json:"recoil"`
	Smoke       SmokeConfigDTO   `yaml:"smoke"       json:"smoke"`
	Yaw         GunAxisDTO       `yaml:"yaw"         json:"yaw"`
	Pitch       GunAxisDTO       `yaml:"pitch"       json:"pitch"`
}

type GunFxConfig struct {
	SchemaVersion int      `yaml:"schema_version" json:"schemaVersion"`
	Enabled       bool     `yaml:"enabled"        json:"enabled"`
	Guns          []GunDTO `yaml:"guns"           json:"guns"`
}

// ─── Live state DTOs ─────────────────────────────────────────────────

// GunStatusDTO is one entry of `GunFxStatus()` — light state for the
// per-gun firing / smoke chip in the panel header.
type GunStatusDTO struct {
	Id         uint8 `json:"id"`
	Firing     bool  `json:"firing"`
	SmokeArmed bool  `json:"smokeArmed"`
}

// GunManualStateDTO is the input to GunManualSet — mirrors
// gunfx.ManualState 1:1.  See `gunfx.ManualFlag*` for the bit list.
type GunManualStateDTO struct {
	Flags         uint8  `json:"flags"`
	YawUs         uint16 `json:"yawUs"`
	PitchUs       uint16 `json:"pitchUs"`
	RofIndex      uint8  `json:"rofIndex"`
	FireHold      uint8  `json:"fireHold"`
	SmokeArm      uint8  `json:"smokeArm"`
	SmokeFanBurst uint8  `json:"smokeFanBurst"`
}

// GunVerboseStatusDTO mirrors gunfx.VerboseStatus for the frontend —
// Phase 4's "simulate" panel subscribes to this stream and renders the
// live mirror.
type GunVerboseStatusDTO struct {
	Id               uint8  `json:"id"`
	Mode             uint8  `json:"mode"`
	Firing           bool   `json:"firing"`
	SmokeArmed       bool   `json:"smokeArmed"`
	SmokeFanRunning  bool   `json:"smokeFanRunning"`
	HeaterDutyPct    uint8  `json:"heaterDutyPct"`
	HeaterTempCx10   int16  `json:"heaterTempCx10"`
	YawCurrentUs     uint16 `json:"yawCurrentUs"`
	YawTargetUs      uint16 `json:"yawTargetUs"`
	PitchCurrentUs   uint16 `json:"pitchCurrentUs"`
	PitchTargetUs    uint16 `json:"pitchTargetUs"`
	RofIndex         uint8  `json:"rofIndex"`
	RofSelectorUs    uint16 `json:"rofSelectorUs"`
	TriggerUs        uint16 `json:"triggerUs"`
	ShotsThisSession uint32 `json:"shotsThisSession"`
}

func defaultGunFxConfig() GunFxConfig {
	return GunFxConfig{SchemaVersion: 1, Enabled: false, Guns: []GunDTO{}}
}

// ─── Config I/O (YAML round-trip) ─────────────────────────────────────

const gunfxPath = "/gunfx.yaml"

// LoadGunFxConfig downloads /gunfx.yaml from flash and returns the
// parsed config.  A missing file is NOT an error — returns sensible
// defaults so the panel can be filled out fresh.
func (a *App) LoadGunFxConfig() (GunFxConfig, error) {
	defer a.diag.Around("LoadGunFxConfig", nil)()
	c := a.snapshotClient()
	if c == nil {
		return defaultGunFxConfig(), fmt.Errorf("not connected")
	}
	res, err := c.Storage.FileDownloadFrom(gunfxPath, client.TargetFlash, 5*time.Second)
	if err != nil {
		a.diag.Info("GUNFX", "no /gunfx.yaml yet (%v) — returning defaults", err)
		return defaultGunFxConfig(), nil
	}
	cfg := defaultGunFxConfig()
	if err := yaml.Unmarshal(res.Data, &cfg); err != nil {
		return defaultGunFxConfig(), fmt.Errorf("parse gunfx.yaml: %w", err)
	}
	if cfg.SchemaVersion == 0 {
		cfg.SchemaVersion = 1
	}
	return cfg, nil
}

// SaveGunFxConfig serialises the config, uploads it to /gunfx.yaml on
// flash, and asks the hub to reload that single store.  Phase 1 firmware
// doesn't yet parse the new schema — the reload is a no-op until Phase 2.
func (a *App) SaveGunFxConfig(cfg GunFxConfig) error {
	defer a.diag.Around("SaveGunFxConfig", nil)()
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
	tmp, err := os.CreateTemp("", "gunfx-*.yaml")
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
		Path: gunfxPath, Target: client.TargetFlash, Mode: client.UploadSync,
	}); err != nil {
		return fmt.Errorf("upload gunfx.yaml: %w", err)
	}
	if err := c.Config.ReloadPath(gunfxPath); err != nil {
		// Reload may NACK in Phase 1 (firmware ignores unknown config
		// paths) — surface as a warning, not an error, so Save still
		// commits the YAML to flash.
		a.diag.Warn("GUNFX", "reload %s: %v (Phase 1 — expected until Phase 2 wires the new schema)", gunfxPath, err)
	}
	a.diag.Info("GUNFX", "saved + reload-attempted /gunfx.yaml")
	return nil
}

// ─── Runtime control (primary surface — already implemented) ──────────

// GunFxStatus returns one entry per registered gun unit.
func (a *App) GunFxStatus() ([]GunStatusDTO, error) {
	c := a.snapshotClient()
	if c == nil {
		return nil, fmt.Errorf("not connected")
	}
	in, err := c.Gun.Status()
	if err != nil {
		return nil, err
	}
	out := make([]GunStatusDTO, len(in))
	for i, s := range in {
		out[i] = GunStatusDTO{Id: s.ID, Firing: s.Firing, SmokeArmed: s.SmokeArmed}
	}
	return out, nil
}

func (a *App) GunFire(id uint8) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Gun.FireOnce(id)
}

func (a *App) GunStartFiring(id uint8, rpm uint16) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Gun.StartFiring(id, rpm)
}

func (a *App) GunStopFiring(id uint8) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Gun.StopFiring(id)
}

func (a *App) GunSmokeArm(id, armed uint8) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Gun.SmokeArm(id, armed)
}

// ─── Phase 1 stubs (firmware NACKs with GUN_NOT_IMPLEMENTED) ──────────

// GunManualSet drives one gun in manual override mode — Studio's
// "simulate" panel uses this.  Phase 1 firmware NACKs with
// GUN_NOT_IMPLEMENTED so the round-trip is visible end-to-end.
func (a *App) GunManualSet(id uint8, state GunManualStateDTO) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Gun.ManualSet(id, gunfx.ManualState{
		Flags:         state.Flags,
		YawUs:         state.YawUs,
		PitchUs:       state.PitchUs,
		RofIndex:      state.RofIndex,
		FireHold:      state.FireHold,
		SmokeArm:      state.SmokeArm,
		SmokeFanBurst: state.SmokeFanBurst,
	})
}

// GunManualRelease exits manual mode for one gun.
func (a *App) GunManualRelease(id uint8) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Gun.ManualRelease(id)
}

// GunVerboseSubscribe enables (1) or disables (0) ~10 Hz verbose-status
// broadcasts for one gun.  Phase 1: firmware NACKs.  Phase 4 simulate
// panel will call this on mount/unmount.
func (a *App) GunVerboseSubscribe(id, enable uint8) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	return c.Gun.VerboseStatusSubscribe(id, enable)
}

// ─── Async streams ────────────────────────────────────────────────────

// installGunFxStream forwards gun shot + verbose-status events to the
// frontend as `gun:shot` / `gun:verbose`.  Called once per connect.
func (a *App) installGunFxStream() {
	if a.c == nil {
		return
	}
	a.c.Events.OnGunShot(func(ev gunfx.Shot) {
		if a.ctx == nil {
			return
		}
		wailsRT.EventsEmit(a.ctx, "gun:shot", map[string]any{
			"id": ev.ID,
		})
	})
	a.c.Events.OnGunVerboseStatus(func(ev gunfx.VerboseStatus) {
		if a.ctx == nil {
			return
		}
		wailsRT.EventsEmit(a.ctx, "gun:verbose", GunVerboseStatusDTO{
			Id:               ev.ID,
			Mode:             ev.Mode,
			Firing:           ev.Firing,
			SmokeArmed:       ev.SmokeArmed,
			SmokeFanRunning:  ev.SmokeFanRunning,
			HeaterDutyPct:    ev.HeaterDutyPct,
			HeaterTempCx10:   ev.HeaterTempCx10,
			YawCurrentUs:     ev.YawCurrentUs,
			YawTargetUs:      ev.YawTargetUs,
			PitchCurrentUs:   ev.PitchCurrentUs,
			PitchTargetUs:    ev.PitchTargetUs,
			RofIndex:         ev.RofIndex,
			RofSelectorUs:    ev.RofSelectorUs,
			TriggerUs:        ev.TriggerUs,
			ShotsThisSession: ev.ShotsThisSession,
		})
	})
}
