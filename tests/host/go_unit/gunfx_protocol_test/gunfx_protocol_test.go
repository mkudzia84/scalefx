package gunfx_protocol_test

// Unit tests for the gun-effect wire protocol.
// Pins packet-type byte assignments per CLAUDE.md
// ("GunFX 0xCC-0xD2 + 0xE2-0xE5 (manual override + verbose status)").
// The 0xE2 ManualSet collision history (with audio preload) is the
// specific drift this guards against (see CLAUDE.md collision log).

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/gunfx"
)

func TestGunFxPacketTypeBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"FIRE_ONCE", gunfx.FireOnce, 0xCC},
		{"START_FIRING", gunfx.StartFiring, 0xCD},
		{"STOP_FIRING", gunfx.StopFiring, 0xCE},
		{"SMOKE_ARM", gunfx.SmokeArm, 0xCF},
		{"STATUS_REQ", gunfx.StatusReq, 0xD0},
		{"STATUS_RESP", gunfx.StatusResp, 0xD1},
		{"SHOT_EVENT", gunfx.ShotEvent, 0xD2},
		// Manual override extension — 0xE2 specifically had a known
		// collision with audio's preload status (see CLAUDE.md collision
		// history 2026-05-28).  If this fails, the byte assignment
		// drifted and the dispatcher's BoardOf<> short-circuit at
		// gunfx_service.ownsType() might be claiming bytes that audio
		// owns or vice versa.
		{"MANUAL_SET (0xE2)", gunfx.ManualSet, 0xE2},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X — CLAUDE.md byte-allocation drift",
				tc.name, byte(tc.got), tc.want)
		}
	}
}

func TestGunFxCommandPayloadShapes(t *testing.T) {
	// Most gunfx commands take 1-byte gun ID; status-req takes 0.
	type cmd struct {
		name      string
		raw       []byte
		want      protocol.PacketType
		wantLen   int
	}
	cases := []cmd{
		// FireOnce: [id:u8]
		{"FireOnce(2)", gunfx.CmdFireOnce(2), gunfx.FireOnce, 1},
		// StartFiring: [id:u8][rpm:u16LE]
		{"StartFiring(0, 600)", gunfx.CmdStartFiring(0, 600), gunfx.StartFiring, 3},
		// StopFiring: [id:u8]
		{"StopFiring(0)", gunfx.CmdStopFiring(0), gunfx.StopFiring, 1},
		// StatusReq: empty
		{"StatusReq", gunfx.CmdStatusReq(), gunfx.StatusReq, 0},
	}
	for _, tc := range cases {
		ptype, _, payload, ok := protocol.ParsePacket(tc.raw)
		if !ok {
			t.Errorf("%s: ParsePacket failed", tc.name)
			continue
		}
		if ptype != tc.want {
			t.Errorf("%s: type = 0x%02X, want 0x%02X",
				tc.name, byte(ptype), byte(tc.want))
		}
		if len(payload) != tc.wantLen {
			t.Errorf("%s: payload length = %d, want %d",
				tc.name, len(payload), tc.wantLen)
		}
	}
}

func TestCmdFireOnceCarriesGunId(t *testing.T) {
	for _, gunID := range []byte{0, 1, 2, 7} {
		raw := gunfx.CmdFireOnce(gunID)
		_, _, payload, ok := protocol.ParsePacket(raw)
		if !ok {
			t.Fatalf("ParsePacket failed for FireOnce(%d)", gunID)
		}
		if len(payload) != 1 || payload[0] != gunID {
			t.Errorf("FireOnce(%d) payload = %v, want [%d]", gunID, payload, gunID)
		}
	}
}
