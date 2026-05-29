package main

// Input-side bindings — the left column of the Input & Ports tab.
// Per-input-port protocol + channel count + channel→function mapping, the
// "apply sensible defaults" action, and the live channel-value stream the
// colourful bars render.
//
// Live values: on connect the hub is told to broadcast RC PWM values
// (SetBroadcastHz) for every input port carrying an rc-pwm-input role; the
// client decodes the frames and we forward them to the frontend as
// `input:values` events.

import (
	"fmt"

	"scalefx/client"
	"scalefx/devicemodel"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// kInputBroadcastHz is the channel cadence requested from the hub.  It drives
// BOTH the live-bar refresh AND the on-board InputDispatcher that feeds the
// effect triggers (the local dispatch fires off the same broadcast tick), so
// it's set to 50 Hz to match the firmware's standalone default and keep RC
// effect response snappy.  At 6 Mbps the ~2 KB/s async stream is negligible
// against synchronous topology/status RPCs, and the verbose gate stops it
// entirely when no host is listening.
const kInputBroadcastHz = 50

// ensureInputConfigs creates a default InputPortConfig for every input
// port in the model that doesn't have one yet.  Caller holds dmMu.
func (a *App) ensureInputConfigs() {
	if a.dm == nil {
		return
	}
	for _, p := range a.dm.Ports {
		if p.Direction != devicemodel.DirInput {
			continue
		}
		cfg, ok := a.inputs[p.Ref]
		if !ok {
			c := devicemodel.NewInputPortConfig(p.Ref)
			a.inputs[p.Ref] = &c
			cfg = &c
		}
		// The attached role IS the protocol — derive the dropdown selection
		// from live topology so a board that booted with jeti-ex-input (from
		// /hubfx.yaml) shows "Jeti EX", not the PPM default.  Only override
		// when the port actually has an input role attached.
		if proto, ok := devicemodel.ProtocolByRoleKind(p.RoleKind); ok && proto != devicemodel.InputNone {
			cfg.Protocol = proto
		}
	}
}

// installInputStream subscribes to decoded input frames and forwards them
// to the frontend.  Called once per connect (fresh client → fresh
// subscriber list).
func (a *App) installInputStream() {
	if a.c == nil {
		return
	}
	a.c.Events.OnInputValue(func(v client.InputValue) {
		if a.ctx == nil {
			return
		}
		// Hub-local frames decode with GUID "" (the wire has no GUID for the
		// master's own port), but the device model — and the frontend's
		// liveChannelKey / autoExpand / detectedCount — key off the hub's
		// GUID.  Remap so the live bars actually find their values.
		if v.GUID == "" {
			a.mu.Lock()
			v.GUID = a.id.GUID
			a.mu.Unlock()
		}
		wailsRT.EventsEmit(a.ctx, "input:values", v)
	})
}

// startInputBroadcasts asks the hub to stream RC values for every hub-local
// input port carrying an input role — PPM, SBUS, or Jeti EX.  Each protocol
// has its own broadcast-enable packet, so we dispatch by role kind (a Jeti
// role attached but left on the PPM enable would never stream).  Snapshots
// the client + input ports under lock, then issues the wire commands
// lock-free.
func (a *App) startInputBroadcasts() {
	c := a.snapshotClient()
	if c == nil {
		return
	}
	type inputBcast struct {
		idx  byte
		kind byte
	}
	// Hub-local input ports are tagged with the HUB's GUID (not ""), so a
	// `GUID == ""` filter matches nothing — which is why Studio's live bars
	// never armed.  Accept both the empty GUID and the hub's own GUID.
	a.mu.Lock()
	hubGUID := a.id.GUID
	a.mu.Unlock()
	a.dmMu.Lock()
	var ins []inputBcast
	if a.dm != nil {
		for _, p := range a.dm.Ports {
			if p.Ref.Kind != ports.KindInput {
				continue
			}
			if p.Ref.GUID != "" && p.Ref.GUID != hubGUID {
				continue // expander input — armed via its own path, not here
			}
			switch p.RoleKind {
			case roles.KindRcPwmInput, roles.KindSbusInput, roles.KindJetiExInput:
				ins = append(ins, inputBcast{idx: p.Ref.Index, kind: p.RoleKind})
			}
		}
	}
	a.dmMu.Unlock()
	for _, in := range ins {
		var err error
		switch in.kind {
		case roles.KindSbusInput:
			err = c.Input.SetSbusBroadcastHz(in.idx, kInputBroadcastHz)
		case roles.KindJetiExInput:
			err = c.Input.SetJetiBroadcastHz(in.idx, kInputBroadcastHz)
		default: // KindRcPwmInput
			err = c.Input.SetBroadcastHz(in.idx, kInputBroadcastHz)
		}
		if err != nil {
			a.diag.Warn("INPUT", "set broadcast hz on input %d (role %d): %v", in.idx, in.kind, err)
		}
	}
}

// ─── Global RC-routing gate ───────────────────────────────────────────

// SetInputRouting toggles the master's global RC→effect routing gate.
// enabled=true (default) = RC drives effects; false = effects ignore RC
// and hold their last commanded state (drive them from Studio).  Live
// channel monitors keep updating either way.  Bound to the ConfigToolbar
// toggle next to the sync indicator.
func (a *App) SetInputRouting(enabled bool) error {
	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}
	a.diag.Info("INPUT", "SetInputRouting → %v", enabled)
	if err := c.Input.SetRouting(enabled); err != nil {
		a.diag.Error("INPUT", "SetInputRouting failed: %v", err)
		return err
	}
	return nil
}

// GetInputRouting reads the current routing-gate state (called on connect
// so the toggle reflects the device).
func (a *App) GetInputRouting() (bool, error) {
	c := a.snapshotClient()
	if c == nil {
		return false, fmt.Errorf("not connected")
	}
	return c.Input.GetRouting()
}

// ─── Config mutations ─────────────────────────────────────────────────

func (a *App) inputCfg(guid string, kind, index byte) *devicemodel.InputPortConfig {
	ref := devicemodel.PortRef{GUID: guid, Kind: kind, Index: index}
	cfg, ok := a.inputs[ref]
	if !ok {
		c := devicemodel.NewInputPortConfig(ref)
		a.inputs[ref] = &c
		cfg = &c
	}
	return cfg
}

// SetInputProtocol sets an input port's protocol.  PPM attaches the
// rc-pwm-input role and starts the value stream; SBUS / Jeti are not yet
// implemented and are rejected.
func (a *App) SetInputProtocol(guid string, kind, index byte, protocol string) (DeviceModelSnapshot, error) {
	defer a.diag.Around("SetInputProtocol",
		map[string]any{"guid": guid, "kind": kind, "idx": index, "protocol": protocol})()
	a.diag.Info("INPUT", "SetInputProtocol guid=%s kind=%d idx=%d protocol=%s",
		guid, kind, index, protocol)
	def, ok := devicemodel.ProtocolByID(devicemodel.InputProtocol(protocol))
	if !ok {
		a.diag.Error("INPUT", "SetInputProtocol: unknown protocol %q", protocol)
		return a.deviceModelSnapshot(), fmt.Errorf("unknown protocol %q", protocol)
	}
	if !def.Implemented {
		a.diag.Warn("INPUT", "SetInputProtocol: %s not implemented yet", def.Label)
		return a.deviceModelSnapshot(), fmt.Errorf("%s is not implemented yet", def.Label)
	}
	a.dmMu.Lock()
	a.inputCfg(guid, kind, index).Protocol = def.ID
	a.dmMu.Unlock()

	// Attach the realizing role + start broadcasting.
	if _, err := a.AttachRole(guid, kind, index, def.RoleKind); err != nil {
		a.diag.Error("INPUT", "SetInputProtocol: AttachRole failed: %v", err)
		return a.deviceModelSnapshot(), err
	}
	a.startInputBroadcasts()
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot(), nil
}

// SetInputChannelCount resizes an input port's channel map.
func (a *App) SetInputChannelCount(guid string, kind, index byte, count int) DeviceModelSnapshot {
	defer a.diag.Around("SetInputChannelCount",
		map[string]any{"guid": guid, "kind": kind, "idx": index, "count": count})()
	a.diag.Info("INPUT", "SetInputChannelCount guid=%s kind=%d idx=%d count=%d",
		guid, kind, index, count)
	a.dmMu.Lock()
	a.inputCfg(guid, kind, index).SetChannelCount(count)
	a.dmMu.Unlock()
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot()
}

// SetChannelFunction assigns a logical function to a channel.
func (a *App) SetChannelFunction(guid string, kind, index byte, channel int, fn string) DeviceModelSnapshot {
	defer a.diag.Around("SetChannelFunction",
		map[string]any{"guid": guid, "kind": kind, "idx": index, "channel": channel, "fn": fn})()
	a.diag.Info("INPUT", "SetChannelFunction guid=%s kind=%d idx=%d ch=%d fn=%s",
		guid, kind, index, channel, fn)
	a.dmMu.Lock()
	a.inputCfg(guid, kind, index).SetFunction(channel, fn)
	a.dmMu.Unlock()
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot()
}

// ─── Defaults ─────────────────────────────────────────────────────────

// ApplyDefaults applies the bundled default profile for the hub kind and
// each attached board kind: role attachments, domain claims, and input
// channel maps.  Returns the snapshot plus any per-item warnings.
func (a *App) ApplyDefaults() (DeviceModelSnapshot, []string, error) {
	c := a.snapshotClient()
	if c == nil {
		return DeviceModelSnapshot{}, nil, fmt.Errorf("not connected")
	}

	// Which board kinds are present (hub + expanders), de-duplicated.
	kinds := map[string]bool{a.kind: true}
	a.dmMu.Lock()
	for _, p := range a.dm.Ports {
		kinds[boardKindFromName(p.BoardName)] = true
	}
	a.dmMu.Unlock()

	var warnings []string
	var assigns []devicemodel.ResolvedAssign
	for kind := range kinds {
		prof, ok := devicemodel.ProfileFor(kind)
		if !ok {
			continue
		}
		a.dmMu.Lock()
		if a.dm != nil {
			as, w, err := a.dm.ApplyPreset(prof.Preset)
			assigns = append(assigns, as...)
			warnings = append(warnings, w...)
			if err != nil {
				warnings = append(warnings, fmt.Sprintf("%s: %v", kind, err))
			}
			// Apply the input channel map to the matching input port(s).
			if prof.Input != nil {
				for _, p := range a.dm.Ports {
					if p.Ref.Kind == ports.KindInput && boardKindFromName(p.BoardName) == kind {
						cfg := devicemodel.NewInputPortConfig(p.Ref)
						cfg.Protocol = prof.Input.Protocol
						cfg.SetChannelCount(prof.Input.ChannelCount)
						for _, m := range prof.Input.Channels {
							cfg.SetFunction(m.Channel, m.Function)
						}
						a.inputs[p.Ref] = &cfg
					}
				}
			}
		}
		a.dmMu.Unlock()
	}

	// Push resolved role attachments to the wire.
	for _, as := range assigns {
		if err := c.Topology.AttachRole(as.Port.GUID, as.Port.Kind, as.Port.Index, as.RoleKind, nil); err != nil {
			warnings = append(warnings, fmt.Sprintf("attach %s on %s: %v", as.RoleName, as.Port, err))
		}
	}
	a.startInputBroadcasts()
	a.diag.Info("INPUT", "applied defaults (%d roles, %d warnings)", len(assigns), len(warnings))
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot(), warnings, nil
}

// boardKindFromName derives a board kind from a device-name prefix
// (mirrors client.Identity.Kind for arbitrary names).
func boardKindFromName(name string) string {
	switch {
	case hasPrefixFold(name, "HubFx"):
		return "hubfx"
	case hasPrefixFold(name, "LightFx"):
		return "lightfx"
	case hasPrefixFold(name, "GunFx"):
		return "gunfx"
	case hasPrefixFold(name, "GearControl"):
		return "gearcontrol"
	case hasPrefixFold(name, "NoOp"):
		return "noop"
	}
	return ""
}

func hasPrefixFold(s, prefix string) bool {
	if len(s) < len(prefix) {
		return false
	}
	for i := 0; i < len(prefix); i++ {
		a, b := s[i], prefix[i]
		if 'A' <= a && a <= 'Z' {
			a += 'a' - 'A'
		}
		if 'A' <= b && b <= 'Z' {
			b += 'a' - 'A'
		}
		if a != b {
			return false
		}
	}
	return true
}
