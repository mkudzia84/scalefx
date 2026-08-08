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

// ServoMotionProfileDTO mirrors `sfx_core::ServoMotionProfile` field-
// for-field.  Rule 44 carries this inline on each GunAxis (and any
// future per-feature servo binding).  It is ALSO the device-model
// profile DTO (`SetPortProfile` in app_devicemodel.go), so one
// ServoProfileEditor component binds the persisted-with-feature profile
// and the live-tune push to the same shape.
type ServoMotionProfileDTO struct {
	MinUs             uint16 `yaml:"min_us"               json:"minUs"`
	MaxUs             uint16 `yaml:"max_us"               json:"maxUs"`
	CenterUs          uint16 `yaml:"center_us"            json:"centerUs"`
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
	// Stereo routing for this ROF's shot sound — bit 0 = left,
	// bit 1 = right.  Omitted = both (0x03).  Same shape as
	// EngineFx's `output: left|right|both` but per-ROF here
	// because different gun bursts may want different routing.
	OutputMask uint8 `yaml:"output_mask,omitempty" json:"outputMask"`
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

// RecoilConfigDTO — turret behaviour, no dedicated servo.  2026-06:
// recoil is an INSTANT, RANDOM jerk on BOTH yaw + pitch on each shot —
// each axis snaps to commanded ± a random offset up to JerkUs, holds
// HoldMs, then snaps back.  The old `axis` picker was dropped (both axes
// kick); aiming still slews via each axis's ServoActuatorRole.
type RecoilConfigDTO struct {
	Enabled bool   `yaml:"enabled"          json:"enabled"`
	JerkUs  uint16 `yaml:"jerk_us"          json:"jerkUs"`   // max random |offset|
	HoldMs  uint16 `yaml:"hold_ms"          json:"holdMs"`
}

// HeaterActivationDTO — Rule 43 named-channel that gates the heater.
// Optional: empty Input ⇒ no gating (heater is always allowed when
// smoke is armed).  Threshold / hysteresis tuned in µs around the
// activation stick's midpoint, same shape as the trigger cluster.
type HeaterActivationDTO struct {
	Input         string `yaml:"input,omitempty"   json:"input"`
	ThresholdUs   uint16 `yaml:"threshold_us"      json:"thresholdUs"`
	HysteresisUs  uint16 `yaml:"hysteresis_us"     json:"hysteresisUs"`
}

type HeaterDTO struct {
	Port        PortRefDTO          `yaml:"port"                  json:"port"`
	// Element-rated voltage (default 6 V — typical smoke cartridge).
	// Drives the role's scaleDuty() against the port rail.
	ElementMv   uint16              `yaml:"element_mv"            json:"elementMv"`
	// Mode: "continuous" | "cycle".
	//   continuous — drive at element-scaled duty whenever activated.
	//   cycle      — duty-cycle the heater on/off at gun-layer
	//                intervals (CycleOnMs / CycleOffMs).  Use for
	//                power conservation or to limit cartridge
	//                temperature without a thermistor.
	Mode        string              `yaml:"mode"                  json:"mode"`
	// CYCLE-mode timing.  ms units.  Ignored in continuous mode.
	CycleOnMs   uint16              `yaml:"cycle_on_ms"           json:"cycleOnMs"`
	CycleOffMs  uint16              `yaml:"cycle_off_ms"          json:"cycleOffMs"`
	// Rule 43 activation channel (Phase 4 polish 2026-05-26).
	Activation  HeaterActivationDTO `yaml:"activation,omitempty"  json:"activation"`
}

type FanDTO struct {
	Port      PortRefDTO `yaml:"port"               json:"port"`
	ElementMv uint16     `yaml:"element_mv"         json:"elementMv"`
	// Two-mode set: "continuous" | "pulse".  A fan is disabled by
	// leaving `port` empty — no separate "off" mode.
	//   continuous — fan at 100 % whenever smoke is armed and the
	//                trigger is firing.
	//   pulse      — sinusoidal envelope per shot: 50 % base rises
	//                to 100 % peak then back to 50 % over
	//                `PulseDurationMs`.  Default duration matches
	//                the default ROF firing rate (100 ms = 600 RPM).
	Mode             string `yaml:"mode"               json:"mode"`
	PulseDurationMs  uint16 `yaml:"pulse_duration_ms"  json:"pulseDurationMs"`
}

type SmokeConfigDTO struct {
	Heater HeaterDTO `yaml:"heater" json:"heater"`
	Fan    FanDTO    `yaml:"fan"    json:"fan"`
}

type GunAxisDTO struct {
	Enabled   bool       `yaml:"enabled"          json:"enabled"`
	ServoPort PortRefDTO `yaml:"servo_port"       json:"servoPort"`
	// Rule 43 — named-channel reference, resolved at apply time.
	Input     string `yaml:"input,omitempty"  json:"input"`
	NeutralUs uint16 `yaml:"neutral_us"       json:"neutralUs"`
	// Servo motion profile is NOT in /gunfx.yaml — Rule 42 stores it on
	// the role's port-attach in /hubfx.yaml; Rule 44 exposes the editor
	// inline in the GunFx panel via `Port.profile` on the device model.
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
	// Rule 43 activation gate state (Phase 4 polish 2026-05-26).
	HeaterActive     bool   `json:"heaterActive"`
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
	a.diag.Info("GUNFX", "LoadGunFxConfig: downloading %s …", gunfxPath)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "LoadGunFxConfig: not connected")
		return defaultGunFxConfig(), fmt.Errorf("not connected")
	}
	res, err := c.Storage.FileDownloadFrom(gunfxPath, client.TargetFlash, 5*time.Second)
	if err != nil {
		a.diag.Info("GUNFX", "LoadGunFxConfig: %s not present (%v) — returning defaults", gunfxPath, err)
		return defaultGunFxConfig(), nil
	}
	cfg := defaultGunFxConfig()
	if err := yaml.Unmarshal(res.Data, &cfg); err != nil {
		a.diag.Error("GUNFX", "LoadGunFxConfig: yaml.Unmarshal: %v", err)
		return defaultGunFxConfig(), fmt.Errorf("parse gunfx.yaml: %w", err)
	}
	if cfg.SchemaVersion == 0 {
		cfg.SchemaVersion = 1
	}
	// Canonicalise hub-identity port-ref GUIDs → "" (instructions/31) so the
	// output-port dropdowns resolve against the canonical device model.
	folds := 0
	foldRef := func(label string, p *PortRefDTO) {
		if c, was := a.canonGuid(p.Guid); was {
			a.diag.Info("GUNFX", "[gap] %s ref guid=%q kind=%q idx=%d → canon \"\" (hub=%q)",
				label, p.Guid, p.Kind, p.Idx, a.id.GUID)
			p.Guid = c
			folds++
		}
	}
	for i := range cfg.Guns {
		g := &cfg.Guns[i]
		foldRef(fmt.Sprintf("gun[%d].muzzle", i), &g.MuzzleFlash.Port)
		foldRef(fmt.Sprintf("gun[%d].smoke.heater", i), &g.Smoke.Heater.Port)
		foldRef(fmt.Sprintf("gun[%d].smoke.fan", i), &g.Smoke.Fan.Port)
		foldRef(fmt.Sprintf("gun[%d].yaw.servo", i), &g.Yaw.ServoPort)
		foldRef(fmt.Sprintf("gun[%d].pitch.servo", i), &g.Pitch.ServoPort)
	}
	a.diag.Info("GUNFX", "LoadGunFxConfig: ok — enabled=%v guns=%d bytes=%d; folded %d hub-identity port ref(s) → \"\" (hub=%q)",
		cfg.Enabled, len(cfg.Guns), len(res.Data), folds, a.id.GUID)
	return cfg, nil
}

// SaveGunFxConfig serialises the config, uploads it to /gunfx.yaml on
// flash, and asks the hub to reload that single store.  The reload
// runs `applyGunFxConfig` on the firmware side (wired at boot via
// kGunFx.wire(cfgPolicy, applyGunFxConfigCallback) in hubfx_esp32s3.ino),
// so this is what makes a draft "take effect" — sound paths, ROF
// bands, trigger thresholds, etc. all hit RAM here.
func (a *App) SaveGunFxConfig(cfg GunFxConfig) error {
	defer a.diag.Around("SaveGunFxConfig", nil)()
	a.diag.Info("GUNFX", "SaveGunFxConfig: enabled=%v guns=%d → %s",
		cfg.Enabled, len(cfg.Guns), gunfxPath)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "SaveGunFxConfig: not connected")
		return fmt.Errorf("not connected")
	}
	if cfg.SchemaVersion == 0 {
		cfg.SchemaVersion = 1
	}
	data, err := yaml.Marshal(&cfg)
	if err != nil {
		a.diag.Error("GUNFX", "SaveGunFxConfig: yaml.Marshal: %v", err)
		return fmt.Errorf("serialise: %w", err)
	}
	tmp, err := os.CreateTemp("", "gunfx-*.yaml")
	if err != nil {
		a.diag.Error("GUNFX", "SaveGunFxConfig: tempfile: %v", err)
		return err
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		a.diag.Error("GUNFX", "SaveGunFxConfig: write tempfile: %v", err)
		return err
	}
	tmp.Close()

	upStart := time.Now()
	if _, err := c.Storage.FileUpload(tmpPath, client.UploadOptions{
		Path: gunfxPath, Target: client.TargetFlash, Mode: client.UploadSync,
	}); err != nil {
		a.diag.Error("GUNFX", "SaveGunFxConfig: upload %s: %v", gunfxPath, err)
		return fmt.Errorf("upload gunfx.yaml: %w", err)
	}
	a.diag.Info("GUNFX", "SaveGunFxConfig: uploaded %d bytes in %d ms",
		len(data), time.Since(upStart).Milliseconds())

	relStart := time.Now()
	if err := c.Config.ReloadPath(gunfxPath); err != nil {
		// Reload NACK means firmware refused to apply — surface it as
		// an ERROR (not a warning), because the YAML is on flash but
		// the live config didn't update, which is exactly the "I
		// edited the panel but nothing changed" symptom.
		a.diag.Error("GUNFX", "SaveGunFxConfig: reload %s NACKed: %v — file is on flash but firmware did NOT re-apply (sounds/trigger may not reflect changes)",
			gunfxPath, err)
		return fmt.Errorf("firmware refused reload of %s: %w (file is saved on flash but not applied — power-cycle or fix schema)", gunfxPath, err)
	}
	a.diag.Info("GUNFX", "SaveGunFxConfig: ✓ uploaded + reloaded %s (reload %d ms) — firmware re-applied",
		gunfxPath, time.Since(relStart).Milliseconds())
	return nil
}

// ─── Runtime control (primary surface — already implemented) ──────────

// logGunErr is a tiny helper that escalates non-nil errors to the
// console at ERROR level (so timeouts and NACKs are visible without
// needing to flip /diag debug on).  Returns the err unchanged so call
// sites stay one-line.
func (a *App) logGunErr(method string, fields map[string]any, err error) error {
	if err == nil {
		return nil
	}
	if fields == nil {
		fields = map[string]any{}
	}
	fields["error"] = err.Error()
	a.diag.With(LvlError, "GUNFX", method+" failed", fields)
	return err
}

// GunFxStatus returns one entry per registered gun unit.
func (a *App) GunFxStatus() ([]GunStatusDTO, error) {
	defer a.diag.Around("GunFxStatus", nil)()
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "GunFxStatus: not connected")
		return nil, fmt.Errorf("not connected")
	}
	in, err := c.Gun.Status()
	if err != nil {
		return nil, a.logGunErr("GunFxStatus", nil, err)
	}
	out := make([]GunStatusDTO, len(in))
	for i, s := range in {
		out[i] = GunStatusDTO{Id: s.ID, Firing: s.Firing, SmokeArmed: s.SmokeArmed}
	}
	a.diag.Debug("GUNFX", "GunFxStatus: %d gun(s)", len(out))
	return out, nil
}

// GunFire fires one shot using the firmware's currently-armed ROF.
// Kept for backward compatibility — new GUI callers should prefer
// GunFireWithRof so the operator's dropdown selection is honoured.
func (a *App) GunFire(id uint8) error {
	defer a.diag.Around("GunFire", map[string]any{"id": id})()
	a.diag.Info("GUNFX", "GunFire id=%d (single shot)", id)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "GunFire id=%d: not connected", id)
		return fmt.Errorf("not connected")
	}
	return a.logGunErr("GunFire", map[string]any{"id": id}, c.Gun.FireOnce(id))
}

// GunFireWithRof forces an explicit ROF index for THIS shot — used
// by Studio's Fire button + ROF dropdown so manual testing always
// has a concrete program even when the RC selector stick is between
// bands (2026-05-24).  rofIndex=0xFF (= 255) means "use the
// currently-armed ROF" — same as the legacy GunFire path.
func (a *App) GunFireWithRof(id, rofIndex uint8) error {
	defer a.diag.Around("GunFireWithRof", map[string]any{"id": id, "rof": rofIndex})()
	a.diag.Info("GUNFX", "GunFireWithRof id=%d rof=%d (single shot, dropdown)", id, rofIndex)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "GunFireWithRof id=%d: not connected", id)
		return fmt.Errorf("not connected")
	}
	return a.logGunErr("GunFireWithRof", map[string]any{"id": id, "rof": rofIndex},
		c.Gun.FireOnceWithRof(id, rofIndex))
}

func (a *App) GunStartFiring(id uint8, rpm uint16) error {
	defer a.diag.Around("GunStartFiring", map[string]any{"id": id, "rpm": rpm})()
	a.diag.Info("GUNFX", "GunStartFiring id=%d rpm=%d (auto-fire start)", id, rpm)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "GunStartFiring id=%d: not connected", id)
		return fmt.Errorf("not connected")
	}
	return a.logGunErr("GunStartFiring", map[string]any{"id": id, "rpm": rpm}, c.Gun.StartFiring(id, rpm))
}

// GunStartFiringWithRof forces an explicit ROF index for the burst —
// counterpart to GunFireWithRof for auto-fire (2026-05-24).
// rofIndex=0xFF means "use the currently-armed ROF".  The forced
// index is cleared on the next GunStopFiring.
func (a *App) GunStartFiringWithRof(id uint8, rpm uint16, rofIndex uint8) error {
	defer a.diag.Around("GunStartFiringWithRof",
		map[string]any{"id": id, "rpm": rpm, "rof": rofIndex})()
	a.diag.Info("GUNFX", "GunStartFiringWithRof id=%d rpm=%d rof=%d (auto-fire start, dropdown)", id, rpm, rofIndex)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "GunStartFiringWithRof id=%d: not connected", id)
		return fmt.Errorf("not connected")
	}
	return a.logGunErr("GunStartFiringWithRof",
		map[string]any{"id": id, "rpm": rpm, "rof": rofIndex},
		c.Gun.StartFiringWithRof(id, rpm, rofIndex))
}

func (a *App) GunStopFiring(id uint8) error {
	defer a.diag.Around("GunStopFiring", map[string]any{"id": id})()
	a.diag.Info("GUNFX", "GunStopFiring id=%d", id)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "GunStopFiring id=%d: not connected", id)
		return fmt.Errorf("not connected")
	}
	return a.logGunErr("GunStopFiring", map[string]any{"id": id}, c.Gun.StopFiring(id))
}

func (a *App) GunSmokeArm(id, armed uint8) error {
	defer a.diag.Around("GunSmokeArm", map[string]any{"id": id, "armed": armed})()
	a.diag.Info("GUNFX", "GunSmokeArm id=%d armed=%d", id, armed)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "GunSmokeArm id=%d: not connected", id)
		return fmt.Errorf("not connected")
	}
	return a.logGunErr("GunSmokeArm", map[string]any{"id": id, "armed": armed}, c.Gun.SmokeArm(id, armed))
}

// ─── Manual override / verbose-status subscribe ───────────────────────

// GunManualSet drives one gun in manual override mode — Studio's
// "simulate" panel uses this.  Logs the flag set + each non-zero field
// so a tail on the console makes the puppet-mode traffic legible.
func (a *App) GunManualSet(id uint8, state GunManualStateDTO) error {
	fields := map[string]any{
		"id": id, "flags": fmt.Sprintf("0x%02X", state.Flags),
	}
	if state.Flags&0x01 != 0 {
		fields["yawUs"] = state.YawUs
	}
	if state.Flags&0x02 != 0 {
		fields["pitchUs"] = state.PitchUs
	}
	if state.Flags&0x04 != 0 {
		fields["rofIndex"] = state.RofIndex
	}
	if state.Flags&0x08 != 0 {
		fields["fireHold"] = state.FireHold
	}
	if state.Flags&0x10 != 0 {
		fields["smokeArm"] = state.SmokeArm
	}
	if state.Flags&0x20 != 0 {
		fields["smokeFanBurst"] = state.SmokeFanBurst
	}
	defer a.diag.Around("GunManualSet", fields)()
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "GunManualSet id=%d: not connected", id)
		return fmt.Errorf("not connected")
	}
	return a.logGunErr("GunManualSet", fields, c.Gun.ManualSet(id, gunfx.ManualState{
		Flags:         state.Flags,
		YawUs:         state.YawUs,
		PitchUs:       state.PitchUs,
		RofIndex:      state.RofIndex,
		FireHold:      state.FireHold,
		SmokeArm:      state.SmokeArm,
		SmokeFanBurst: state.SmokeFanBurst,
	}))
}

// GunManualRelease exits manual mode for one gun.
func (a *App) GunManualRelease(id uint8) error {
	defer a.diag.Around("GunManualRelease", map[string]any{"id": id})()
	a.diag.Info("GUNFX", "GunManualRelease id=%d", id)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "GunManualRelease id=%d: not connected", id)
		return fmt.Errorf("not connected")
	}
	return a.logGunErr("GunManualRelease", map[string]any{"id": id}, c.Gun.ManualRelease(id))
}

// GunVerboseSubscribe enables (1) or disables (0) ~10 Hz verbose-status
// broadcasts for one gun.  Phase 4 simulate panel calls this on
// mount/unmount.
func (a *App) GunVerboseSubscribe(id, enable uint8) error {
	defer a.diag.Around("GunVerboseSubscribe", map[string]any{"id": id, "enable": enable})()
	a.diag.Info("GUNFX", "GunVerboseSubscribe id=%d enable=%d", id, enable)
	c := a.snapshotClient()
	if c == nil {
		a.diag.Error("GUNFX", "GunVerboseSubscribe id=%d: not connected", id)
		return fmt.Errorf("not connected")
	}
	return a.logGunErr("GunVerboseSubscribe",
		map[string]any{"id": id, "enable": enable},
		c.Gun.VerboseStatusSubscribe(id, enable))
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
			HeaterActive:     ev.HeaterActive,
		})
	})
}
