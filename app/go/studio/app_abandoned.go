package main

// Abandoned-expander support — configured-but-disconnected boards.
//
// The device model is assembled from LIVE topology only (BuildModel), so an
// expander declared in /hubfx.yaml's `expanders:` block but not currently
// plugged in would vanish from the UI AND get silently dropped on the next
// Save (the old SaveHubConfig rebuilt purely from live ports).  That lost all
// the operator's per-port names / roles / profiles on a reconnect.
//
// Here we keep those boards visible: the retained config (a.hubExpanders,
// loaded by LoadHubConfig) is turned into OFFLINE "ghost" ports that surface
// in the Ports & Roles list + the diagram with an OFFLINE warning, and the
// operator can explicitly remove a board (RemoveExpanderConfig).  SaveHubConfig
// preserves any abandoned board it didn't remove.

import (
	"fmt"
	"sort"

	"scalefx/devicemodel"
	"scalefx/protocol/roles"
)

// liveGUIDs is the set of board GUIDs present in the live model. Caller holds dmMu.
func (a *App) liveGUIDs() map[string]bool {
	out := map[string]bool{}
	if a.dm != nil {
		for _, p := range a.dm.Ports {
			out[p.Ref.GUID] = true
		}
	}
	return out
}

// abandonedExpanders returns the retained expander configs whose GUID is NOT
// in the live model — configured boards that aren't currently connected.
// Caller holds dmMu.  Sorted by GUID for stable output.
func (a *App) abandonedExpanders() []*yamlExpanderEntry {
	live := a.liveGUIDs()
	var out []*yamlExpanderEntry
	for guid, e := range a.hubExpanders {
		if guid == "" || live[guid] {
			continue
		}
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].GUID < out[j].GUID })
	return out
}

// expanderAlias returns the operator alias retained for a GUID from the loaded
// /hubfx.yaml (empty if none).  Caller holds dmMu.  Lets a live board keep its
// alias across a Save instead of dropping it.
func (a *App) expanderAlias(guid string) string {
	if e, ok := a.hubExpanders[guid]; ok {
		return e.Alias
	}
	return ""
}

// boardNameForOffline synthesises a BoardName for a ghost board so the
// frontend's boardKindOf / display-name derivation work (the kind must be a
// recognisable prefix — see devicemodel.BoardKindFromName).
func boardNameForOffline(kind, guid, alias string) string {
	if kind != "" {
		return kind + "-" + guid
	}
	if alias != "" {
		return alias
	}
	return "expander-" + guid
}

// offlinePortsFor reconstructs ghost Ports from a retained expander config
// entry, overlaying any operator name / profile edits.  Caller holds dmMu.
func (a *App) offlinePortsFor(e *yamlExpanderEntry) []devicemodel.Port {
	boardName := boardNameForOffline(e.expanderKind(), e.GUID, e.Alias)
	var out []devicemodel.Port
	for _, pb := range e.Ports {
		k, ok := kindFromYamlKindName(pb.Kind)
		if !ok {
			continue
		}
		rk := roles.KindNone
		if pb.Role != "" {
			if r, ok := roles.KindFromName(pb.Role); ok {
				rk = r
			}
		}
		ref := devicemodel.PortRef{GUID: e.GUID, Kind: k, Index: pb.Idx}
		name := pb.Label
		if n, ok := a.portNames[ref]; ok && n != "" {
			name = n
		}
		var prof *devicemodel.ServoMotionProfile
		if pb.Kind == "servo" {
			if pb.Profile != nil {
				p := *pb.Profile
				prof = &p
			} else if dp, ok := a.lookupProfile(ref); ok {
				p := devicemodel.ServoMotionProfile(dp)
				prof = &p
			}
		}
		out = append(out, devicemodel.OfflinePort(e.GUID, k, pb.Idx, rk, boardName, name, prof))
	}
	return out
}

// appendOfflinePorts adds ghost ports + a warning issue for every abandoned
// expander to a snapshot under construction.  Caller holds dmMu.
func (a *App) appendOfflinePorts(snap *DeviceModelSnapshot) {
	for _, e := range a.abandonedExpanders() {
		ghosts := a.offlinePortsFor(e)
		if len(ghosts) == 0 {
			continue
		}
		snap.Ports = append(snap.Ports, ghosts...)
		name := boardNameForOffline(e.expanderKind(), e.GUID, e.Alias)
		snap.Issues = append(snap.Issues, devicemodel.Issue{
			Severity: devicemodel.SevWarn,
			Message: fmt.Sprintf("%s is configured in /hubfx.yaml but not connected — "+
				"its ports are shown from saved config (remove it, or reconnect the board).", name),
		})
	}
}

// RemoveExpanderConfig drops an expander's entry from the retained config +
// its overlays so the next Save writes /hubfx.yaml WITHOUT it.  The change is
// in-memory (the frontend marks the hub config dirty so the global toolbar's
// Apply persists it).  Returns the post-removal snapshot.
func (a *App) RemoveExpanderConfig(guid string) (DeviceModelSnapshot, error) {
	if guid == "" {
		return a.deviceModelSnapshot(), fmt.Errorf("empty guid")
	}
	defer a.diag.Around("RemoveExpanderConfig", map[string]any{"guid": guid})()
	a.dmMu.Lock()
	delete(a.hubExpanders, guid)
	for ref := range a.portNames {
		if ref.GUID == guid {
			delete(a.portNames, ref)
		}
	}
	for ref := range a.portProfiles {
		if ref.GUID == guid {
			delete(a.portProfiles, ref)
		}
	}
	a.dmMu.Unlock()
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot(), nil
}
