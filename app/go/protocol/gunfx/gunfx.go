// Package gunfx mirrors
// controllers/hubfx/esp32s3/src/effects/gunfx/gunfx_protocol.h —
// the master-side GunFX wire surface.  Each gun unit is muzzle-flash
// LED + optional recoil servo + optional smoke heater + fan + optional
// yaw/pitch turret + optional fire trigger + optional ROF selector.
//
// Packet slices:
//
//	0xCC..0xD2  primary  (fire / auto-fire / smoke / status / shot-event)
//	0xE2..0xE5  manual override + verbose status  (Phase 1 of the
//	            GunFX rollout — instructions/22)
package gunfx

import (
	"encoding/binary"
	"fmt"

	"scalefx/protocol"
)

// ─── Packet types ────────────────────────────────────────────────────

const (
	FireOnce             protocol.PacketType = 0xCC
	StartFiring          protocol.PacketType = 0xCD
	StopFiring           protocol.PacketType = 0xCE
	SmokeArm             protocol.PacketType = 0xCF
	StatusReq            protocol.PacketType = 0xD0
	StatusResp           protocol.PacketType = 0xD1
	ShotEvent            protocol.PacketType = 0xD2
	ManualSet            protocol.PacketType = 0xE2
	ManualRelease        protocol.PacketType = 0xE3
	VerboseStatusReq     protocol.PacketType = 0xE4
	VerboseStatusPkt     protocol.PacketType = 0xE5 // decoded struct is `VerboseStatus`
)

// ─── Manual-override flag bits (GUN_MANUAL_SET.flags) ────────────────

const (
	ManualFlagYaw      byte = 1 << 0 // 0x01
	ManualFlagPitch    byte = 1 << 1 // 0x02
	ManualFlagRof      byte = 1 << 2 // 0x04
	ManualFlagFire     byte = 1 << 3 // 0x08
	ManualFlagSmoke    byte = 1 << 4 // 0x10
	ManualFlagFanBurst byte = 1 << 5 // 0x20
)

// ─── Mode (GUN_VERBOSE_STATUS.mode) ──────────────────────────────────

const (
	ModeRc     byte = 0
	ModeManual byte = 1
)

// Sentinel values used in the verbose-status payload.
const (
	RofIndexNone        byte  = 0xFF
	HeaterTempNoSensor  int16 = 0x7FFF
)

// ─── Error codes ─────────────────────────────────────────────────────

// GunFX errors live at 0x30..0x3F (CLAUDE.md spec).  Old values
// 0xCB-0xCD squatted in the GunFX packet-type range — moved as part
// of the 2026-05-23 comprehensive error-code cleanup.
const (
	ErrUnknownID      protocol.ErrorCode = 0x30
	ErrTableFull      protocol.ErrorCode = 0x31
	ErrNotImplemented protocol.ErrorCode = 0x32 // Phase 1 stub
	ErrRofOutOfRange  protocol.ErrorCode = 0x33 // 2026-05-24: explicit rofIndex on FIRE_ONCE/START_FIRING was >= numRofItems
)

// ─── Decoded types ───────────────────────────────────────────────────

// GunStatus is one entry in GUN_STATUS_RESP.
type GunStatus struct {
	ID         byte `json:"id"`
	Firing     bool `json:"firing"`
	SmokeArmed bool `json:"smokeArmed"`
}

// Shot is the decoded GUN_SHOT_EVENT async payload — one packet per
// fired round.
type Shot struct {
	ID byte `json:"id"`
}

// ManualState carries the fields that GUN_MANUAL_SET writes to one
// gun. `Flags` MUST be the OR of `ManualFlag*` bits the caller wants
// honoured; fields not selected by Flags are ignored by the firmware
// (they keep their prior manual value, or the RC-driven value if the
// field has never been manually set).
type ManualState struct {
	Flags          byte   `json:"flags"`
	YawUs          uint16 `json:"yawUs"`
	PitchUs        uint16 `json:"pitchUs"`
	RofIndex       byte   `json:"rofIndex"`
	FireHold       byte   `json:"fireHold"`
	SmokeArm       byte   `json:"smokeArm"`
	SmokeFanBurst  byte   `json:"smokeFanBurst"`
}

// VerboseStatus is the decoded GUN_VERBOSE_STATUS async payload — every
// field a Studio "simulate" panel needs to mirror what the firmware is
// doing for one gun.
type VerboseStatus struct {
	ID                byte   `json:"id"`
	Mode              byte   `json:"mode"`              // ModeRc / ModeManual
	Firing            bool   `json:"firing"`
	SmokeArmed        bool   `json:"smokeArmed"`
	SmokeFanRunning   bool   `json:"smokeFanRunning"`
	// HeaterDutyPct: 0 when the gun has the heater off, 100 when the
	// gun is commanding the heater on.  This is the GUN'S INTENT —
	// the role layer applies element-vs-rail scaleDuty() to translate
	// it into the actual port-native duty.
	HeaterDutyPct     byte   `json:"heaterDutyPct"`
	HeaterTempCx10    int16  `json:"heaterTempCx10"`    // HeaterTempNoSensor = no sensor
	YawCurrentUs      uint16 `json:"yawCurrentUs"`
	YawTargetUs       uint16 `json:"yawTargetUs"`
	PitchCurrentUs    uint16 `json:"pitchCurrentUs"`
	PitchTargetUs     uint16 `json:"pitchTargetUs"`
	RofIndex          byte   `json:"rofIndex"`          // RofIndexNone = no item armed
	RofSelectorUs     uint16 `json:"rofSelectorUs"`     // last raw value on the ROF channel
	TriggerUs         uint16 `json:"triggerUs"`         // last raw value on the trigger channel
	ShotsThisSession  uint32 `json:"shotsThisSession"`
	// HeaterActive — Rule 43 activation gate state (Phase 4 polish
	// 2026-05-26).  True when the activation channel reads above its
	// threshold, OR when no activation channel is bound at all (the
	// gate defaults to "always allowed").  Studio uses this to render
	// the "gated off" pill when SmokeArmed is true but HeaterActive
	// is false (i.e. the operator armed smoke but the RC gate is low).
	HeaterActive      bool   `json:"heaterActive"`
}

// verboseStatusSize is the fixed wire size of a VerboseStatus payload
// in v1 (no Rule-11 append-only; greenfield).
const verboseStatusSize = 26

// ─── Decoders ────────────────────────────────────────────────────────

// DecodeStatus parses GUN_STATUS_RESP:
//
//	[count:u8] per-entry: [id:u8][firing:u8][smokeArmed:u8]
func DecodeStatus(p []byte) ([]GunStatus, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("gun status: empty")
	}
	count := int(p[0])
	if 1+3*count > len(p) {
		return nil, fmt.Errorf("gun status: truncated (need %d)", count)
	}
	out := make([]GunStatus, count)
	for i := 0; i < count; i++ {
		off := 1 + 3*i
		out[i] = GunStatus{
			ID:         p[off],
			Firing:     p[off+1] != 0,
			SmokeArmed: p[off+2] != 0,
		}
	}
	return out, nil
}

// DecodeShotEvent parses a GUN_SHOT_EVENT async payload.
func DecodeShotEvent(p []byte) (Shot, error) {
	if len(p) < 1 {
		return Shot{}, fmt.Errorf("gun shot event: empty")
	}
	return Shot{ID: p[0]}, nil
}

// DecodeVerboseStatus parses a GUN_VERBOSE_STATUS async payload.
// Fixed 26-byte layout — see gunfx_protocol.h for the field map.
func DecodeVerboseStatus(p []byte) (VerboseStatus, error) {
	if len(p) != verboseStatusSize {
		return VerboseStatus{}, fmt.Errorf(
			"gun verbose status: expected %d bytes, got %d",
			verboseStatusSize, len(p))
	}
	return VerboseStatus{
		ID:               p[0],
		Mode:             p[1],
		Firing:           p[2] != 0,
		SmokeArmed:       p[3] != 0,
		SmokeFanRunning:  p[4] != 0,
		HeaterDutyPct:    p[5],
		HeaterTempCx10:   int16(binary.LittleEndian.Uint16(p[6:8])),
		YawCurrentUs:     binary.LittleEndian.Uint16(p[8:10]),
		YawTargetUs:      binary.LittleEndian.Uint16(p[10:12]),
		PitchCurrentUs:   binary.LittleEndian.Uint16(p[12:14]),
		PitchTargetUs:    binary.LittleEndian.Uint16(p[14:16]),
		RofIndex:         p[16],
		RofSelectorUs:    binary.LittleEndian.Uint16(p[17:19]),
		TriggerUs:        binary.LittleEndian.Uint16(p[19:21]),
		ShotsThisSession: binary.LittleEndian.Uint32(p[21:25]),
		HeaterActive:     p[25] != 0,
	}, nil
}

// ─── Command builders ────────────────────────────────────────────────

// CmdFireOnce builds a GUN_FIRE_ONCE for the given unit id, using
// the currently-armed ROF on the firmware side.
func CmdFireOnce(id byte) []byte {
	return protocol.BuildPacket(FireOnce, []byte{id}, 0)
}

// CmdFireOnceWithRof builds a GUN_FIRE_ONCE that forces a specific
// ROF index for THIS shot — used by Studio's Fire button + dropdown
// so manual testing always has a concrete program even when the RC
// selector stick is between bands (Rule 11 append-only extension,
// 2026-05-24).  rofIndex=0xFF means "use the currently-armed ROF",
// which the firmware also defaults to when the 1-byte form arrives.
func CmdFireOnceWithRof(id, rofIndex byte) []byte {
	return protocol.BuildPacket(FireOnce, []byte{id, rofIndex}, 0)
}

// CmdStartFiring builds a GUN_START_FIRING for `id` at `rpm` rounds /
// minute.  rpm == 0 falls back to the unit's configured default.
func CmdStartFiring(id byte, rpm uint16) []byte {
	p := []byte{id, byte(rpm), byte(rpm >> 8)}
	return protocol.BuildPacket(StartFiring, p, 0)
}

// CmdStartFiringWithRof — same as CmdStartFiring but forces an ROF
// index for the burst (Rule 11 append-only extension, 2026-05-24).
// Used by Studio's Auto-Fire button + dropdown.  rofIndex=0xFF means
// "use the currently-armed ROF".  The forced index applies for the
// duration of the burst and is cleared by GUN_STOP_FIRING.
func CmdStartFiringWithRof(id byte, rpm uint16, rofIndex byte) []byte {
	p := []byte{id, byte(rpm), byte(rpm >> 8), rofIndex}
	return protocol.BuildPacket(StartFiring, p, 0)
}

// CmdStopFiring builds a GUN_STOP_FIRING for the given unit id.
func CmdStopFiring(id byte) []byte {
	return protocol.BuildPacket(StopFiring, []byte{id}, 0)
}

// CmdSmokeArm builds a GUN_SMOKE_ARM (`armed == 0` disables).
func CmdSmokeArm(id, armed byte) []byte {
	return protocol.BuildPacket(SmokeArm, []byte{id, armed}, 0)
}

// CmdStatusReq builds a GUN_STATUS_REQ.
func CmdStatusReq() []byte { return protocol.BuildPacket(StatusReq, nil, 0) }

// CmdManualSet builds a GUN_MANUAL_SET for one gun. The caller picks
// which subsystems are addressed via `state.Flags` (OR of `ManualFlag*`).
// Fields not selected by Flags are filled with 0 on the wire — the
// firmware ignores them.
func CmdManualSet(id byte, state ManualState) []byte {
	p := make([]byte, 10)
	p[0] = id
	p[1] = state.Flags
	binary.LittleEndian.PutUint16(p[2:4], state.YawUs)
	binary.LittleEndian.PutUint16(p[4:6], state.PitchUs)
	p[6] = state.RofIndex
	p[7] = state.FireHold
	p[8] = state.SmokeArm
	p[9] = state.SmokeFanBurst
	return protocol.BuildPacket(ManualSet, p, 0)
}

// CmdManualRelease builds a GUN_MANUAL_RELEASE — drop the gun out of
// manual mode immediately; RC takes over.
func CmdManualRelease(id byte) []byte {
	return protocol.BuildPacket(ManualRelease, []byte{id}, 0)
}

// CmdVerboseStatusReq builds a GUN_VERBOSE_STATUS_REQ. enable=1
// subscribes the caller to ~10 Hz GUN_VERBOSE_STATUS broadcasts for
// the named gun; enable=0 unsubscribes. Subscriptions are released on
// disconnect.
func CmdVerboseStatusReq(id, enable byte) []byte {
	return protocol.BuildPacket(VerboseStatusReq, []byte{id, enable}, 0)
}

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		FireOnce:         "GUN_FIRE_ONCE",
		StartFiring:      "GUN_START_FIRING",
		StopFiring:       "GUN_STOP_FIRING",
		SmokeArm:         "GUN_SMOKE_ARM",
		StatusReq:        "GUN_STATUS_REQ",
		StatusResp:       "GUN_STATUS_RESP",
		ShotEvent:        "GUN_SHOT_EVENT",
		ManualSet:        "GUN_MANUAL_SET",
		ManualRelease:    "GUN_MANUAL_RELEASE",
		VerboseStatusReq: "GUN_VERBOSE_STATUS_REQ",
		VerboseStatusPkt: "GUN_VERBOSE_STATUS",
	})
	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrUnknownID:      "GUN_UNKNOWN_ID",
		ErrTableFull:      "GUN_TABLE_FULL",
		ErrNotImplemented: "GUN_NOT_IMPLEMENTED",
		ErrRofOutOfRange:  "GUN_ROF_OUT_OF_RANGE",
	})
}
