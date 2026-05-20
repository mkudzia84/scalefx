package client

import (
	"sort"
	"strings"

	"scalefx/protocol/core"
)

// FeatureGroup labels one cohesive subsystem the firmware can expose
// (audio, storage, topology, …).  Studio consumes this for UI gating —
// show the audio tab only when AudioFeature is supported, etc.
type FeatureGroup string

const (
	FeatureCore     FeatureGroup = "core"
	FeatureStorage  FeatureGroup = "storage"
	FeatureAudio    FeatureGroup = "audio"
	FeatureAlerts   FeatureGroup = "alerts"
	FeatureTopology FeatureGroup = "topology"
	FeatureExpander FeatureGroup = "expanders"
	FeatureLightFx  FeatureGroup = "lightfx"
	FeatureLanding  FeatureGroup = "landing"
	FeatureGear     FeatureGroup = "gear"
	FeatureGun      FeatureGroup = "gun"
	FeatureEngine   FeatureGroup = "engine"
	FeatureBattery  FeatureGroup = "battery"
	FeatureConfig   FeatureGroup = "config"
	FeatureUsbHost  FeatureGroup = "usb_host"
	FeaturePorts    FeatureGroup = "ports"
	FeatureRoles    FeatureGroup = "roles"
)

// FeatureSet derives the high-level facet availability from a raw
// capability bitmask.  Every UI / CLI / orchestrator that wants to ask
// "can this board do X?" should route through this rather than poking
// individual cap bits at the call site.
type FeatureSet struct {
	caps uint32
}

// NewFeatureSet wraps a raw capability bitmask (the value advertised
// in IDENTIFY / INIT_READY).
func NewFeatureSet(caps uint32) FeatureSet { return FeatureSet{caps: caps} }

// Raw returns the underlying capability bitmask.
func (f FeatureSet) Raw() uint32 { return f.caps }

// Has reports whether `group` is supported on this board.
func (f FeatureSet) Has(group FeatureGroup) bool {
	mask := featureMaskTable[group]
	if mask == 0 {
		return false
	}
	// "Any-bit-set" semantics: a group with multiple backing caps (e.g.
	// storage = flash OR sd) lights up when any of them are advertised.
	return f.caps&mask != 0
}

// Names returns every supported feature group in stable display order.
func (f FeatureSet) Names() []string {
	out := make([]string, 0, len(featureOrder))
	for _, g := range featureOrder {
		if f.Has(g) {
			out = append(out, string(g))
		}
	}
	return out
}

// featureMaskTable maps a group → bitmask used for the "Has" check.
// Effects without dedicated bits (LightFx, Landing, Gear, Gun, Alerts)
// proxy through the closest underlying subsystem capability advertised
// by their backing service policy on the firmware:
//
//	LightFx / Landing / Gear / Gun  → CapTopology (they fan out to
//	                                  remote LEDs / servos through
//	                                  the topology service)
//	Alerts                          → CapAudio (rides the master mixer)
//
// When a dedicated bit is added on the firmware side (Rule 11 append),
// flip the entry here.
var featureMaskTable = map[FeatureGroup]uint32{
	FeatureCore:     0xFFFFFFFF, // always on once the board is up
	FeatureStorage:  core.CapFlash | core.CapSd,
	FeatureAudio:    core.CapAudio,
	FeatureAlerts:   core.CapAlerts,
	FeatureTopology: core.CapTopology,
	FeatureExpander: core.CapExpanderBus,
	FeatureLightFx:  core.CapLightFx,
	FeatureLanding:  core.CapLandingLights,
	FeatureGear:     core.CapGearCtrl,
	FeatureGun:      core.CapGunFx,
	FeatureEngine:   core.CapEngine,
	FeatureBattery:  core.CapBattery,
	FeatureConfig:   core.CapConfig,
	FeatureUsbHost:  core.CapUsbHost,
	FeaturePorts:    core.CapPorts,
	FeatureRoles:    core.CapRoles,
}

// featureOrder is the canonical render order — used by Names() and any
// UI that wants a consistent column layout.
var featureOrder = []FeatureGroup{
	FeatureCore,
	FeatureStorage,
	FeatureAudio,
	FeatureAlerts,
	FeatureTopology,
	FeatureExpander,
	FeaturePorts,
	FeatureRoles,
	FeatureLightFx,
	FeatureLanding,
	FeatureGear,
	FeatureGun,
	FeatureEngine,
	FeatureBattery,
	FeatureConfig,
	FeatureUsbHost,
}

// Features returns the FeatureSet derived from `Identity.Capabilities`.
// Convenience for callers holding an Identity (CLI banner, Studio
// after-connect hydrate).
func (i Identity) Features() FeatureSet { return NewFeatureSet(i.Capabilities) }

// FeatureGroups returns the supported feature labels as a sorted slice
// — same data Names() returns, but suitable for emitting through Wails
// to a frontend (always sorted alphabetically for stable diffs).
func (i Identity) FeatureGroups() []string {
	out := i.Features().Names()
	sort.Strings(out)
	return out
}

// AsCSV joins the supported groups with " · " — handy for one-line
// status banners.
func (f FeatureSet) AsCSV() string {
	return strings.Join(f.Names(), " · ")
}
