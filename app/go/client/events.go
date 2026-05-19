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
	"scalefx/protocol/storage"
	"scalefx/protocol/topology"
)

// Events is the async-packet subscription facet.  Callers register
// typed handlers; the client routes incoming TAG_ASYNC packets to the
// appropriate handler.  Handlers run on the connection's reader
// goroutine — keep them quick (push to a channel, return).
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
	onPacket       []func(*protocol.Response) // catch-all
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

// ─── Subscribers ─────────────────────────────────────────────────────

func (e *Events) OnLog(fn func(LogMessage))                              { e.add(&e.onLog, fn) }
func (e *Events) OnExpanderIdentified(fn func(expp.ExpanderEntry))       { e.add(&e.onIdentified, fn) }
func (e *Events) OnExpanderConnected(fn func(expp.ConnectedEvent))       { e.add(&e.onConnected, fn) }
func (e *Events) OnExpanderDisconnected(fn func(expp.DisconnectedEvent)) { e.add(&e.onDisconnected, fn) }
func (e *Events) OnExpanderCollision(fn func(expp.CollisionEvent))       { e.add(&e.onCollision, fn) }
func (e *Events) OnRoleEvent(fn func(topology.RoleEvent))                { e.add(&e.onRoleEvent, fn) }
func (e *Events) OnUploadProgress(fn func(storage.UploadProgress))       { e.add(&e.onUploadProg, fn) }
func (e *Events) OnLandingLightPhase(fn func(landing.PhaseChange))       { e.add(&e.onLLPhase, fn) }
func (e *Events) OnGearPhase(fn func(gear.PhaseChange))                  { e.add(&e.onGearPhase, fn) }
func (e *Events) OnEngineState(fn func(enginefx.StateChange))            { e.add(&e.onEngineState, fn) }
func (e *Events) OnGunShot(fn func(gunfx.Shot))                          { e.add(&e.onGunShot, fn) }
func (e *Events) OnAny(fn func(*protocol.Response))                      { e.add(&e.onPacket, fn) }

// ─── Dispatch ────────────────────────────────────────────────────────

func (e *Events) onAsync(resp *protocol.Response) {
	if resp == nil {
		return
	}
	// Always fan out to "any" subscribers first.
	for _, fn := range e.snapshotAny() {
		fn(resp)
	}

	switch resp.PacketType {
	case core.LogMessage:
		if msg := decodeLog(resp.Payload); msg != nil {
			for _, fn := range e.snapshotLog() {
				fn(*msg)
			}
		}
	case expp.ExpanderIdentified:
		if ev, err := expp.DecodeIdentifiedEvent(resp.Payload); err == nil {
			for _, fn := range e.snapshotIdent() {
				fn(ev)
			}
		}
	case expp.ExpanderConnected:
		if ev, err := expp.DecodeConnectedEvent(resp.Payload); err == nil {
			for _, fn := range e.snapshotConnected() {
				fn(ev)
			}
		}
	case expp.ExpanderDisconnected:
		if ev, err := expp.DecodeDisconnectedEvent(resp.Payload); err == nil {
			for _, fn := range e.snapshotDisconnected() {
				fn(ev)
			}
		}
	case expp.ExpanderCollision:
		if ev, err := expp.DecodeCollisionEvent(resp.Payload); err == nil {
			for _, fn := range e.snapshotCollision() {
				fn(ev)
			}
		}
	case topology.TopologyRoleEvent:
		if ev, err := topology.DecodeRoleEvent(resp.Payload); err == nil {
			for _, fn := range e.snapshotRole() {
				fn(ev)
			}
		}
	case storage.FileUploadProgress:
		if ev, err := storage.DecodeUploadProgress(resp.Payload); err == nil {
			for _, fn := range e.snapshotUpload() {
				fn(ev)
			}
		}
	case landing.PhaseEvent:
		if ev, err := landing.DecodePhaseEvent(resp.Payload); err == nil {
			for _, fn := range e.snapshotLLPhase() {
				fn(ev)
			}
		}
	case gear.PhaseEvent:
		if ev, err := gear.DecodePhaseEvent(resp.Payload); err == nil {
			for _, fn := range e.snapshotGearPhase() {
				fn(ev)
			}
		}
	case enginefx.StateEvent:
		if ev, err := enginefx.DecodeStateEvent(resp.Payload); err == nil {
			for _, fn := range e.snapshotEngineState() {
				fn(ev)
			}
		}
	case gunfx.ShotEvent:
		if ev, err := gunfx.DecodeShotEvent(resp.Payload); err == nil {
			for _, fn := range e.snapshotGunShot() {
				fn(ev)
			}
		}
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

// ─── Snapshot helpers (read under lock, fire without holding it) ──────

func (e *Events) snapshotLog() []func(LogMessage) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(LogMessage){}, e.onLog...)
}
func (e *Events) snapshotIdent() []func(expp.ExpanderEntry) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(expp.ExpanderEntry){}, e.onIdentified...)
}
func (e *Events) snapshotConnected() []func(expp.ConnectedEvent) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(expp.ConnectedEvent){}, e.onConnected...)
}
func (e *Events) snapshotDisconnected() []func(expp.DisconnectedEvent) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(expp.DisconnectedEvent){}, e.onDisconnected...)
}
func (e *Events) snapshotCollision() []func(expp.CollisionEvent) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(expp.CollisionEvent){}, e.onCollision...)
}
func (e *Events) snapshotRole() []func(topology.RoleEvent) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(topology.RoleEvent){}, e.onRoleEvent...)
}
func (e *Events) snapshotUpload() []func(storage.UploadProgress) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(storage.UploadProgress){}, e.onUploadProg...)
}
func (e *Events) snapshotLLPhase() []func(landing.PhaseChange) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(landing.PhaseChange){}, e.onLLPhase...)
}
func (e *Events) snapshotGearPhase() []func(gear.PhaseChange) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(gear.PhaseChange){}, e.onGearPhase...)
}
func (e *Events) snapshotEngineState() []func(enginefx.StateChange) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(enginefx.StateChange){}, e.onEngineState...)
}
func (e *Events) snapshotGunShot() []func(gunfx.Shot) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(gunfx.Shot){}, e.onGunShot...)
}
func (e *Events) snapshotAny() []func(*protocol.Response) {
	e.mu.RLock()
	defer e.mu.RUnlock()
	return append([]func(*protocol.Response){}, e.onPacket...)
}

// add appends `fn` to the slice pointed to by `dst` under the write lock.
func (e *Events) add(dst any, fn any) {
	e.mu.Lock()
	defer e.mu.Unlock()
	switch d := dst.(type) {
	case *[]func(LogMessage):
		*d = append(*d, fn.(func(LogMessage)))
	case *[]func(expp.ExpanderEntry):
		*d = append(*d, fn.(func(expp.ExpanderEntry)))
	case *[]func(expp.ConnectedEvent):
		*d = append(*d, fn.(func(expp.ConnectedEvent)))
	case *[]func(expp.DisconnectedEvent):
		*d = append(*d, fn.(func(expp.DisconnectedEvent)))
	case *[]func(expp.CollisionEvent):
		*d = append(*d, fn.(func(expp.CollisionEvent)))
	case *[]func(topology.RoleEvent):
		*d = append(*d, fn.(func(topology.RoleEvent)))
	case *[]func(storage.UploadProgress):
		*d = append(*d, fn.(func(storage.UploadProgress)))
	case *[]func(landing.PhaseChange):
		*d = append(*d, fn.(func(landing.PhaseChange)))
	case *[]func(gear.PhaseChange):
		*d = append(*d, fn.(func(gear.PhaseChange)))
	case *[]func(enginefx.StateChange):
		*d = append(*d, fn.(func(enginefx.StateChange)))
	case *[]func(gunfx.Shot):
		*d = append(*d, fn.(func(gunfx.Shot)))
	case *[]func(*protocol.Response):
		*d = append(*d, fn.(func(*protocol.Response)))
	}
}
