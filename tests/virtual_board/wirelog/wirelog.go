// Virtual board — human-readable wire log.
//
// FormatPacket turns a (type, tag, payload) triple into a one-line
// summary suitable for terminal output. For known packet types it
// decodes the relevant fields; for unknown types it falls back to a
// hex preview so we can still tell roughly what arrived.
//
// The output is column-aligned so a tail looks like a CSV:
//
//   tx STATUS                 tag=00 len=86  ch=[0,100,21,100,_,_,60,0] seq=0xCF
//   rx LFX.LED_SET            tag=05 len=2   ch=1 bright=80
//   rx LFX.LANDING_LIGHT_DEPLOY tag=07 len=1 slot=1
//   tx LFX.LANDING_LIGHT_STATUS tag=00 len=3 slot=1 phase=DEPLOYING done=0

package wirelog

import (
	"fmt"
	"strings"

	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	pgc "scalefx/protocol/gearcontrol"
	pgx "scalefx/protocol/gunfx"
	phub "scalefx/protocol/hubfx"
	plfx "scalefx/protocol/lightfx"
)

// Direction is "tx" / "rx" — used as the leading column.
type Direction string

const (
	TX Direction = "tx"
	RX Direction = "rx"
)

// FormatPacket renders one wire-log line.
func FormatPacket(dir Direction, ptype protocol.PacketType, tag byte, payload []byte) string {
	name := protocol.PacketTypeName(ptype)
	body := decodeBody(ptype, payload)
	if body == "" {
		body = hexPreview(payload, 24)
	}
	return fmt.Sprintf("%s %-26s tag=0x%02X len=%-3d %s",
		dir, name, tag, len(payload), body)
}

// ─── Per-packet decoders ───

func decodeBody(ptype protocol.PacketType, p []byte) string {
	switch ptype {
	// ─ Core ────────────────────────────────────────────────
	case pcore.Init:
		if len(p) >= 2 {
			return fmt.Sprintf("mode=%s flags=0x%02X",
				pcore.InitModeName(p[0]), p[1])
		}
	case pcore.InitReady, pcore.Identify:
		return decodeInitReady(p)
	case pcore.Status:
		return decodeStatus(p)
	case pcore.Ack:
		return ""
	case pcore.Nack:
		if len(p) >= 1 {
			return fmt.Sprintf("err=%s", protocol.ErrorName(protocol.ErrorCode(p[0])))
		}
	case pcore.Keepalive, pcore.Reboot, pcore.Bootsel, pcore.Shutdown,
		pcore.StatusReq:
		return ""
	case pcore.BatteryConfig:
		if len(p) >= 2 {
			return fmt.Sprintf("chemistry=%d cells=%d", p[0], p[1])
		}
	// ─ LightFX ─────────────────────────────────────────────
	case plfx.LedSet:
		if len(p) >= 2 {
			return fmt.Sprintf("ch=%d bright=%d", p[0], p[1])
		}
	case plfx.LedOff:
		if len(p) >= 1 {
			return fmt.Sprintf("ch=%s", chOrAll(p[0]))
		}
	case plfx.LedSeqClear, plfx.LedSeqStart, plfx.LedSeqStop, plfx.LedSeqRestart,
		plfx.LedReset:
		if len(p) >= 1 {
			return fmt.Sprintf("ch=%s", chOrAll(p[0]))
		}
	case plfx.LedSeqAdd:
		if len(p) >= 8 {
			ev := evtName(p[1])
			return fmt.Sprintf("ch=%d evt=%s p1=%d p2=%d p3=%d p4=%d",
				p[0], ev,
				protocol.ReadU16LE(p, 2),
				protocol.ReadU16LE(p, 4),
				p[6], p[7])
		}
	case plfx.LedMasterBrightness:
		if len(p) >= 1 {
			return fmt.Sprintf("pct=%d", p[0])
		}
	case plfx.LedEnable:
		if len(p) >= 2 {
			return fmt.Sprintf("ch=%s enabled=%t", chOrAll(p[0]), p[1] != 0)
		}
	case plfx.LightProgramSelect:
		if len(p) >= 1 {
			return fmt.Sprintf("idx=%d", p[0])
		}
	case plfx.LightProgramReset:
		return ""
	case plfx.ServoSet:
		if len(p) >= 3 {
			return fmt.Sprintf("id=%d pulse=%dµs", p[0], int16(protocol.ReadU16LE(p, 1)))
		}
	case plfx.ServoSettings:
		if len(p) >= 11 {
			return fmt.Sprintf("id=%d min=%d max=%d speed=%d accel=%d decel=%d",
				p[0],
				protocol.ReadU16LE(p, 1), protocol.ReadU16LE(p, 3),
				protocol.ReadU16LE(p, 5), protocol.ReadU16LE(p, 7),
				protocol.ReadU16LE(p, 9))
		}
	case plfx.LandingLightBind:
		if len(p) >= 4 {
			return fmt.Sprintf("slot=%d servoId=%d mask=0x%02X bright=%d",
				p[0], p[1], p[2], p[3])
		}
	case plfx.LandingLightUnbind, plfx.LandingLightDeploy, plfx.LandingLightRetract:
		if len(p) >= 1 {
			return fmt.Sprintf("slot=%s", slotOrAll(p[0]))
		}
	case plfx.LandingLightStatus:
		if len(p) >= 3 {
			return fmt.Sprintf("slot=%d phase=%s finished=%d",
				p[0], landingPhaseName(p[1]), p[2])
		}
	case plfx.BatteryAutoCutoff:
		if len(p) >= 1 {
			return fmt.Sprintf("enabled=%t", p[0] != 0)
		}

	// ─ GearControl ─────────────────────────────────────────
	case pgc.GearDeploy, pgc.GearRetract, pgc.GearStop, pgc.GearReset:
		if len(p) >= 1 {
			return fmt.Sprintf("gear=%d", p[0])
		}
	case pgc.GearAll:
		if len(p) >= 1 {
			op := "deploy"
			if p[0] != 0 {
				op = "retract"
			}
			return fmt.Sprintf("op=%s", op)
		}
	case pgc.GearEnable:
		if len(p) >= 2 {
			return fmt.Sprintf("gear=%d enabled=%t", p[0], p[1] != 0)
		}
	case pgc.ServoSet:
		if len(p) >= 3 {
			return fmt.Sprintf("id=%d pulse=%dµs", p[0], int16(protocol.ReadU16LE(p, 1)))
		}
	case pgc.BatteryAutoDeploy:
		if len(p) >= 1 {
			return fmt.Sprintf("enabled=%t", p[0] != 0)
		}

	// ─ GunFX ───────────────────────────────────────────────
	case pgx.TriggerOn, pgx.TriggerOff:
		return ""
	case pgx.SmokeHeat:
		if len(p) >= 2 {
			return fmt.Sprintf("enable=%t fanSpeed=%d", p[0] != 0, p[1])
		}
	case pgx.ServoSet:
		if len(p) >= 3 {
			return fmt.Sprintf("id=%d pulse=%dµs", p[0], int16(protocol.ReadU16LE(p, 1)))
		}

	// ─ HubFX (file system + slave + audio) ─────────────────
	case phub.SlaveList:
		return ""
	case phub.SlaveListResp:
		if len(p) >= 1 {
			return fmt.Sprintf("count=%d", p[0])
		}
	case phub.FileList, phub.FileTree, phub.FileDownload, phub.FileInfo:
		path, target, ok := decodePathTarget(p)
		if ok {
			return fmt.Sprintf("path=%q target=%s", path, targetName(target))
		}
	case phub.FileDelete, phub.FileMkdir:
		path, target, flags, ok := decodePathTargetFlags(p)
		if ok {
			return fmt.Sprintf("path=%q target=%s flags=0x%02X",
				path, targetName(target), flags)
		}
	case phub.FileUploadBegin:
		if len(p) >= 5 {
			size := protocol.ReadU32LE(p, 0)
			pathLen := int(p[4])
			if 5+pathLen+2 <= len(p) {
				path := string(p[5 : 5+pathLen])
				target := p[5+pathLen]
				mode := p[6+pathLen]
				return fmt.Sprintf("path=%q size=%d target=%s mode=%d",
					path, size, targetName(target), mode)
			}
		}
	case phub.FileUploadData:
		if len(p) >= 4 {
			return fmt.Sprintf("seq=%d crc=0x%04X bytes=%d",
				protocol.ReadU16LE(p, 0), protocol.ReadU16LE(p, 2), len(p)-4)
		}
	case phub.FileUploadEnd, phub.FileUploadCancel:
		return ""
	case phub.FileInfoResp:
		if len(p) >= 5 {
			isDir := p[0] != 0
			size := protocol.ReadU32LE(p, 1)
			return fmt.Sprintf("isDir=%t size=%d", isDir, size)
		}
	case phub.FileUploadProgress:
		if len(p) >= 8 {
			return fmt.Sprintf("sent=%d/%d",
				protocol.ReadU32LE(p, 0), protocol.ReadU32LE(p, 4))
		}

	// ─ Stream ──────────────────────────────────────────────
	case protocol.StreamBegin:
		if len(p) >= 4 {
			return fmt.Sprintf("totalBytes=%d", protocol.ReadU32LE(p, 0))
		}
	case protocol.StreamData:
		if len(p) >= 4 {
			return fmt.Sprintf("seq=%d crc=0x%04X bytes=%d",
				protocol.ReadU16LE(p, 0), protocol.ReadU16LE(p, 2), len(p)-4)
		}
	case protocol.StreamEnd:
		if len(p) >= 8 {
			return fmt.Sprintf("segs=%d totalBytes=%d crc=0x%04X",
				protocol.ReadU16LE(p, 0),
				protocol.ReadU32LE(p, 2),
				protocol.ReadU16LE(p, 6))
		}
	}
	return ""
}

// ─── Helpers ───

func decodeInitReady(p []byte) string {
	if len(p) < 3 {
		return ""
	}
	off := 0
	nameLen := int(p[off])
	off++
	if off+nameLen > len(p) {
		return ""
	}
	name := string(p[off : off+nameLen])
	off += nameLen
	if off >= len(p) {
		return ""
	}
	verLen := int(p[off])
	off++
	if off+verLen > len(p) {
		return ""
	}
	ver := string(p[off : off+verLen])
	return fmt.Sprintf("name=%q version=%q", name, ver)
}

func decodeStatus(p []byte) string {
	if len(p) < 22 {
		return ""
	}
	counter := protocol.ReadU32LE(p, 0)
	uptimeMs := protocol.ReadU32LE(p, 4)
	state := pcore.BoardStateName(p[20])
	flags := p[21]
	moduleLen := len(p) - 22
	return fmt.Sprintf("counter=%d uptime=%dms state=%s flags=0x%02X module=%dB",
		counter, uptimeMs, state, flags, moduleLen)
}

func decodePathTarget(p []byte) (string, byte, bool) {
	if len(p) < 1 {
		return "", 0, false
	}
	pathLen := int(p[0])
	if 1+pathLen+1 > len(p) {
		return "", 0, false
	}
	return string(p[1 : 1+pathLen]), p[1+pathLen], true
}

func decodePathTargetFlags(p []byte) (string, byte, byte, bool) {
	if len(p) < 1 {
		return "", 0, 0, false
	}
	pathLen := int(p[0])
	if 1+pathLen+2 > len(p) {
		return "", 0, 0, false
	}
	return string(p[1 : 1+pathLen]), p[1+pathLen], p[2+pathLen], true
}

func chOrAll(b byte) string {
	if b == 0 {
		return "all"
	}
	return fmt.Sprintf("%d", b)
}

func slotOrAll(b byte) string {
	if b == 0 {
		return "all"
	}
	return fmt.Sprintf("%d", b)
}

func targetName(b byte) string {
	switch b {
	case phub.StorageTargetSd:
		return "sd"
	case phub.StorageTargetFlash:
		return "flash"
	}
	return fmt.Sprintf("0x%02X", b)
}

func evtName(b byte) string {
	switch b {
	case plfx.EvtOn:
		return "ON"
	case plfx.EvtOff:
		return "OFF"
	case plfx.EvtFlash:
		return "FLASH"
	case plfx.EvtFadeIn:
		return "FADE_IN"
	case plfx.EvtFadeOut:
		return "FADE_OUT"
	case plfx.EvtFading:
		return "FADING"
	case plfx.EvtBeacon:
		return "BEACON"
	}
	return fmt.Sprintf("0x%02X", b)
}

func landingPhaseName(b byte) string {
	switch b {
	case 0:
		return "RETRACTED"
	case 1:
		return "DEPLOYING"
	case 2:
		return "DEPLOYED"
	case 3:
		return "RETRACTING"
	}
	return fmt.Sprintf("0x%02X", b)
}

func hexPreview(p []byte, max int) string {
	if len(p) == 0 {
		return ""
	}
	n := len(p)
	if n > max {
		n = max
	}
	parts := make([]string, n)
	for i := 0; i < n; i++ {
		parts[i] = fmt.Sprintf("%02X", p[i])
	}
	out := strings.Join(parts, " ")
	if len(p) > max {
		out += fmt.Sprintf("…(+%d)", len(p)-max)
	}
	return "hex=[" + out + "]"
}
