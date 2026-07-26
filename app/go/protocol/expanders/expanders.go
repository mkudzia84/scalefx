// Package expanders mirrors controllers/hubfx/esp32s3/src/expanders/expander_protocol.h
// — the HubFX-side wire protocol for tracking downstream USB-CDC expanders
// (GunFX / LightFX / GearControl) and their identify state.
package expanders

import (
	"encoding/binary"
	"fmt"

	"scalefx/protocol"
)

// ─── ExpanderKind — single-byte enum, append-only (Rule 11) ───────────

const (
	KindUnknown      byte = 0x00
	KindGunFX        byte = 0x01
	KindLightFX      byte = 0x02
	KindGearControl  byte = 0x03
	KindPicoDefault  byte = 0x04
	KindPortExpander byte = 0x05
)

// KindName returns a human-readable name for an expander kind byte.
func KindName(k byte) string {
	switch k {
	case KindGunFX:
		return "GunFX"
	case KindLightFX:
		return "LightFX"
	case KindGearControl:
		return "GearControl"
	case KindPicoDefault:
		return "Pico(default-PID)"
	case KindPortExpander:
		return "PortExpander"
	default:
		return "Unknown"
	}
}

// ─── Packet types (0x80..0x87) ────────────────────────────────────────

const (
	ExpanderListReq        protocol.PacketType = 0x80
	ExpanderListResp       protocol.PacketType = 0x81
	ExpanderConnected      protocol.PacketType = 0x82
	ExpanderDisconnected   protocol.PacketType = 0x83
	ExpanderIdentified     protocol.PacketType = 0x84
	ExpanderSystemInfoReq  protocol.PacketType = 0x85
	ExpanderSystemInfoResp protocol.PacketType = 0x86
	ExpanderCollision      protocol.PacketType = 0x87
)

// ─── Error codes ──────────────────────────────────────────────────────

const (
	ErrUsbHostDown        protocol.ErrorCode = 0x80
	ErrExpanderTableFull  protocol.ErrorCode = 0x81
	ErrIdentifyFailed     protocol.ErrorCode = 0x82
	ErrGuidCollision      protocol.ErrorCode = 0x83
)

// ─── Decoded data types ───────────────────────────────────────────────

// ExpanderEntry mirrors the C++ struct of the same name — one live USB
// expander slot.
type ExpanderEntry struct {
	Kind       byte   `json:"kind"`
	KindName   string `json:"kindName"`
	USBAddr    byte   `json:"usbAddr"`
	VID        uint16 `json:"vid"`
	PID        uint16 `json:"pid"`
	Identified bool   `json:"identified"`
	Collision  bool   `json:"collision"`

	// Filled iff Identified.
	GUID            string `json:"guid,omitempty"`
	DeviceName      string `json:"deviceName,omitempty"`
	FirmwareVersion string `json:"firmwareVersion,omitempty"`
	Capabilities    uint32 `json:"capabilities,omitempty"`
	BuildNumber     uint32 `json:"buildNumber,omitempty"`

	// Battery telemetry — present only in EXPANDER_SYSTEM_INFO_RESP from a
	// hub new enough to append it (Rule 11), and only for BATTERY-capable
	// expanders. nil ⇒ the hub didn't report a battery section.
	Battery *BatteryInfo `json:"battery,omitempty"`
}

// BatteryInfo mirrors the per-expander battery section the hub polls from
// each BATTERY-capable expander's STATUS_REQ. Valid=false ⇒ no reading yet.
type BatteryInfo struct {
	Valid     bool   `json:"valid"`
	Present   bool   `json:"present"`
	VoltageMV uint16 `json:"voltage_mV"`
	CellCount byte   `json:"cellCount"`
	Pct       byte   `json:"pct"`
	Flags     byte   `json:"flags"` // bit0 = low, bit1 = critical
}

// Low reports the low-voltage flag. Critical reports the cutoff flag.
func (b BatteryInfo) Low() bool      { return b.Flags&0x01 != 0 }
func (b BatteryInfo) Critical() bool { return b.Flags&0x02 != 0 }

// HubInfo mirrors the hub block of EXPANDER_SYSTEM_INFO_RESP.
type HubInfo struct {
	GUID            string `json:"guid"`
	DeviceName      string `json:"deviceName"`
	FirmwareVersion string `json:"firmwareVersion"`
	Platform        string `json:"platform"`
	CPUFreqMHz      uint32 `json:"cpuFreqMHz"`
	FreeRAMBytes    uint32 `json:"freeRamBytes"`
	BuildNumber     uint32 `json:"buildNumber"`
	Capabilities    uint32 `json:"capabilities"`
}

// SystemInfo holds the decoded EXPANDER_SYSTEM_INFO_RESP — the hub
// identity plus every connected expander in a single round-trip.
type SystemInfo struct {
	Hub       HubInfo         `json:"hub"`
	Expanders []ExpanderEntry `json:"expanders"`
}

// CollisionEvent is the decoded EXPANDER_COLLISION async payload.
type CollisionEvent struct {
	GUID     string `json:"guid"`
	USBAddrA byte   `json:"usbAddrA"`
	USBAddrB byte   `json:"usbAddrB"`
}

// ConnectedEvent — EXPANDER_CONNECTED async payload (pre-identify).
type ConnectedEvent struct {
	Kind     byte   `json:"kind"`
	KindName string `json:"kindName"`
	USBAddr  byte   `json:"usbAddr"`
	VID      uint16 `json:"vid"`
	PID      uint16 `json:"pid"`
}

// DisconnectedEvent — EXPANDER_DISCONNECTED async payload.
type DisconnectedEvent struct {
	Kind     byte   `json:"kind"`
	KindName string `json:"kindName"`
	USBAddr  byte   `json:"usbAddr"`
	GUID     string `json:"guid,omitempty"`
}

// ─── Decoders ─────────────────────────────────────────────────────────

// DecodeExpanderList decodes an EXPANDER_LIST_RESP payload.
func DecodeExpanderList(p []byte) ([]ExpanderEntry, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("expander list: empty payload")
	}
	count := int(p[0])
	out := make([]ExpanderEntry, 0, count)
	off := 1
	for i := 0; i < count; i++ {
		if off+7 > len(p) {
			return nil, fmt.Errorf("expander list: truncated header (entry %d)", i)
		}
		e := ExpanderEntry{
			Kind:       p[off],
			USBAddr:    p[off+1],
			VID:        binary.LittleEndian.Uint16(p[off+2 : off+4]),
			PID:        binary.LittleEndian.Uint16(p[off+4 : off+6]),
			Identified: p[off+6] != 0,
		}
		off += 7
		if off >= len(p) {
			return nil, fmt.Errorf("expander list: truncated collision byte")
		}
		e.Collision = p[off] != 0
		off++
		e.KindName = KindName(e.Kind)

		if e.Identified {
			g, nOff, err := readLenStr(p, off, "guid")
			if err != nil {
				return nil, err
			}
			e.GUID = g
			n, nOff, err := readLenStr(p, nOff, "name")
			if err != nil {
				return nil, err
			}
			e.DeviceName = n
			v, nOff, err := readLenStr(p, nOff, "ver")
			if err != nil {
				return nil, err
			}
			e.FirmwareVersion = v
			if nOff+8 > len(p) {
				return nil, fmt.Errorf("expander list: truncated caps/build (entry %d)", i)
			}
			e.Capabilities = binary.LittleEndian.Uint32(p[nOff : nOff+4])
			e.BuildNumber = binary.LittleEndian.Uint32(p[nOff+4 : nOff+8])
			off = nOff + 8
		}
		out = append(out, e)
	}
	return out, nil
}

// DecodeSystemInfo decodes EXPANDER_SYSTEM_INFO_RESP — hub block + every
// expander's spec, all in one packet.
func DecodeSystemInfo(p []byte) (SystemInfo, error) {
	var si SystemInfo
	off := 0
	g, off, err := readLenStr(p, off, "hub guid")
	if err != nil {
		return si, err
	}
	n, off, err := readLenStr(p, off, "hub name")
	if err != nil {
		return si, err
	}
	v, off, err := readLenStr(p, off, "hub ver")
	if err != nil {
		return si, err
	}
	pl, off, err := readLenStr(p, off, "hub plat")
	if err != nil {
		return si, err
	}
	if off+16 > len(p) {
		return si, fmt.Errorf("system info: truncated hub trailer")
	}
	si.Hub = HubInfo{
		GUID:            g,
		DeviceName:      n,
		FirmwareVersion: v,
		Platform:        pl,
		CPUFreqMHz:      binary.LittleEndian.Uint32(p[off : off+4]),
		FreeRAMBytes:    binary.LittleEndian.Uint32(p[off+4 : off+8]),
		BuildNumber:     binary.LittleEndian.Uint32(p[off+8 : off+12]),
		Capabilities:    binary.LittleEndian.Uint32(p[off+12 : off+16]),
	}
	off += 16

	if off >= len(p) {
		return si, fmt.Errorf("system info: missing expander count")
	}
	count := int(p[off])
	off++

	si.Expanders = make([]ExpanderEntry, 0, count)
	for i := 0; i < count; i++ {
		if off+4 > len(p) {
			return si, fmt.Errorf("system info: truncated expander header (entry %d)", i)
		}
		e := ExpanderEntry{
			Kind:       p[off],
			USBAddr:    p[off+1],
			Identified: p[off+2] != 0,
			Collision:  p[off+3] != 0,
		}
		e.KindName = KindName(e.Kind)
		off += 4
		if e.Identified {
			g, off, err = readLenStr(p, off, "exp guid")
			if err != nil {
				return si, err
			}
			e.GUID = g
			n, off, err = readLenStr(p, off, "exp name")
			if err != nil {
				return si, err
			}
			e.DeviceName = n
			v, off, err = readLenStr(p, off, "exp ver")
			if err != nil {
				return si, err
			}
			e.FirmwareVersion = v
			if off+8 > len(p) {
				return si, fmt.Errorf("system info: truncated exp trailer (entry %d)", i)
			}
			e.Capabilities = binary.LittleEndian.Uint32(p[off : off+4])
			e.BuildNumber = binary.LittleEndian.Uint32(p[off+4 : off+8])
			off += 8

			// Battery section (Rule 11 append-only) — present only from a
			// hub new enough to emit it; length-guard so older hubs decode.
			if off+7 <= len(p) {
				e.Battery = &BatteryInfo{
					Valid:     p[off] != 0,
					Present:   p[off+1] != 0,
					VoltageMV: binary.LittleEndian.Uint16(p[off+2 : off+4]),
					CellCount: p[off+4],
					Pct:       p[off+5],
					Flags:     p[off+6],
				}
				off += 7
			}
		}
		si.Expanders = append(si.Expanders, e)
	}
	return si, nil
}

// DecodeIdentifiedEvent decodes an EXPANDER_IDENTIFIED async payload.
// Returns the ExpanderEntry the event would produce.
func DecodeIdentifiedEvent(p []byte) (ExpanderEntry, error) {
	if len(p) < 2 {
		return ExpanderEntry{}, fmt.Errorf("identified: truncated header")
	}
	e := ExpanderEntry{Kind: p[0], USBAddr: p[1], Identified: true}
	e.KindName = KindName(e.Kind)
	off := 2
	g, off, err := readLenStr(p, off, "guid")
	if err != nil {
		return e, err
	}
	e.GUID = g
	n, off, err := readLenStr(p, off, "name")
	if err != nil {
		return e, err
	}
	e.DeviceName = n
	v, off, err := readLenStr(p, off, "ver")
	if err != nil {
		return e, err
	}
	e.FirmwareVersion = v
	if off+8 > len(p) {
		return e, fmt.Errorf("identified: truncated trailer")
	}
	e.Capabilities = binary.LittleEndian.Uint32(p[off : off+4])
	e.BuildNumber = binary.LittleEndian.Uint32(p[off+4 : off+8])
	return e, nil
}

// DecodeConnectedEvent decodes EXPANDER_CONNECTED.
func DecodeConnectedEvent(p []byte) (ConnectedEvent, error) {
	if len(p) != 6 {
		return ConnectedEvent{}, fmt.Errorf("connected: expected 6 bytes, got %d", len(p))
	}
	return ConnectedEvent{
		Kind:     p[0],
		KindName: KindName(p[0]),
		USBAddr:  p[1],
		VID:      binary.LittleEndian.Uint16(p[2:4]),
		PID:      binary.LittleEndian.Uint16(p[4:6]),
	}, nil
}

// DecodeDisconnectedEvent decodes EXPANDER_DISCONNECTED.
func DecodeDisconnectedEvent(p []byte) (DisconnectedEvent, error) {
	if len(p) < 2 {
		return DisconnectedEvent{}, fmt.Errorf("disconnected: truncated")
	}
	e := DisconnectedEvent{Kind: p[0], USBAddr: p[1], KindName: KindName(p[0])}
	if len(p) > 2 {
		g, _, err := readLenStr(p, 2, "guid")
		if err != nil {
			return e, err
		}
		e.GUID = g
	}
	return e, nil
}

// DecodeCollisionEvent decodes EXPANDER_COLLISION.
func DecodeCollisionEvent(p []byte) (CollisionEvent, error) {
	if len(p) < 1 {
		return CollisionEvent{}, fmt.Errorf("collision: empty payload")
	}
	g, off, err := readLenStr(p, 0, "guid")
	if err != nil {
		return CollisionEvent{}, err
	}
	if off+2 > len(p) {
		return CollisionEvent{}, fmt.Errorf("collision: truncated addrs")
	}
	return CollisionEvent{GUID: g, USBAddrA: p[off], USBAddrB: p[off+1]}, nil
}

// ─── Command builders ────────────────────────────────────────────────

func CmdExpanderListReq() []byte       { return protocol.BuildPacket(ExpanderListReq, nil, 0) }
func CmdExpanderSystemInfoReq() []byte { return protocol.BuildPacket(ExpanderSystemInfoReq, nil, 0) }

// ─── Helpers ─────────────────────────────────────────────────────────

func readLenStr(p []byte, off int, label string) (string, int, error) {
	if off >= len(p) {
		return "", off, fmt.Errorf("expanders: missing %s length", label)
	}
	n := int(p[off])
	off++
	if off+n > len(p) {
		return "", off, fmt.Errorf("expanders: truncated %s (need %d)", label, n)
	}
	return string(p[off : off+n]), off + n, nil
}

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		ExpanderListReq:        "EXPANDER_LIST_REQ",
		ExpanderListResp:       "EXPANDER_LIST_RESP",
		ExpanderConnected:      "EXPANDER_CONNECTED",
		ExpanderDisconnected:   "EXPANDER_DISCONNECTED",
		ExpanderIdentified:     "EXPANDER_IDENTIFIED",
		ExpanderSystemInfoReq:  "EXPANDER_SYSTEM_INFO_REQ",
		ExpanderSystemInfoResp: "EXPANDER_SYSTEM_INFO_RESP",
		ExpanderCollision:      "EXPANDER_COLLISION",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrUsbHostDown:       "USB_HOST_DOWN",
		ErrExpanderTableFull: "EXPANDER_TABLE_FULL",
		ErrIdentifyFailed:    "IDENTIFY_FAILED",
		ErrGuidCollision:     "GUID_COLLISION",
	})
}
