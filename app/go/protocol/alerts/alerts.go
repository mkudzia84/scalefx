// Package alerts mirrors
// controllers/hubfx/esp32s3/src/effects/alerts/alert_protocol.h —
// system-alert audio layer on top of the local mixer.  Owns channel
// HubFxLayout::Alert (== 0) and plays a short preset sound per
// severity level.  Packet slice: 0xD3..0xD6.
package alerts

import (
	"fmt"

	"scalefx/protocol"
)

// ─── Packet types ────────────────────────────────────────────────────

const (
	Beep       protocol.PacketType = 0xD3
	StopAlert  protocol.PacketType = 0xD4
	StatusReq  protocol.PacketType = 0xD5
	StatusResp protocol.PacketType = 0xD6
)

// ─── Severity codes ──────────────────────────────────────────────────

const (
	SeverityInfo     byte = 0
	SeverityWarning  byte = 1
	SeverityError    byte = 2
	SeverityCritical byte = 3
)

// SeverityName returns the canonical label ("info" / "warning" / …).
func SeverityName(s byte) string {
	switch s {
	case SeverityInfo:
		return "info"
	case SeverityWarning:
		return "warning"
	case SeverityError:
		return "error"
	case SeverityCritical:
		return "critical"
	default:
		return fmt.Sprintf("0x%02X?", s)
	}
}

// SeverityFromName is the inverse of SeverityName.  Returns (sev, true)
// on hit or (0, false) on miss.
func SeverityFromName(name string) (byte, bool) {
	switch name {
	case "info":
		return SeverityInfo, true
	case "warning", "warn":
		return SeverityWarning, true
	case "error", "err":
		return SeverityError, true
	case "critical", "crit":
		return SeverityCritical, true
	}
	return 0, false
}

// OutputDefault tells the firmware to use the severity's configured
// output mask (passed as outputMask=0 in the BEEP payload).
const OutputDefault byte = 0

// ─── Error codes ─────────────────────────────────────────────────────

const (
	ErrAlertDisabled       protocol.ErrorCode = 0xD1
	ErrUnknownSeverity     protocol.ErrorCode = 0xD2
	ErrSoundNotConfigured  protocol.ErrorCode = 0xD3
)

// ─── Decoded types ───────────────────────────────────────────────────

// Status is the decoded ALERT_STATUS_RESP payload.
type Status struct {
	Playing      bool `json:"playing"`
	LastSeverity byte `json:"lastSeverity"`
}

// ─── Decoders ────────────────────────────────────────────────────────

// DecodeStatus parses ALERT_STATUS_RESP — [playing:u8][lastSeverity:u8].
func DecodeStatus(p []byte) (Status, error) {
	if len(p) < 2 {
		return Status{}, fmt.Errorf("alert status: need 2 bytes, got %d", len(p))
	}
	return Status{
		Playing:      p[0] != 0,
		LastSeverity: p[1],
	}, nil
}

// ─── Command builders ────────────────────────────────────────────────

// CmdBeep builds an ALERT_BEEP packet.  outputMask == 0 means "use the
// severity preset's configured output mask".
func CmdBeep(severity, outputMask byte) []byte {
	if outputMask == 0 {
		return protocol.BuildPacket(Beep, []byte{severity}, 0)
	}
	return protocol.BuildPacket(Beep, []byte{severity, outputMask}, 0)
}

// CmdStop builds an ALERT_STOP — silences the alert channel.
func CmdStop() []byte { return protocol.BuildPacket(StopAlert, nil, 0) }

// CmdStatusReq builds an ALERT_STATUS_REQ.
func CmdStatusReq() []byte { return protocol.BuildPacket(StatusReq, nil, 0) }

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		Beep:       "ALERT_BEEP",
		StopAlert:  "ALERT_STOP",
		StatusReq:  "ALERT_STATUS_REQ",
		StatusResp: "ALERT_STATUS_RESP",
	})
	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrAlertDisabled:      "ALERT_DISABLED",
		ErrUnknownSeverity:    "ALERT_UNKNOWN_SEVERITY",
		ErrSoundNotConfigured: "ALERT_SOUND_NOT_CONFIGURED",
	})
}
