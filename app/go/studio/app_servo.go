package main

import (
	"scalefx/client"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// kServoBroadcastHz — cadence for the generic servo telemetry stream while a
// panel showing servo status is on screen.  Servos are slow; 20 Hz is smooth
// without flooding the lossy async queue (Rule 53).
const kServoBroadcastHz uint8 = 20

// installServoStream subscribes to the batched SERVO_MOTION_UPDATE telemetry and
// forwards it to the frontend as the `servo:motion` event.  Generic / port-keyed
// (Rule 42): any UI domain that uses a servo consumes the same stream.  Called
// once per connect (fresh client → fresh subscriber list).
//
// IMPORTANT: the caller (openLocked) ALREADY HOLDS a.mu — this runs inside the
// connect critical section.  Do NOT call a.mu.Lock() or any helper that does
// (snapshotClient) here, and do NOT block on a wire round-trip: a.mu is
// non-reentrant, so re-locking self-deadlocks the whole connect.  Read a.c /
// a.servoLiveView directly and fire any wire command off the lock (a goroutine).
func (a *App) installServoStream() {
	if a.c == nil {
		return
	}
	a.c.Events.OnServoMotion(func(ev client.ServoMotionEvent) {
		if a.ctx == nil {
			return
		}
		// Hub-local frames decode with GUID "" — which is now the canonical
		// hub-local form the device model + frontend key by (instructions/31),
		// so the old remap to the hub's own GUID is gone: "" already matches.
		wailsRT.EventsEmit(a.ctx, "servo:motion", ev)
	})

	// Re-apply the live-view subscription on (re)connect.  The firmware resets
	// its broadcast Hz to 0 on reboot/reconnect, and SetServoLiveView's
	// changed-guard won't re-send (a.servoLiveView is still true from before).
	// Without this, reconnecting — or having the panel already open before the
	// connection came up — leaves the stream silent.  Caller holds a.mu, so read
	// the flag + client directly and send off-lock in a goroutine (Connection is
	// thread-safe, Rule 56) so we never block the connect path on an ACK.
	if a.servoLiveView {
		c := a.c
		go a.applyServoBroadcast(c, byte(kServoBroadcastHz))
		a.diag.Info("SERVO", "re-subscribing live-view on connect (%d Hz)", kServoBroadcastHz)
	}
}

// applyServoBroadcast enables/disables the servo telemetry stream on the hub AND
// every connected expander (Rule 58 — transparent: an expander servo's live
// position must reach Studio just like a hub servo's).  Forwarded per-GUID via
// RoleTarget; the expander then emits SERVO_MOTION_UPDATE async → hub re-emit →
// OnServoMotion (guid-tagged) → servo:motion event.  Runs off-lock.
func (a *App) applyServoBroadcast(c *client.Client, hz byte) {
	if c == nil {
		return
	}
	_ = c.Role("").ServoSetBroadcastHz(hz) // hub
	for _, guid := range a.connectedExpanderGUIDs() {
		_ = c.Role(guid).ServoSetBroadcastHz(hz)
	}
}

// connectedExpanderGUIDs returns the distinct non-hub board GUIDs in the live
// device model (the connected expanders).
func (a *App) connectedExpanderGUIDs() []string {
	a.dmMu.Lock()
	defer a.dmMu.Unlock()
	if a.dm == nil {
		return nil
	}
	seen := map[string]bool{}
	var out []string
	for _, p := range a.dm.Ports {
		if p.Ref.GUID != "" && !seen[p.Ref.GUID] {
			seen[p.Ref.GUID] = true
			out = append(out, p.Ref.GUID)
		}
	}
	return out
}

// SetServoLiveView is the frontend's subscribe-on-view toggle for live servo
// position (gun turret output bars, etc.).  Enables/disables the GLOBAL
// SERVO_SET_BROADCAST_HZ stream for the hub's own servos.
//
// Upload-safe (matches the input live-view + Rules 53/54/56): the firmware gates
// the emit on hostVerboseActive AND the upload-exclusive loop skips
// RoleService::update() entirely while a transfer is active, so the stream goes
// silent during an upload with no client-side poller to gate.
func (a *App) SetServoLiveView(on bool) {
	a.mu.Lock()
	changed := a.servoLiveView != on
	a.servoLiveView = on
	a.mu.Unlock()
	if !changed {
		return
	}
	c := a.snapshotClient()
	if c == nil {
		return
	}
	hz := uint8(0)
	if on {
		hz = kServoBroadcastHz
	}
	a.applyServoBroadcast(c, byte(hz)) // hub + every connected expander (Rule 58)
	a.diag.Info("SERVO", "live-view → %v (broadcast %d Hz)", on, hz)
}
