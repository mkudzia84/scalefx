package client

import (
	"sync"

	"scalefx/protocol"
	"scalefx/protocol/core"
	"scalefx/protocol/enginefx"
	expp "scalefx/protocol/expanders"
	"scalefx/protocol/gear"
	"scalefx/protocol/gunfx"
	"scalefx/protocol/landing"
	"scalefx/protocol/roles"
	"scalefx/protocol/storage"
	"scalefx/protocol/topology"
)

// Events is the async-packet subscription facet.  Callers register typed
// handlers; the client routes incoming TAG_ASYNC packets to them.  Handlers run
// on the connection's reader goroutine — keep them quick (push to a channel,
// return).
//
// Subscription is GENERIC (Rule 57): OnXxx call the Go-generic `subscribe[T]`,
// and per-role telemetry flows through the `OnRole` inner-type registry — so a
// new typed event or a new role's events can NEVER silently no-op.  (The old
// hand-maintained type-switch `add()` WAS that trap — it bit InputValue then
// ServoMotion before this was generic.)
type Events struct {
	c *Client

	mu sync.RWMutex

	onLog          []func(LogMessage)
	onIdentified   []func(expp.ExpanderEntry)
	onConnected    []func(expp.ConnectedEvent)
	onDisconnected []func(expp.DisconnectedEvent)
	onCollision    []func(expp.CollisionEvent)
	onRoleEvent    []func(topology.RoleEvent)
	onUploadProg   []func(storage.UploadProgress)
	onLLPhase      []func(landing.PhaseChange)
	onGearPhase    []func(gear.PhaseChange)
	onEngineState  []func(enginefx.StateChange)
	onGunShot      []func(gunfx.Shot)
	onGunVerbose   []func(gunfx.VerboseStatus)
	onInputValue   []func(InputValue)
	onServoMotion  []func(ServoMotionEvent)
	onPacket       []func(*protocol.Response) // catch-all

	// Generic per-inner-type role-event registry (Rule 57): handlers keyed by
	// the role async packet byte.  EVERY role event — hub-local OR expander,
	// any present/future role — is dispatched here with its source GUID, so a
	// new role's telemetry is consumable with ZERO events.go changes.
	onRoleByType map[byte][]func(guid string, payload []byte)
}

// LogMessage is a decoded LOG_MESSAGE async packet.
type LogMessage struct {
	Level   byte
	Millis  uint32
	Message string
}

func newEvents(c *Client) *Events {
	e := &Events{c: c}
	c.conn.SetCallback(e.onAsync)
	return e
}

func (e *Events) shutdown() {
	if e == nil || e.c == nil {
		return
	}
	e.c.conn.SetCallback(nil)
}

// ─── Generic subscribe / snapshot (the trap-proof core) ───────────────

// subscribe appends fn to dst under the write lock.  Generic, so a new typed
// event can never be dropped by a missing type-switch case (the old `add()`
// trap).  Every OnXxx below is one line through here.
func subscribe[T any](e *Events, dst *[]func(T), fn func(T)) {
	e.mu.Lock()
	defer e.mu.Unlock()
	*dst = append(*dst, fn)
}

// snapshot copies a handler slice under the read lock so it can be fired
// without holding the mutex (handlers may re-enter the Events API).
func snapshot[T any](e *Events, src []func(T)) []func(T) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(T){}, src...)
}

// ─── Subscribers ─────────────────────────────────────────────────────

func (e *Events) OnLog(fn func(LogMessage))                              { subscribe(e, &e.onLog, fn) }
func (e *Events) OnExpanderIdentified(fn func(expp.ExpanderEntry))       { subscribe(e, &e.onIdentified, fn) }
func (e *Events) OnExpanderConnected(fn func(expp.ConnectedEvent))       { subscribe(e, &e.onConnected, fn) }
func (e *Events) OnExpanderDisconnected(fn func(expp.DisconnectedEvent)) { subscribe(e, &e.onDisconnected, fn) }
func (e *Events) OnExpanderCollision(fn func(expp.CollisionEvent))       { subscribe(e, &e.onCollision, fn) }
func (e *Events) OnRoleEvent(fn func(topology.RoleEvent))                { subscribe(e, &e.onRoleEvent, fn) }
func (e *Events) OnUploadProgress(fn func(storage.UploadProgress))       { subscribe(e, &e.onUploadProg, fn) }
func (e *Events) OnLandingLightPhase(fn func(landing.PhaseChange))       { subscribe(e, &e.onLLPhase, fn) }
func (e *Events) OnGearPhase(fn func(gear.PhaseChange))                  { subscribe(e, &e.onGearPhase, fn) }
func (e *Events) OnEngineState(fn func(enginefx.StateChange))            { subscribe(e, &e.onEngineState, fn) }
func (e *Events) OnGunShot(fn func(gunfx.Shot))                          { subscribe(e, &e.onGunShot, fn) }
func (e *Events) OnGunVerboseStatus(fn func(gunfx.VerboseStatus))        { subscribe(e, &e.onGunVerbose, fn) }
func (e *Events) OnInputValue(fn func(InputValue))                       { subscribe(e, &e.onInputValue, fn) }
func (e *Events) OnServoMotion(fn func(ServoMotionEvent))                { subscribe(e, &e.onServoMotion, fn) }
func (e *Events) OnAny(fn func(*protocol.Response))                      { subscribe(e, &e.onPacket, fn) }

// OnRole registers a handler for ONE role async packet type (Rule 57) — any
// role, hub-local (guid "") or expander.  The handler gets the source GUID +
// raw payload; decode it with the matching roles.DecodeXxx.  This is the
// generic, future-proof telemetry path: a new role's events (a stepper's
// done-event, a PID's status, …) flow with no change here.  Pass innerType 0
// to receive EVERY role event.
func (e *Events) OnRole(innerType byte, fn func(guid string, payload []byte)) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.onRoleByType == nil {
		e.onRoleByType = map[byte][]func(string, []byte){}
	}
	e.onRoleByType[innerType] = append(e.onRoleByType[innerType], fn)
}

// ─── Dispatch ────────────────────────────────────────────────────────

// isRoleAsync reports whether a packet byte is in the role-layer range
// (0x40..0x7F).  Async packets in that range are role EVENTS — responses are
// tag-correlated and never reach onAsync — so any role-range async is fanned
// out generically (dispatchRole).
func isRoleAsync(t protocol.PacketType) bool { return t >= 0x40 && t <= 0x7F }

// isHighRateRole reports the continuous-telemetry STREAM packets (RC frames,
// servo-motion).  These have dedicated typed subscriptions (OnInputValue /
// OnServoMotion) and fire at 20–50 Hz, so they're kept OUT of the generic
// OnRoleEvent catch-all (which carries DISCRETE events — stall, endstop,
// reached, done, attached/detached — and must stay readable).  A consumer that
// wants a stream uses its typed subscription, not the catch-all.
func isHighRateRole(t byte) bool {
	switch protocol.PacketType(t) {
	case roles.RcinValueBroadcast, roles.PpmFrameBroadcast, roles.SbusFrameBroadcast,
		roles.JetiExFrameBroadcast, roles.ServoMotionUpdate:
		return true
	}
	return false
}

// dispatchRole fans a role event (hub-local or expander, guid-tagged) out to
// OnRoleEvent + the per-inner-type OnRole registry.  The ONE place every role
// event flows through.
func (e *Events) dispatchRole(guid string, innerType byte, payload []byte) {
	ev := topology.RoleEvent{GUID: guid, InnerType: protocol.PacketType(innerType), InnerPayload: payload}
	for _, fn := range snapshot(e, e.onRoleEvent) {
		fn(ev)
	}
	e.mu.RLock()
	hs := append([]func(string, []byte){}, e.onRoleByType[innerType]...)
	all := append([]func(string, []byte){}, e.onRoleByType[0]...) // innerType 0 = "every role event"
	e.mu.RUnlock()
	for _, fn := range hs {
		fn(guid, payload)
	}
	for _, fn := range all {
		fn(guid, payload)
	}
}

func (e *Events) onAsync(resp *protocol.Response) {
	if resp == nil {
		return
	}
	// Always fan out to "any" subscribers first.
	for _, fn := range snapshot(e, e.onPacket) {
		fn(resp)
	}

	switch resp.PacketType {
	case core.LogMessage:
		if msg := decodeLog(resp.Payload); msg != nil {
			for _, fn := range snapshot(e, e.onLog) {
				fn(*msg)
			}
		}
	case expp.ExpanderIdentified:
		if ev, err := expp.DecodeIdentifiedEvent(resp.Payload); err == nil {
			for _, fn := range snapshot(e, e.onIdentified) {
				fn(ev)
			}
		}
	case expp.ExpanderConnected:
		if ev, err := expp.DecodeConnectedEvent(resp.Payload); err == nil {
			for _, fn := range snapshot(e, e.onConnected) {
				fn(ev)
			}
		}
	case expp.ExpanderDisconnected:
		if ev, err := expp.DecodeDisconnectedEvent(resp.Payload); err == nil {
			for _, fn := range snapshot(e, e.onDisconnected) {
				fn(ev)
			}
		}
	case expp.ExpanderCollision:
		if ev, err := expp.DecodeCollisionEvent(resp.Payload); err == nil {
			for _, fn := range snapshot(e, e.onCollision) {
				fn(ev)
			}
		}
	case topology.TopologyRoleEvent:
		// Expander role event — unwrap [guid][innerType][payload] and feed the
		// SAME generic role-event path as hub-local (Rule 57), plus the typed
		// servo-telemetry view.  (Input is hub-only, so no expander unwrap.)
		if ev, err := topology.DecodeRoleEvent(resp.Payload); err == nil {
			// Servo telemetry → typed stream view; discrete events → generic.
			if sm, ok := decodeServoMotion(ev.GUID, byte(ev.InnerType), ev.InnerPayload); ok {
				for _, fn := range snapshot(e, e.onServoMotion) {
					fn(sm)
				}
			}
			if !isHighRateRole(byte(ev.InnerType)) {
				e.dispatchRole(ev.GUID, byte(ev.InnerType), ev.InnerPayload)
			}
		}
	case roles.RcinValueBroadcast, roles.PpmFrameBroadcast, roles.SbusFrameBroadcast, roles.JetiExFrameBroadcast:
		// Hub-local input frames arrive directly (guid "", HubFX-only).
		if iv, ok := decodeInputValue("", byte(resp.PacketType), resp.Payload); ok {
			for _, fn := range snapshot(e, e.onInputValue) {
				fn(iv)
			}
		}
	case roles.ServoMotionUpdate:
		// Hub-local servo telemetry arrives directly (guid "").
		if sm, ok := decodeServoMotion("", byte(resp.PacketType), resp.Payload); ok {
			for _, fn := range snapshot(e, e.onServoMotion) {
				fn(sm)
			}
		}
	case storage.FileUploadProgress:
		if ev, err := storage.DecodeUploadProgress(resp.Payload); err == nil {
			for _, fn := range snapshot(e, e.onUploadProg) {
				fn(ev)
			}
		}
	case landing.PhaseEvent:
		if ev, err := landing.DecodePhaseEvent(resp.Payload); err == nil {
			for _, fn := range snapshot(e, e.onLLPhase) {
				fn(ev)
			}
		}
	case gear.PhaseEvent:
		if ev, err := gear.DecodePhaseEvent(resp.Payload); err == nil {
			for _, fn := range snapshot(e, e.onGearPhase) {
				fn(ev)
			}
		}
	case enginefx.StateEvent:
		if ev, err := enginefx.DecodeStateEvent(resp.Payload); err == nil {
			for _, fn := range snapshot(e, e.onEngineState) {
				fn(ev)
			}
		}
	case gunfx.ShotEvent:
		if ev, err := gunfx.DecodeShotEvent(resp.Payload); err == nil {
			for _, fn := range snapshot(e, e.onGunShot) {
				fn(ev)
			}
		}
	case gunfx.VerboseStatusPkt:
		if ev, err := gunfx.DecodeVerboseStatus(resp.Payload); err == nil {
			for _, fn := range snapshot(e, e.onGunVerbose) {
				fn(ev)
			}
		}
	}

	// Generic hub-local role-event fan-out (Rule 57): any DISCRETE role-range
	// async (not a high-rate stream — those use the typed views above) is a role
	// event → OnRoleEvent + the OnRole registry with guid "".  So every role's
	// discrete telemetry is reachable without a per-type case here.  Expander
	// role events come in wrapped (handled above).
	if isRoleAsync(resp.PacketType) && !isHighRateRole(byte(resp.PacketType)) {
		e.dispatchRole("", byte(resp.PacketType), resp.Payload)
	}
}

func decodeLog(p []byte) *LogMessage {
	if len(p) < 5 {
		return nil
	}
	return &LogMessage{
		Level:   p[0],
		Millis:  uint32(p[1]) | uint32(p[2])<<8 | uint32(p[3])<<16 | uint32(p[4])<<24,
		Message: string(p[5:]),
	}
}
