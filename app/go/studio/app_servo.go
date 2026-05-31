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
func (a *App) installServoStream() {
	if a.c == nil {
		return
	}
	a.servoDiagSeen = 0
	a.c.Events.OnServoMotion(func(ev client.ServoMotionEvent) {
		if a.ctx == nil {
			return
		}
		// Hub-local frames decode with GUID "" — remap to the hub's GUID so the
		// frontend keys servo status by the same port ref it uses everywhere.
		if ev.GUID == "" {
			a.mu.Lock()
			ev.GUID = a.id.GUID
			a.mu.Unlock()
		}
		// One-shot confirmation that the stream is flowing + what it carries
		// (guid/idx must match the gun panel's servoPort key).  Throttled to the
		// first frame per (re)connect so it doesn't flood at 20 Hz.
		if a.servoDiagSeen < 1 {
			a.servoDiagSeen++
			if len(ev.Servos) > 0 {
				s := ev.Servos[0]
				a.diag.Info("SERVO", "stream live: guid=%q servos=%d first{idx=%d pos=%d tgt=%d}",
					ev.GUID, len(ev.Servos), s.PortIdx, s.PosUs, s.TargetUs)
			} else {
				a.diag.Info("SERVO", "stream live: guid=%q servos=0 (no ServoActuator roles attached)", ev.GUID)
			}
		}
		wailsRT.EventsEmit(a.ctx, "servo:motion", ev)
	})

	// Re-apply the live-view subscription on (re)connect.  The firmware resets
	// its broadcast Hz to 0 on reboot/reconnect, and SetServoLiveView's
	// changed-guard won't re-send (a.servoLiveView is still true from before).
	// Without this, reconnecting — or having the panel already open before the
	// connection came up — leaves the stream silent.
	a.mu.Lock()
	want := a.servoLiveView
	a.mu.Unlock()
	if want {
		if c := a.snapshotClient(); c != nil {
			_ = c.Roles.ServoSetBroadcastHz(byte(kServoBroadcastHz))
			a.diag.Info("SERVO", "re-subscribed live-view on connect (%d Hz)", kServoBroadcastHz)
		}
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
