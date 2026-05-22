package main

// Device-model bindings — the backbone for the Port/Role setup tab, the
// Input configuration tab, and every functional domain tab.  Studio holds
// ONE working devicemodel.Model assembled from the hub's topology
// snapshot (PORT_LIST + ROLE_LIST).  The frontend renders it, edits claims
// against it, validates, and applies presets — all through these bindings.
//
// Concurrency: the model is guarded by dmMu (short critical sections).
// Wire I/O (PortList / RoleList / RoleAttach) is done on a client pointer
// snapshotted under a.mu and released BEFORE the call, so a multi-second
// transfer never blocks the connection mutex or the model mutex.

import (
	"fmt"

	"scalefx/client"
	"scalefx/devicemodel"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// ─── Frontend DTOs ────────────────────────────────────────────────────

// DeviceModelSnapshot is the full model state the frontend renders.
type DeviceModelSnapshot struct {
	Ports   []devicemodel.Port   `json:"ports"`
	Claims  []devicemodel.Claim  `json:"claims"`
	Domains []devicemodel.Domain `json:"domains"`
	Issues  []devicemodel.Issue  `json:"issues"`
	Roles   []RoleKindInfo       `json:"roleCatalog"`
	Presets []devicemodel.Preset `json:"presets"`

	// Input side (left column of the Input & Ports tab).
	Inputs           []devicemodel.InputPortConfig    `json:"inputs"`
	ChannelFunctions []devicemodel.ChannelFunctionDef `json:"channelFunctions"`
	InputProtocols   []devicemodel.InputProtocolDef   `json:"inputProtocols"`
}

// RoleKindInfo is a (id, wire-name, friendly-label) tuple for role UIs.
type RoleKindInfo struct {
	Kind  byte   `json:"kind"`
	Name  string `json:"name"`
	Label string `json:"label"`
}

// roleCatalog lists every attachable role kind with its human label.
func roleCatalog() []RoleKindInfo {
	kinds := []byte{
		roles.KindServoActuator, roles.KindRcPwmInput, roles.KindSbusInput,
		roles.KindJetiExInput, roles.KindLedAnimator, roles.KindDcMotor,
		roles.KindHeater, roles.KindBiDcMotor,
	}
	out := make([]RoleKindInfo, len(kinds))
	for i, k := range kinds {
		out[i] = RoleKindInfo{Kind: k, Name: roles.KindName(k), Label: devicemodel.RoleLabel(k)}
	}
	return out
}

// ─── Snapshot assembly ────────────────────────────────────────────────

// snapshotClient returns the live client (or nil) without holding a.mu
// across the caller's wire I/O.
func (a *App) snapshotClient() *client.Client {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.c
}

func (a *App) hubCaps() uint32 {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.id.Capabilities
}

// RefreshDeviceModel re-fetches the topology from the hub and rebuilds the
// working model, preserving the existing claim set (claims survive a
// refresh as long as their ports still exist; stale claims surface as
// validation errors rather than vanishing silently).  Returns the new
// snapshot.
func (a *App) RefreshDeviceModel() (DeviceModelSnapshot, error) {
	defer a.diag.Around("RefreshDeviceModel", nil)()
	c := a.snapshotClient()
	if c == nil {
		return DeviceModelSnapshot{}, fmt.Errorf("not connected")
	}
	boardPorts, err := c.Topology.PortList("")
	if err != nil {
		// A transient topology timeout (e.g. a slow expander forward) must
		// NOT blank the UI or surface a hard error — keep the previous
		// model and report it as a soft warning.
		a.diag.Warn("DM", "port list failed (keeping previous model): %v", err)
		return a.deviceModelSnapshot(), nil
	}
	boardRoles, err := c.Topology.RoleList("")
	if err != nil {
		// Roles are optional (a board without RoleServicePolicy) — log and
		// continue with an empty role set rather than failing the refresh.
		a.diag.Warn("DM", "role list failed (continuing without roles): %v", err)
		boardRoles = nil
	}

	a.dmMu.Lock()
	prevClaims := []devicemodel.Claim(nil)
	if a.dm != nil {
		prevClaims = a.dm.Claims
	}
	a.dm = devicemodel.BuildModel(boardPorts, boardRoles)
	a.dm.Claims = prevClaims
	a.ensureInputConfigs()
	a.dmMu.Unlock()

	// Side-effects (servo auto-attach, input broadcast start) run in the
	// background so a slow attach can't block or time-out the refresh
	// itself.  autoAttachServos re-emits the model when it changes roles.
	go func() {
		a.autoAttachServos()
		a.startInputBroadcasts()
	}()
	return a.deviceModelSnapshot(), nil
}

// autoAttachServos attaches the servo-actuator role to every servo port
// that has no role yet (servos host only that role).  Snapshots the
// targets under lock, then issues the wire attaches lock-free and updates
// the model.
func (a *App) autoAttachServos() {
	c := a.snapshotClient()
	if c == nil {
		return
	}
	a.dmMu.Lock()
	var targets []devicemodel.PortRef
	if a.dm != nil {
		for _, p := range a.dm.Ports {
			if p.Ref.Kind == ports.KindServo && p.RoleKind == roles.KindNone {
				targets = append(targets, p.Ref)
			}
		}
	}
	a.dmMu.Unlock()
	if len(targets) == 0 {
		return
	}
	for _, ref := range targets {
		if err := c.Topology.AttachRole(ref.GUID, ref.Kind, ref.Index, roles.KindServoActuator, nil); err != nil {
			a.diag.Warn("DM", "auto-attach servo on %s: %v", ref, err)
			continue
		}
		a.dmMu.Lock()
		if a.dm != nil {
			a.dm.SetRole(ref, roles.KindServoActuator)
		}
		a.dmMu.Unlock()
	}
	// Ran in the background — push the updated model to the frontend.
	a.emitDeviceModelChanged()
}

// DeviceModelSnapshot returns the cached model without hitting the wire.
// Builds an empty snapshot (catalogs only) when nothing is loaded yet, so
// the frontend can render the domain/role/preset catalogs pre-connect.
func (a *App) DeviceModelSnapshot() DeviceModelSnapshot {
	return a.deviceModelSnapshot()
}

func (a *App) deviceModelSnapshot() DeviceModelSnapshot {
	snap := DeviceModelSnapshot{
		// Always non-nil so they marshal as JSON [] rather than null —
		// the frontend iterates these in derived stores, and `for..of
		// null` throws and breaks Svelte reactivity.
		Ports:            []devicemodel.Port{},
		Claims:           []devicemodel.Claim{},
		Issues:           []devicemodel.Issue{},
		Domains:          devicemodel.AvailableDomains(a.hubCaps()),
		Roles:            roleCatalog(),
		Presets:          devicemodel.Presets(),
		Inputs:           []devicemodel.InputPortConfig{},
		ChannelFunctions: devicemodel.ChannelFunctions(),
		InputProtocols:   devicemodel.InputProtocols(),
	}
	a.dmMu.Lock()
	defer a.dmMu.Unlock()
	if a.dm != nil {
		// Copy ports so we can overlay operator-assigned names without
		// mutating the model.
		if len(a.dm.Ports) > 0 {
			snap.Ports = make([]devicemodel.Port, len(a.dm.Ports))
			copy(snap.Ports, a.dm.Ports)
			for i := range snap.Ports {
				snap.Ports[i].Name = a.portNames[snap.Ports[i].Ref]
			}
		}
		if a.dm.Claims != nil {
			snap.Claims = a.dm.Claims
		}
		if iss := a.dm.Validate(); iss != nil {
			snap.Issues = iss
		}
	}
	// Input configs are ordered by port for stable rendering.
	for _, p := range snap.Ports {
		if p.Direction != devicemodel.DirInput {
			continue
		}
		if cfg, ok := a.inputs[p.Ref]; ok {
			snap.Inputs = append(snap.Inputs, *cfg)
		}
	}
	return snap
}

// ─── Claim editing ────────────────────────────────────────────────────

// ClaimPort claims a port for a domain slot.  Returns the post-mutation
// snapshot (with fresh validation issues) or an error if the claim is
// illegal.
func (a *App) ClaimPort(domain, slot, guid string, kind, index byte) (DeviceModelSnapshot, error) {
	a.dmMu.Lock()
	if a.dm == nil {
		a.dmMu.Unlock()
		return DeviceModelSnapshot{}, fmt.Errorf("device model not loaded")
	}
	err := a.dm.Claim(devicemodel.Claim{
		Domain: devicemodel.DomainID(domain),
		Slot:   slot,
		Port:   devicemodel.PortRef{GUID: guid, Kind: kind, Index: index},
	})
	a.dmMu.Unlock()
	if err != nil {
		return a.deviceModelSnapshot(), err
	}
	a.diag.Info("DM", "claim %s/%s ← %s/%s%d", domain, slot, guidOrHub(guid), ports.KindName(kind), index)
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot(), nil
}

// UnclaimPort removes a claim.
func (a *App) UnclaimPort(domain, slot, guid string, kind, index byte) DeviceModelSnapshot {
	a.dmMu.Lock()
	if a.dm != nil {
		a.dm.Unclaim(devicemodel.Claim{
			Domain: devicemodel.DomainID(domain),
			Slot:   slot,
			Port:   devicemodel.PortRef{GUID: guid, Kind: kind, Index: index},
		})
	}
	a.dmMu.Unlock()
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot()
}

// SetPortName assigns an operator-friendly name to a port (overlay state).
func (a *App) SetPortName(guid string, kind, index byte, name string) DeviceModelSnapshot {
	ref := devicemodel.PortRef{GUID: guid, Kind: kind, Index: index}
	a.dmMu.Lock()
	if name == "" {
		delete(a.portNames, ref)
	} else {
		a.portNames[ref] = name
	}
	a.dmMu.Unlock()
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot()
}

// CandidatePorts returns the ports a domain may select for a slot — the
// data behind each functional tab's port picker.
func (a *App) CandidatePorts(domain, slot string) []devicemodel.Port {
	a.dmMu.Lock()
	defer a.dmMu.Unlock()
	if a.dm == nil {
		return []devicemodel.Port{}
	}
	if c := a.dm.Candidates(devicemodel.DomainID(domain), slot); c != nil {
		return c
	}
	return []devicemodel.Port{}
}

// ─── Role attach / detach (wire) ──────────────────────────────────────

// AttachRole binds a role to a port on the wire (TOPOLOGY_ROLE_ATTACH),
// then updates the working model so dependent claims validate.  cfg is
// left empty (role defaults); per-role config comes from the functional
// tabs / config files.
func (a *App) AttachRole(guid string, kind, index, roleKind byte) (DeviceModelSnapshot, error) {
	c := a.snapshotClient()
	if c == nil {
		return DeviceModelSnapshot{}, fmt.Errorf("not connected")
	}
	if err := c.Topology.AttachRole(guid, kind, index, roleKind, nil); err != nil {
		return a.deviceModelSnapshot(), fmt.Errorf("attach %s: %w", roles.KindName(roleKind), err)
	}
	a.dmMu.Lock()
	if a.dm != nil {
		a.dm.SetRole(devicemodel.PortRef{GUID: guid, Kind: kind, Index: index}, roleKind)
	}
	a.dmMu.Unlock()
	a.diag.Info("DM", "attach %s → %s/%s%d", roles.KindName(roleKind), guidOrHub(guid), ports.KindName(kind), index)
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot(), nil
}

// DetachRole clears a port's role on the wire and in the model, and drops
// any claims that referenced it.
func (a *App) DetachRole(guid string, kind, index byte) (DeviceModelSnapshot, error) {
	c := a.snapshotClient()
	if c == nil {
		return DeviceModelSnapshot{}, fmt.Errorf("not connected")
	}
	if err := c.Topology.DetachRole(guid, kind, index); err != nil {
		return a.deviceModelSnapshot(), fmt.Errorf("detach: %w", err)
	}
	ref := devicemodel.PortRef{GUID: guid, Kind: kind, Index: index}
	a.dmMu.Lock()
	if a.dm != nil {
		a.dm.SetRole(ref, roles.KindNone)
		for _, cl := range a.dm.ClaimsForPort(ref) {
			a.dm.Unclaim(cl)
		}
	}
	a.dmMu.Unlock()
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot(), nil
}

// ─── Presets ──────────────────────────────────────────────────────────

// ApplyPreset resolves a named preset against the live topology, pushes
// its role attachments to the wire, registers its claims, and returns the
// resulting snapshot plus any per-item warnings (boards/ports the preset
// referenced that aren't present are skipped, not fatal).
func (a *App) ApplyPreset(name string) (DeviceModelSnapshot, []string, error) {
	preset, ok := devicemodel.PresetByName(name)
	if !ok {
		return DeviceModelSnapshot{}, nil, fmt.Errorf("unknown preset %q", name)
	}
	c := a.snapshotClient()
	if c == nil {
		return DeviceModelSnapshot{}, nil, fmt.Errorf("not connected")
	}

	a.dmMu.Lock()
	if a.dm == nil {
		a.dmMu.Unlock()
		return DeviceModelSnapshot{}, nil, fmt.Errorf("device model not loaded")
	}
	assigns, warnings, err := a.dm.ApplyPreset(preset)
	a.dmMu.Unlock()
	if err != nil {
		return a.deviceModelSnapshot(), warnings, err
	}

	// Push the resolved role attachments to the wire (best-effort; a
	// failed attach becomes a warning so partial application is visible).
	for _, as := range assigns {
		if perr := c.Topology.AttachRole(as.Port.GUID, as.Port.Kind, as.Port.Index, as.RoleKind, nil); perr != nil {
			warnings = append(warnings, fmt.Sprintf("attach %s on %s: %v", as.RoleName, as.Port, perr))
		}
	}
	a.diag.Info("DM", "applied preset %q (%d roles, %d warnings)", name, len(assigns), len(warnings))
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot(), warnings, nil
}

// ─── Events ───────────────────────────────────────────────────────────

func (a *App) emitDeviceModelChanged() {
	if a.ctx == nil {
		return
	}
	wailsRT.EventsEmit(a.ctx, "devicemodel:changed", a.deviceModelSnapshot())
}

func guidOrHub(guid string) string {
	if guid == "" {
		return "hub"
	}
	return guid
}
