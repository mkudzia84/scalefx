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
	"scalefx/protocol/input"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// kInputBroadcastHz is the channel cadence requested from the hub for the
// host live-channel view (the colourful bars).  The firmware feeds its own
// on-board InputDispatcher (effect triggers) at a fixed rate REGARDLESS of
// this subscription — so the model flies with no host attached, and the wire
// broadcast is purely a live-view convenience.  We therefore subscribe ONLY
// while a tab that renders the bars is on screen (SetInputLiveView) and
// unsubscribe (hz=0) otherwise, so a connected-but-not-viewing host doesn't
// pay a continuous 50 Hz stream.  At 6 Mbps the ~2 KB/s stream is negligible,
// but leaving it on permanently floods the host drain on every RPC.
const kInputBroadcastHz = 50

// liveHz returns the cadence to request given the current live-view state:
// 50 Hz when a live-channel tab is on screen, 0 (unsubscribe) otherwise.
// Caller must NOT hold a.mu.
func (a *App) liveHz() uint8 {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.liveView {
		return kInputBroadcastHz
	}
	return 0
}

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
			// NEVER invent an EscProtocol default here: this runs on every
			// topology refresh, potentially BEFORE the /hubfx.yaml hydration,
			// and a synthetic "jeti-exbus" would then be PERSISTED by the
			// next Apply — silently overwriting the operator's real stream
			// selection (bench 2026-07-15: a saved kontronik flipped back to
			// jeti-exbus).  An empty value renders as the jeti-exbus fallback
			// in the UI and is simply omitted from the yaml on save.
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
		// Hub-local frames decode with GUID "" — now the canonical hub-local
		// form the device model + frontend (liveChannelKey / autoExpand /
		// detectedCount) key by (instructions/31), so no remap is needed: ""
		// already matches the port refs everywhere.
		wailsRT.EventsEmit(a.ctx, "input:values", v)
	})
	// Generic connection-loss state changes (item 5) → the IO-tab link indicator.
	a.c.Events.OnConnectionEvent(func(ev input.ConnectionEventT) {
		if a.ctx == nil {
			return
		}
		wailsRT.EventsEmit(a.ctx, "input:connection", ev)
	})
}

// SetInputLiveView is the frontend's subscribe-on-view toggle: the active-tab
// reactive calls it true when a tab that renders live channel bars (Input /
// Engine / Gun / Lighting) is on screen, false otherwise.  It records the
// desired state and (re)applies the wire subscription to every hub-local
// input port.  Effects are unaffected — the firmware feeds them locally.
func (a *App) SetInputLiveView(on bool) {
	a.mu.Lock()
	changed := a.liveView != on
	a.liveView = on
	a.mu.Unlock()
	if changed {
		a.diag.Info("INPUT", "live-view → %v (wire broadcast %s)", on,
			map[bool]string{true: "subscribe", false: "unsubscribe"}[on])
	}
	a.applyInputBroadcasts()
}

// applyInputBroadcasts asks the hub to stream (or stop streaming) RC values
// for every hub-local input port carrying an input role — PPM, SBUS, or Jeti
// EX — at the cadence implied by the current live-view state (liveHz()).
// Each protocol has its own broadcast-enable packet, so we dispatch by role
// kind (a Jeti role left on the PPM enable would never stream).  Snapshots the
// client + input ports under lock, then issues the wire commands lock-free.
func (a *App) applyInputBroadcasts() {
	hz := a.liveHz()
	c := a.snapshotClient()
	if c == nil {
		return
	}
	type inputBcast struct {
		idx  byte
		kind byte
	}
	// Hub-local input ports are the canonical empty GUID in the device model
	// (instructions/31), so an empty-GUID filter is exactly right now.
	a.dmMu.Lock()
	var ins []inputBcast
	if a.dm != nil {
		for _, p := range a.dm.Ports {
			if p.Ref.Kind != ports.KindInput {
				continue
			}
			if p.Ref.GUID != "" {
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
			err = c.Input.SetSbusBroadcastHz(in.idx, hz)
		case roles.KindJetiExInput:
			err = c.Input.SetJetiBroadcastHz(in.idx, hz)
		default: // KindRcPwmInput
			err = c.Input.SetBroadcastHz(in.idx, hz)
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

// GetTelemetry snapshots the master's live telemetry collection (item 4) —
// hub-local sensors + actively-polled input devices (e.g. an ESC on IN_2) +
// the publish-rate stats.  Polled by the IO tab's telemetry panel.
func (a *App) GetTelemetry() (input.TelemetrySnapshot, error) {
	c := a.snapshotClient()
	if c == nil {
		return input.TelemetrySnapshot{}, fmt.Errorf("not connected")
	}
	return c.Input.GetTelemetry()
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
	ic := a.inputCfg(guid, kind, index)
	ic.Protocol = def.ID
	if def.ID == devicemodel.InputEscTelem && ic.EscProtocol == "" {
		ic.EscProtocol = "kontronik" // sensible default; sub-select refines
	}
	escProto := ic.EscProtocol
	poles, gear := ic.EscMotorPoles, ic.EscGearRatio
	a.dmMu.Unlock()

	// Attach the realizing role + start broadcasting.  esc-telemetry
	// carries its stream selector + RPM divider in the attach config.
	var cfg []byte
	if def.ID == devicemodel.InputEscTelem {
		if w, ok := devicemodel.EscProtocolWire(escProto); ok {
			cfg = escAttachCfg(w, poles, gear)
		}
	}
	if _, err := a.attachRoleCfg(guid, kind, index, def.RoleKind, cfg); err != nil {
		a.diag.Error("INPUT", "SetInputProtocol: AttachRole failed: %v", err)
		return a.deviceModelSnapshot(), err
	}
	a.applyInputBroadcasts()
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
	a.applyInputBroadcasts()
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

// escAttachCfg builds the esc-telemetry attach config:
// [protocol][baudKHi][baudKLo][ratioHi][ratioLo] — baud 0 = protocol default,
// ratio = RPM divider ×100 (0 = as transmitted), computed as
// (motor poles / 2) × gear ratio (Kontronik transmits ELECTRICAL rpm).
func escAttachCfg(wire byte, motorPoles int, gearRatio float64) []byte {
	polePairs := 1.0
	if motorPoles >= 2 && motorPoles <= 100 {
		polePairs = float64(motorPoles) / 2
	}
	gear := 1.0
	if gearRatio > 0 && gearRatio < 100 {
		gear = gearRatio
	}
	div := polePairs * gear
	x100 := 0
	if div > 0 && div != 1 && div < 655 {
		x100 = int(div*100 + 0.5)
	}
	return []byte{wire, 0, 0, byte(x100 >> 8), byte(x100 & 0xFF)}
}

// SetInputEscProtocol picks the ESC telemetry stream for an input whose
// protocol is esc-telemetry, re-attaching the role with the new selector.
func (a *App) SetInputEscProtocol(guid string, kind, index byte, escProto string) (DeviceModelSnapshot, error) {
	defer a.diag.Around("SetInputEscProtocol",
		map[string]any{"guid": guid, "idx": index, "esc": escProto})()
	w, ok := devicemodel.EscProtocolWire(escProto)
	if !ok {
		return a.deviceModelSnapshot(), fmt.Errorf("unknown ESC protocol %q", escProto)
	}
	a.dmMu.Lock()
	ic := a.inputCfg(guid, kind, index)
	ic.Protocol = devicemodel.InputEscTelem
	ic.EscProtocol = escProto
	poles, gear := ic.EscMotorPoles, ic.EscGearRatio
	a.dmMu.Unlock()
	if _, err := a.attachRoleCfg(guid, kind, index, roles.KindEscTelemetry, escAttachCfg(w, poles, gear)); err != nil {
		return a.deviceModelSnapshot(), err
	}
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot(), nil
}

// SetInputEscRpmScaling sets the RPM scaling for an esc-telemetry input —
// motor pole count (electrical rpm = shaft rpm × poles/2) and gearbox ratio —
// and re-attaches the role so the firmware divider updates immediately.
func (a *App) SetInputEscRpmScaling(guid string, kind, index byte, motorPoles int, gearRatio float64) (DeviceModelSnapshot, error) {
	defer a.diag.Around("SetInputEscRpmScaling",
		map[string]any{"guid": guid, "idx": index, "poles": motorPoles, "gear": gearRatio})()
	if motorPoles != 0 && (motorPoles < 2 || motorPoles > 100 || motorPoles%2 != 0) {
		return a.deviceModelSnapshot(), fmt.Errorf("motor poles %d invalid (even, 2–100)", motorPoles)
	}
	if gearRatio < 0 || gearRatio >= 100 {
		return a.deviceModelSnapshot(), fmt.Errorf("gear ratio %.2f out of range (0–100)", gearRatio)
	}
	a.dmMu.Lock()
	ic := a.inputCfg(guid, kind, index)
	ic.Protocol = devicemodel.InputEscTelem
	ic.EscMotorPoles = motorPoles
	ic.EscGearRatio = gearRatio
	escProto := ic.EscProtocol
	if escProto == "" {
		escProto = "kontronik"
		ic.EscProtocol = escProto
	}
	a.dmMu.Unlock()
	w, ok := devicemodel.EscProtocolWire(escProto)
	if !ok {
		return a.deviceModelSnapshot(), fmt.Errorf("unknown ESC protocol %q", escProto)
	}
	if _, err := a.attachRoleCfg(guid, kind, index, roles.KindEscTelemetry, escAttachCfg(w, motorPoles, gearRatio)); err != nil {
		return a.deviceModelSnapshot(), err
	}
	a.emitDeviceModelChanged()
	return a.deviceModelSnapshot(), nil
}
