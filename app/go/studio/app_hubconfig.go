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

type yamlPortBinding struct {
	Port yamlPortRef `yaml:"port"`
	Role string      `yaml:"role,omitempty"`
	Name string      `yaml:"name,omitempty"`
}

type yamlExpanderEntry struct {
	Alias string            `yaml:"alias"`
	GUID  string            `yaml:"guid"`
	Ports []yamlPortBinding `yaml:"ports,omitempty"`
}

type hubYamlConfig struct {
	SchemaVersion int                 `yaml:"schema_version,omitempty"`
	Inputs        []yamlInputBinding  `yaml:"inputs,omitempty"`
	Ports         []yamlPortBinding   `yaml:"ports,omitempty"`
	Expanders     []yamlExpanderEntry `yaml:"expanders,omitempty"`
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

	a.mu.Lock()
	hubGUID := a.id.GUID
	a.mu.Unlock()

	a.dmMu.Lock()
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

	// ── ports[] → operator-friendly name overlay (hub-local). ──────
	for _, pb := range cfg.Ports {
		if pb.Name == "" {
			continue
		}
		kind, ok := kindFromYamlKindName(pb.Port.Kind)
		if !ok {
			continue
		}
		a.portNames[devicemodel.PortRef{GUID: hubGUID, Kind: kind, Index: pb.Port.Idx}] = pb.Name
	}

	// ── expanders[].ports[] → per-expander port names. ─────────────
	for _, exp := range cfg.Expanders {
		if exp.GUID == "" {
			continue
		}
		for _, pb := range exp.Ports {
			if pb.Name == "" {
				continue
			}
			kind, ok := kindFromYamlKindName(pb.Port.Kind)
			if !ok {
				continue
			}
			a.portNames[devicemodel.PortRef{GUID: exp.GUID, Kind: kind, Index: pb.Port.Idx}] = pb.Name
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

	a.mu.Lock()
	hubGUID := a.id.GUID
	a.mu.Unlock()

	cfg := hubYamlConfig{SchemaVersion: 1}

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
	type pbKey struct{ guid string }
	expGroups := map[string][]yamlPortBinding{}
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
			if name == "" && role == "" {
				continue
			}
			pb := yamlPortBinding{
				Port: yamlPortRef{Kind: yamlKindName(p.Ref.Kind), Idx: p.Ref.Index},
				Role: role,
				Name: name,
			}
			if p.Ref.GUID == hubGUID || p.Ref.GUID == "" {
				cfg.Ports = append(cfg.Ports, pb)
			} else {
				expGroups[p.Ref.GUID] = append(expGroups[p.Ref.GUID], pb)
			}
		}
	}
	a.dmMu.Unlock()

	for guid, pbs := range expGroups {
		cfg.Expanders = append(cfg.Expanders, yamlExpanderEntry{GUID: guid, Ports: pbs})
	}
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
