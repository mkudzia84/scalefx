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
		go func() {
			_ = c.Roles.ServoSetBroadcastHz(byte(kServoBroadcastHz))
		}()
		a.diag.Info("SERVO", "re-subscribing live-view on connect (%d Hz)", kServoBroadcastHz)
	}
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
	_ = c.Roles.ServoSetBroadcastHz(byte(hz))
	a.diag.Info("SERVO", "live-view → %v (broadcast %d Hz)", on, hz)
}
