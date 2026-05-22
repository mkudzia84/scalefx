package devicemodel

import (
	"embed"
	"fmt"
	"io/fs"
	"strings"

	"scalefx/protocol/ports"
	"scalefx/protocol/roles"

	"gopkg.in/yaml.v3"
)

// Default device profiles are bundled as YAML in defaults/ and embedded
// into the binary.  Each is keyed by the board kind it applies to (the
// filename, e.g. hubfx.yaml).  A profile carries default role
// attachments, domain claims, and per-port input configuration the
// operator can apply on connect as a sensible starting point.

//go:embed defaults/*.yaml
var defaultsFS embed.FS

// yamlPortRef is the on-disk symbolic port reference (board selector +
// kind name + index).  Mirrors PresetPortRef but in string-kind form for
// human authoring.
type yamlPortRef struct {
	Board string `yaml:"board"`
	Kind  string `yaml:"kind"`
	Index byte   `yaml:"index"`
}

type yamlRole struct {
	yamlPortRef `yaml:",inline"`
	Role        string `yaml:"role"`
}

type yamlClaim struct {
	yamlPortRef `yaml:",inline"`
	Domain      string `yaml:"domain"`
	Slot        string `yaml:"slot"`
}

type yamlInput struct {
	Protocol string `yaml:"protocol"`
	Channels int    `yaml:"channels"`
	Map      []struct {
		Channel  int    `yaml:"channel"`
		Function string `yaml:"function"`
	} `yaml:"map"`
}

type yamlProfile struct {
	Name        string      `yaml:"name"`
	Description string      `yaml:"description"`
	Input       *yamlInput  `yaml:"input"`
	Roles       []yamlRole  `yaml:"roles"`
	Claims      []yamlClaim `yaml:"claims"`
}

// Profile is a parsed default profile, translated into the in-memory
// model vocabulary (a Preset for the role/claim side + an optional input
// config the host applies to the matching input port).
type Profile struct {
	BoardKind   string
	Name        string
	Description string
	Preset      Preset           // role assigns + claims (symbolic)
	Input       *InputPortConfig // input config for the FIRST input port (port filled in by host)
}

var kindFromName = map[string]byte{
	"servo": ports.KindServo, "pwm": ports.KindPwm,
	"hbridge": ports.KindHBridge, "input": ports.KindInput,
}

// loadProfiles parses every embedded YAML profile once.
func loadProfiles() (map[string]Profile, error) {
	out := map[string]Profile{}
	entries, err := fs.ReadDir(defaultsFS, "defaults")
	if err != nil {
		return nil, err
	}
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".yaml") {
			continue
		}
		kind := strings.TrimSuffix(e.Name(), ".yaml")
		raw, err := defaultsFS.ReadFile("defaults/" + e.Name())
		if err != nil {
			return nil, err
		}
		var yp yamlProfile
		if err := yaml.Unmarshal(raw, &yp); err != nil {
			return nil, fmt.Errorf("%s: %w", e.Name(), err)
		}
		out[kind] = translateProfile(kind, yp)
	}
	return out, nil
}

func translateProfile(kind string, yp yamlProfile) Profile {
	p := Profile{
		BoardKind:   kind,
		Name:        yp.Name,
		Description: yp.Description,
		Preset:      Preset{Name: yp.Name, Description: yp.Description},
	}
	for _, r := range yp.Roles {
		rk, _ := roles.KindFromName(r.Role)
		p.Preset.Roles = append(p.Preset.Roles, RoleAssign{
			Port:     PresetPortRef{Board: BoardSel(r.Board), Kind: kindFromName[r.Kind], Index: r.Index},
			RoleKind: rk,
		})
	}
	for _, c := range yp.Claims {
		p.Preset.Claims = append(p.Preset.Claims, PresetClaim{
			Domain: DomainID(c.Domain),
			Slot:   c.Slot,
			Port:   PresetPortRef{Board: BoardSel(c.Board), Kind: kindFromName[c.Kind], Index: c.Index},
		})
	}
	if yp.Input != nil {
		ic := &InputPortConfig{
			Protocol:     InputProtocol(yp.Input.Protocol),
			ChannelCount: yp.Input.Channels,
		}
		if ic.ChannelCount == 0 {
			ic.ChannelCount = len(yp.Input.Map)
		}
		for _, m := range yp.Input.Map {
			ic.Channels = append(ic.Channels, ChannelMap{Channel: m.Channel, Function: m.Function})
		}
		p.Input = ic
	}
	return p
}

var profileCache map[string]Profile

// Profiles returns every embedded default profile, keyed by board kind.
func Profiles() map[string]Profile {
	if profileCache == nil {
		if p, err := loadProfiles(); err == nil {
			profileCache = p
		} else {
			profileCache = map[string]Profile{}
		}
	}
	return profileCache
}

// ProfileFor returns the default profile for a board kind, if bundled.
func ProfileFor(boardKind string) (Profile, bool) {
	p, ok := Profiles()[boardKind]
	return p, ok
}
