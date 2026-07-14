package main

// /hubfx.yaml on-connect loader.  Pulls the operator-authored hub config
// off the device and populates Studio's overlay state:
//
//   inputs[]   → channel-function map (channel.function = binding.name)
//   ports[]    → operator-friendly port names (Port.Name overlay)
//   expanders[].ports[] → per-expander port names
//
// Role attachments are NOT applied here — the firmware already does that
// on YAML load and the live RoleList reflects it.  This binding only
// hydrates the editable Studio overlay so the UI shows what's currently
// on the device.

import (
	"fmt"
	"os"
	"sort"
	"time"

	"scalefx/client"
	"scalefx/devicemodel"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"

	"gopkg.in/yaml.v3"
)

const hubConfigPath = "/hubfx.yaml"

type yamlPortRef struct {
	Kind string `yaml:"kind"`
	Idx  byte   `yaml:"idx"`
}

type yamlInputBinding struct {
	Name        string       `yaml:"name"`
	ID          int          `yaml:"id"`
	Port        *yamlPortRef `yaml:"port,omitempty"`
	Description string       `yaml:"description,omitempty"`
}

// yamlPortBinding mirrors the FLAT shape the firmware parser at
// `hubfx_config.h::populate` expects: `{kind, idx, role, label}` at
// each ports[i] entry — NOT the nested `port: { … }` form that the
// `inputs[]` block uses (Studio used to emit nested here too, which
// silently dropped every port's kind on the firmware side — the parser
// then logged "ports[N]: missing or unknown `kind`" for every entry).
// `Label` matches the firmware key; `Name` is kept on the Go struct
// only as an alias for inbound parsing of older /hubfx.yaml files
// written by the previous Studio.
type yamlPortBinding struct {
	Kind    string                       `yaml:"kind"`
	Idx     byte                         `yaml:"idx"`
	Role    string                       `yaml:"role,omitempty"`
	Label   string                       `yaml:"label,omitempty"`
	// Rule 42 storage + Rule 44 editing surface: servo motion profile
	// stamped per port.  Nil when omitted — the role attaches with its
	// initFromPort defaults.  Only set for `kind: servo`.
	Profile *devicemodel.ServoMotionProfile `yaml:"profile,omitempty"`
	// esc-telemetry stream selector (kontronik | scorpion | hobbywing-v4 |
	// hobbywing-v5 | jeti-exbus) — only on input ports with that role.
	EscProtocol string `yaml:"esc_protocol,omitempty"`
	// esc-telemetry RPM scaling: motor pole count (electrical rpm = shaft
	// rpm × poles/2) + gearbox ratio; omitted when direct / unknown.
	EscMotorPoles int     `yaml:"esc_motor_poles,omitempty"`
	EscGearRatio  float64 `yaml:"esc_gear_ratio,omitempty"`
}

type yamlExpanderEntry struct {
	Alias string            `yaml:"alias"`
	GUID  string            `yaml:"guid"`
	// Type is the board kind ("gearcontrol", "lightfx", …).  Stamped on
	// Save from the connected board's kind so a later OFFLINE load knows
	// which schematic to show + how to name the ghost board.  Optional for
	// back-compat: an older file without it is inferred from its ports.
	Type  string            `yaml:"type,omitempty"`
	Ports []yamlPortBinding `yaml:"ports,omitempty"`
}

// expanderKind returns the board kind for a saved expander entry: the
// explicit `type:` if present, else inferred from its port kinds (an
// H-bridge ⇒ gearcontrol — the only board that declares them today).
// Empty when it can't be determined (the UI then shows a generic card,
// no schematic).
func (e *yamlExpanderEntry) expanderKind() string {
	if e.Type != "" {
		return e.Type
	}
	for _, p := range e.Ports {
		if p.Kind == "hbridge" {
			return "gearcontrol"
		}
	}
	return ""
}

// yamlAudio / yamlFeatures / yamlTelemetry mirror the same-named blocks
// in `hubfx_config.h::populate()` — the firmware reads them at top
// level.  Studio used to drop them on save (the struct didn't have
// fields for them) which RESET them to firmware defaults every Apply
// — bit us hard because `features.enginefx` and `features.gunfx`
// default to FALSE, so an Apply silently killed both effects.
// Round-trip them now: read on Load, write back on Save.

type yamlAudio struct {
	CodecSupply string `yaml:"codec_supply,omitempty"`
}

type yamlFeatures struct {
	Alerts        bool `yaml:"alerts"`
	Enginefx      bool `yaml:"enginefx"`
	LandingLights bool `yaml:"landing_lights"`
	Lightfx       bool `yaml:"lightfx"`
	Gears         bool `yaml:"gears"`
	Gunfx         bool `yaml:"gunfx"`
}

type yamlTelemetry struct {
	Inputs     bool   `yaml:"inputs"`
	Outputs    bool   `yaml:"outputs"`
	IntervalMs uint16 `yaml:"interval_ms,omitempty"`
}

type hubYamlConfig struct {
	SchemaVersion int                 `yaml:"schema_version,omitempty"`
	Audio         *yamlAudio          `yaml:"audio,omitempty"`
	Features      *yamlFeatures       `yaml:"features,omitempty"`
	Telemetry     *yamlTelemetry      `yaml:"telemetry,omitempty"`
	Inputs        []yamlInputBinding  `yaml:"inputs,omitempty"`
	Ports         []yamlPortBinding   `yaml:"ports,omitempty"`
	Expanders     []yamlExpanderEntry `yaml:"expanders,omitempty"`
}

// defaultFeatures is what Studio emits when /hubfx.yaml has no
// `features:` block on load.  All-true so a fresh first save doesn't
// kill the effects the operator just configured.  The firmware-side
// FeaturesBlock defaults (enginefx=false, gunfx=false) only apply
// when the YAML key is genuinely absent — Studio now ALWAYS emits a
// `features:` block, so the firmware defaults stop being a footgun.
func defaultFeatures() *yamlFeatures {
	return &yamlFeatures{
		Alerts:        true,
		Enginefx:      true,
		LandingLights: true,
		Lightfx:       true,
		Gears:         true,
		Gunfx:         true,
	}
}

// LoadHubConfig downloads /hubfx.yaml and applies the inputs[]
// channel-function map + ports[]/expanders[] friendly names to the
// in-memory overlay.  A missing file is not an error — first-time use
// just means no overrides.
func (a *App) LoadHubConfig() error {
	defer a.diag.Around("LoadHubConfig", nil)()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	res, err := c.Storage.FileDownloadFrom(hubConfigPath, client.TargetFlash, 5*time.Second)
	if err != nil {
		a.diag.Info("CFG", "no /hubfx.yaml yet (%v) — skipping overlay hydration", err)
		return nil
	}
	var cfg hubYamlConfig
	if err := yaml.Unmarshal(res.Data, &cfg); err != nil {
		return fmt.Errorf("parse /hubfx.yaml: %w", err)
	}

	// Hub-local ports/inputs/profiles are keyed by the CANONICAL empty GUID
	// (instructions/31) — the same form the device model + SetPortProfile use,
	// so a loaded /hubfx.yaml profile overlays the right port. (Expander ports
	// below key by their real GUID.)
	const hubGUID = ""

	a.dmMu.Lock()
	// Capture the top-level non-overlay blocks (audio / features /
	// telemetry) so Save can round-trip them.  Nil ⇒ the YAML had no
	// such block; we leave the field nil and Save substitutes the
	// canonical default (all-true for features).
	a.hubAudio     = cfg.Audio
	a.hubFeatures  = cfg.Features
	a.hubTelemetry = cfg.Telemetry
	// ── inputs[] → channel.function ────────────────────────────────
	for _, ib := range cfg.Inputs {
		if ib.Name == "" || ib.ID <= 0 {
			continue
		}
		// Defaults: hub-local Input port, index 0.
		kind := ports.KindInput
		idx := byte(0)
		if ib.Port != nil {
			if k, ok := kindFromYamlKindName(ib.Port.Kind); ok {
				kind = k
			}
			idx = ib.Port.Idx
		}
		ref := devicemodel.PortRef{GUID: hubGUID, Kind: kind, Index: idx}
		pCfg, ok := a.inputs[ref]
		if !ok {
			// Auto-create config entry for this input so the binding lands.
			c := devicemodel.NewInputPortConfig(ref)
			a.inputs[ref] = &c
			pCfg = &c
		}
		ch := ib.ID - 1
		if ch < 0 {
			continue
		}
		if ch >= pCfg.ChannelCount {
			pCfg.SetChannelCount(ch + 1)
		}
		pCfg.SetFunction(ch, ib.Name)
	}

	// ── ports[] → operator-friendly label overlay + servo motion
	//    profile overlay (Rule 42 storage / Rule 44 editing surface).
	for _, pb := range cfg.Ports {
		kind, ok := kindFromYamlKindName(pb.Kind)
		if !ok {
			continue
		}
		ref := devicemodel.PortRef{GUID: hubGUID, Kind: kind, Index: pb.Idx}
		if pb.Label != "" {
			a.portNames[ref] = pb.Label
		}
		if pb.EscProtocol != "" {
			ic := a.inputCfg(hubGUID, kind, pb.Idx)
			ic.Protocol = devicemodel.InputEscTelem
			ic.EscProtocol = pb.EscProtocol
			if pb.EscMotorPoles >= 2 {
				ic.EscMotorPoles = pb.EscMotorPoles
			}
			if pb.EscGearRatio > 0 {
				ic.EscGearRatio = pb.EscGearRatio
			}
		}
		if pb.Profile != nil && pb.Kind == "servo" {
			a.portProfiles[ref] = ServoMotionProfileDTO(*pb.Profile)
		}
	}

	// ── expanders[].ports[] → per-expander port labels + profiles. ─
	// Also RETAIN the whole expander block keyed by GUID: this is what lets
	// a configured-but-disconnected board still surface (offline ghost
	// ports) and survive a Save (Rule: never silently drop config).
	a.hubExpanders = map[string]*yamlExpanderEntry{}
	for i := range cfg.Expanders {
		exp := &cfg.Expanders[i]
		if exp.GUID == "" {
			continue
		}
		cp := *exp
		a.hubExpanders[exp.GUID] = &cp
		for _, pb := range exp.Ports {
			kind, ok := kindFromYamlKindName(pb.Kind)
			if !ok {
				continue
			}
			ref := devicemodel.PortRef{GUID: exp.GUID, Kind: kind, Index: pb.Idx}
			if pb.Label != "" {
				a.portNames[ref] = pb.Label
			}
			if pb.Profile != nil && pb.Kind == "servo" {
				a.portProfiles[ref] = ServoMotionProfileDTO(*pb.Profile)
			}
		}
	}
	a.dmMu.Unlock()

	a.diag.Info("CFG", "loaded /hubfx.yaml: %d inputs · %d hub ports · %d expanders",
		len(cfg.Inputs), len(cfg.Ports), len(cfg.Expanders))
	a.emitDeviceModelChanged()
	return nil
}

// SaveHubConfig serialises the studio's current overlay state back to
// /hubfx.yaml: every channel with a function becomes an entry in inputs[],
// every named or role-attached port becomes an entry in ports[] (or in the
// matching expander's nested ports[]).  The file is uploaded to flash and
// the firmware reloads just that store so the change takes effect live.
func (a *App) SaveHubConfig() error {
	defer a.diag.Around("SaveHubConfig", nil)()
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}

	// Hub-local ports are the canonical empty GUID in the device model
	// (instructions/31); the input-default check + the ports[]-vs-expanders[]
	// split below compare against it.
	const hubGUID = ""

	cfg := hubYamlConfig{SchemaVersion: 1}
	// Round-trip the top-level blocks we don't have a UI for yet.
	// `features` is critical — if we omit it, the firmware applies
	// FeaturesBlock defaults (enginefx=false, gunfx=false) and silently
	// disables those effects on every Apply.  Use the in-memory overlay
	// from LoadHubConfig if present, else the canonical all-true
	// defaults so a fresh first save doesn't kill anything.
	a.dmMu.Lock()
	if a.hubAudio != nil {
		cfg.Audio = a.hubAudio
	}
	if a.hubFeatures != nil {
		cfg.Features = a.hubFeatures
	} else {
		cfg.Features = defaultFeatures()
	}
	if a.hubTelemetry != nil {
		cfg.Telemetry = a.hubTelemetry
	}
	a.dmMu.Unlock()

	a.dmMu.Lock()
	// inputs[] — emit every channel whose function is set (and not the
	// sentinel "unassigned").  Sort by (port, channel) for stable YAML.
	inputRefs := make([]devicemodel.PortRef, 0, len(a.inputs))
	for ref := range a.inputs {
		inputRefs = append(inputRefs, ref)
	}
	sort.Slice(inputRefs, func(i, j int) bool {
		if inputRefs[i].GUID != inputRefs[j].GUID {
			return inputRefs[i].GUID < inputRefs[j].GUID
		}
		if inputRefs[i].Kind != inputRefs[j].Kind {
			return inputRefs[i].Kind < inputRefs[j].Kind
		}
		return inputRefs[i].Index < inputRefs[j].Index
	})
	for _, ref := range inputRefs {
		ic := a.inputs[ref]
		for _, ch := range ic.Channels {
			if ch.Function == "" || ch.Function == "unassigned" {
				continue
			}
			ib := yamlInputBinding{Name: ch.Function, ID: ch.Channel + 1}
			// Emit the port block only when it's not the default hub-local
			// IN_1 — keeps YAML small + matches the firmware's defaulting.
			if !(ref.GUID == hubGUID && ref.Kind == ports.KindInput && ref.Index == 0) {
				ib.Port = &yamlPortRef{Kind: yamlKindName(ref.Kind), Idx: ref.Index}
			}
			cfg.Inputs = append(cfg.Inputs, ib)
		}
	}

	// ports[] and expanders[].ports[] — every port that has a name or an
	// attached role.  Hub ports go in ports[]; expander ports group under
	// their GUID in expanders[].
	expGroups := map[string][]yamlPortBinding{}
	expBoardName := map[string]string{} // guid → live BoardName (for Type stamping)
	if a.dm != nil {
		dmPorts := append([]devicemodel.Port(nil), a.dm.Ports...)
		sort.Slice(dmPorts, func(i, j int) bool {
			if dmPorts[i].Ref.GUID != dmPorts[j].Ref.GUID {
				return dmPorts[i].Ref.GUID < dmPorts[j].Ref.GUID
			}
			if dmPorts[i].Ref.Kind != dmPorts[j].Ref.Kind {
				return dmPorts[i].Ref.Kind < dmPorts[j].Ref.Kind
			}
			return dmPorts[i].Ref.Index < dmPorts[j].Ref.Index
		})
		for _, p := range dmPorts {
			name := a.portNames[p.Ref]
			role := ""
			if p.RoleKind != roles.KindNone {
				role = roles.KindName(p.RoleKind)
			}
			// lookupProfile tries both hub-local GUID forms (hub GUID ⇄ "")
			// so a profile saved via the calibration dialog (keyed under the
			// device-model GUID) persists even though /hubfx.yaml ports[] are
			// hub-local ("").  Without this, calibrated settings silently never
			// reached the YAML.
			prof, hasProf := a.lookupProfile(p.Ref)
			if name == "" && role == "" && !hasProf {
				continue
			}
			pb := yamlPortBinding{
				Kind:  yamlKindName(p.Ref.Kind),
				Idx:   p.Ref.Index,
				Role:  role,
				Label: name,
			}
			if p.RoleKind == roles.KindEscTelemetry {
				if ic, ok := a.inputs[devicemodel.PortRef{GUID: "", Kind: p.Ref.Kind, Index: p.Ref.Index}]; ok {
					if ic.EscProtocol != "" {
						pb.EscProtocol = ic.EscProtocol
					}
					if ic.EscMotorPoles > 2 {
						pb.EscMotorPoles = ic.EscMotorPoles
					}
					if ic.EscGearRatio > 0 && ic.EscGearRatio != 1 {
						pb.EscGearRatio = ic.EscGearRatio
					}
				}
			}
			if hasProf && p.KindName == "servo" {
				dp := devicemodel.ServoMotionProfile(prof)
				pb.Profile = &dp
			}
			if p.Ref.GUID == "" { // canonical hub-local → ports[]; else expanders[]
				cfg.Ports = append(cfg.Ports, pb)
			} else {
				expGroups[p.Ref.GUID] = append(expGroups[p.Ref.GUID], pb)
				expBoardName[p.Ref.GUID] = p.BoardName
			}
		}
	}
	// Build expanders[] while still under the lock — both live groups (with
	// their board-kind stamped into `type:`) AND retained ABANDONED boards
	// (configured but disconnected) so a Save never silently drops a board's
	// config.  Carrying the previously-loaded entry verbatim preserves its
	// type, alias, ports, roles + profiles.
	live := a.liveGUIDs()
	for guid, pbs := range expGroups {
		cfg.Expanders = append(cfg.Expanders, yamlExpanderEntry{
			GUID:  guid,
			Alias: a.expanderAlias(guid),
			Type:  devicemodel.BoardKindFromName(expBoardName[guid]),
			Ports: pbs,
		})
	}
	for guid, e := range a.hubExpanders {
		if guid == "" || live[guid] {
			continue // live boards already emitted above
		}
		cp := *e // preserve type / alias / ports / roles / profiles verbatim
		cfg.Expanders = append(cfg.Expanders, cp)
	}
	a.dmMu.Unlock()

	sort.Slice(cfg.Expanders, func(i, j int) bool { return cfg.Expanders[i].GUID < cfg.Expanders[j].GUID })

	// Serialise + upload via temp file (Storage.FileUpload reads from disk).
	data, err := yaml.Marshal(&cfg)
	if err != nil {
		return fmt.Errorf("serialise: %w", err)
	}
	tmp, err := os.CreateTemp("", "hubfx-*.yaml")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if _, werr := tmp.Write(data); werr != nil {
		tmp.Close()
		return werr
	}
	tmp.Close()

	if _, err := c.Storage.FileUpload(tmpPath, client.UploadOptions{
		Path: hubConfigPath, Target: client.TargetFlash, Mode: client.UploadSync,
	}); err != nil {
		return fmt.Errorf("upload /hubfx.yaml: %w", err)
	}
	if err := c.Config.ReloadPath(hubConfigPath); err != nil {
		return fmt.Errorf("reload: %w", err)
	}
	a.diag.Info("CFG", "saved + reloaded /hubfx.yaml (%d inputs · %d hub ports · %d expanders)",
		len(cfg.Inputs), len(cfg.Ports), len(cfg.Expanders))
	// The firmware reload re-attaches every role from scratch, which resets
	// each input role's broadcast rate to 0 — so the live RC bars in Studio
	// would go dead after any save.  Re-arm the per-port input broadcasts so
	// the firmware keeps streaming channel frames to us.
	a.applyInputBroadcasts()
	return nil
}

// yamlKindName is the inverse of kindFromYamlKindName.
func yamlKindName(kind byte) string {
	switch kind {
	case ports.KindServo:
		return "servo"
	case ports.KindPwm:
		return "pwm"
	case ports.KindHBridge:
		return "hbridge"
	case ports.KindInput:
		return "input"
	}
	return ""
}

func kindFromYamlKindName(s string) (byte, bool) {
	switch s {
	case "servo":
		return ports.KindServo, true
	case "pwm":
		return ports.KindPwm, true
	case "hbridge":
		return ports.KindHBridge, true
	case "input":
		return ports.KindInput, true
	}
	return 0, false
}
