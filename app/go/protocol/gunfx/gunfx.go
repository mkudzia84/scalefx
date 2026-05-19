// Package gunfx mirrors
// controllers/hubfx/esp32s3/src/effects/gunfx/gunfx_protocol.h —
// the master-side GunFX wire surface.  Each gun unit is muzzle-flash
// LED + optional recoil servo + optional smoke heater + optional RC
// trigger input.  Packet slice: 0xCC..0xD2.
package gunfx

import (
	"fmt"

	"scalefx/protocol"
)

// ─── Packet types ────────────────────────────────────────────────────

const (
	FireOnce    protocol.PacketType = 0xCC
	StartFiring protocol.PacketType = 0xCD
	StopFiring  protocol.PacketType = 0xCE
	SmokeArm    protocol.PacketType = 0xCF
	StatusReq   protocol.PacketType = 0xD0
	StatusResp  protocol.PacketType = 0xD1
	ShotEvent   protocol.PacketType = 0xD2
)

// ─── Error codes ─────────────────────────────────────────────────────

const (
	ErrUnknownID protocol.ErrorCode = 0xCB
	ErrTableFull protocol.ErrorCode = 0xCC
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

// ─── Command builders ────────────────────────────────────────────────

// CmdFireOnce builds a GUN_FIRE_ONCE for the given unit id.
func CmdFireOnce(id byte) []byte {
	return protocol.BuildPacket(FireOnce, []byte{id}, 0)
}

// CmdStartFiring builds a GUN_START_FIRING for `id` at `rpm` rounds /
// minute.  rpm == 0 falls back to the unit's configured default.
func CmdStartFiring(id byte, rpm uint16) []byte {
	p := []byte{id, byte(rpm), byte(rpm >> 8)}
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

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		FireOnce:    "GUN_FIRE_ONCE",
		StartFiring: "GUN_START_FIRING",
		StopFiring:  "GUN_STOP_FIRING",
		SmokeArm:    "GUN_SMOKE_ARM",
		StatusReq:   "GUN_STATUS_REQ",
		StatusResp:  "GUN_STATUS_RESP",
		ShotEvent:   "GUN_SHOT_EVENT",
	})
	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrUnknownID: "GUN_UNKNOWN_ID",
		ErrTableFull: "GUN_TABLE_FULL",
	})
}
