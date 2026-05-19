// Package topology mirrors controllers/hubfx/esp32s3/src/topology/topology_protocol.h
// — the GUID-addressed wire surface that lets a master enumerate ports
// and roles across every connected board (hub + every expander) and
// attach/detach roles by board GUID.
package topology

import (
	"encoding/binary"
	"fmt"

	"scalefx/protocol"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
)

// ─── Packet types (0x88..0x8E) ────────────────────────────────────────

const (
	TopologyPortListReq  protocol.PacketType = 0x88
	TopologyPortListResp protocol.PacketType = 0x89
	TopologyRoleListReq  protocol.PacketType = 0x8A
	TopologyRoleListResp protocol.PacketType = 0x8B
	TopologyRoleAttach   protocol.PacketType = 0x8C
	TopologyRoleDetach   protocol.PacketType = 0x8D
	TopologyRoleEvent    protocol.PacketType = 0x8E
)

// ─── Error codes (0x90..0x96) ─────────────────────────────────────────

const (
	ErrUnknownGuid       protocol.ErrorCode = 0x90
	ErrPendingGuid       protocol.ErrorCode = 0x91
	ErrPortNotFound      protocol.ErrorCode = 0x92
	ErrRoleNotSupported  protocol.ErrorCode = 0x93
	ErrForwardFailed     protocol.ErrorCode = 0x94
	ErrGuidCollision     protocol.ErrorCode = 0x95
	ErrHandshakePending  protocol.ErrorCode = 0x96
)

// ─── Decoded data types ───────────────────────────────────────────────

// BoardPorts is one board's contribution to a TOPOLOGY_PORT_LIST_RESP.
// `GUID == ""` means "the hub".
type BoardPorts struct {
	GUID       string          `json:"guid"`
	DeviceName string          `json:"deviceName"`
	Ports      ports.PortList  `json:"ports"`
}

// BoardRoles is one board's contribution to a TOPOLOGY_ROLE_LIST_RESP.
type BoardRoles struct {
	GUID  string                 `json:"guid"`
	Roles []roles.RoleListEntry  `json:"roles"`
}

// RoleEvent is the decoded TOPOLOGY_ROLE_EVENT — an async role packet
// from one of the boards, re-emitted by the hub with a GUID prefix so
// every consumer sees a unified event stream regardless of origin.
type RoleEvent struct {
	GUID         string              `json:"guid"`
	InnerType    protocol.PacketType `json:"innerType"`
	InnerPayload []byte              `json:"-"`
}

// ─── Decoders ─────────────────────────────────────────────────────────

// DecodePortListResp decodes a TOPOLOGY_PORT_LIST_RESP payload — one
// entry per board, each entry the C++ board's PORT_LIST_RESP layout
// prefixed by the board's GUID + deviceName.
func DecodePortListResp(p []byte) ([]BoardPorts, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("topology port list: empty payload")
	}
	count := int(p[0])
	out := make([]BoardPorts, 0, count)
	off := 1
	for i := 0; i < count; i++ {
		g, no, err := readLenStr(p, off, "guid")
		if err != nil {
			return nil, fmt.Errorf("board %d: %w", i, err)
		}
		off = no
		nm, no, err := readLenStr(p, off, "name")
		if err != nil {
			return nil, fmt.Errorf("board %d: %w", i, err)
		}
		off = no
		// Per-board port enumeration is the same shape as PORT_LIST_RESP.
		// Parse inline because we need to advance `off` as we go.
		var pl ports.PortList
		var perr error
		pl, off, perr = decodeInlinePortBlock(p, off)
		if perr != nil {
			return nil, fmt.Errorf("board %d: %w", i, perr)
		}
		out = append(out, BoardPorts{GUID: g, DeviceName: nm, Ports: pl})
	}
	return out, nil
}

// DecodeRoleListResp decodes a TOPOLOGY_ROLE_LIST_RESP payload.
func DecodeRoleListResp(p []byte) ([]BoardRoles, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("topology role list: empty payload")
	}
	count := int(p[0])
	out := make([]BoardRoles, 0, count)
	off := 1
	for i := 0; i < count; i++ {
		g, no, err := readLenStr(p, off, "guid")
		if err != nil {
			return nil, fmt.Errorf("board %d: %w", i, err)
		}
		off = no
		if off >= len(p) {
			return nil, fmt.Errorf("board %d: missing role count", i)
		}
		rc := int(p[off])
		off++
		if off+4*rc > len(p) {
			return nil, fmt.Errorf("board %d: truncated role entries (need %d)", i, rc)
		}
		entries := make([]roles.RoleListEntry, rc)
		for j := 0; j < rc; j++ {
			entries[j] = roles.RoleListEntry{
				PortKind: p[off],
				PortIdx:  p[off+1],
				RoleKind: p[off+2],
				Flags:    p[off+3],
			}
			off += 4
		}
		out = append(out, BoardRoles{GUID: g, Roles: entries})
	}
	return out, nil
}

// DecodeRoleEvent decodes a TOPOLOGY_ROLE_EVENT async payload.
func DecodeRoleEvent(p []byte) (RoleEvent, error) {
	g, off, err := readLenStr(p, 0, "guid")
	if err != nil {
		return RoleEvent{}, err
	}
	if off >= len(p) {
		return RoleEvent{}, fmt.Errorf("role event: missing inner type")
	}
	ev := RoleEvent{
		GUID:         g,
		InnerType:    protocol.PacketType(p[off]),
		InnerPayload: append([]byte(nil), p[off+1:]...),
	}
	return ev, nil
}

// ─── Command builders ────────────────────────────────────────────────

// guidPrefix builds the leading [guidLen:u8][guid:str] block.  An empty
// guid means "target the hub".
func guidPrefix(guid string) []byte {
	out := make([]byte, 0, 1+len(guid))
	out = append(out, byte(len(guid)))
	out = append(out, []byte(guid)...)
	return out
}

// CmdPortListReq builds a TOPOLOGY_PORT_LIST_REQ.  Empty guid → all boards.
func CmdPortListReq(guid string) []byte {
	return protocol.BuildPacket(TopologyPortListReq, guidPrefix(guid), 0)
}

// CmdRoleListReq builds a TOPOLOGY_ROLE_LIST_REQ.  Empty guid → all boards.
func CmdRoleListReq(guid string) []byte {
	return protocol.BuildPacket(TopologyRoleListReq, guidPrefix(guid), 0)
}

// CmdRoleAttach builds a TOPOLOGY_ROLE_ATTACH for the given (guid, port).
func CmdRoleAttach(guid string, portKind, portIdx, roleKind byte, cfg []byte) []byte {
	payload := guidPrefix(guid)
	payload = append(payload, portKind, portIdx, roleKind, byte(len(cfg)))
	payload = append(payload, cfg...)
	return protocol.BuildPacket(TopologyRoleAttach, payload, 0)
}

// CmdRoleDetach builds a TOPOLOGY_ROLE_DETACH.
func CmdRoleDetach(guid string, portKind, portIdx byte) []byte {
	payload := append(guidPrefix(guid), portKind, portIdx)
	return protocol.BuildPacket(TopologyRoleDetach, payload, 0)
}

// ─── Helpers ─────────────────────────────────────────────────────────

func readLenStr(p []byte, off int, label string) (string, int, error) {
	if off >= len(p) {
		return "", off, fmt.Errorf("topology: missing %s length", label)
	}
	n := int(p[off])
	off++
	if off+n > len(p) {
		return "", off, fmt.Errorf("topology: truncated %s (need %d)", label, n)
	}
	return string(p[off : off+n]), off + n, nil
}

// decodeInlinePortBlock decodes a four-array (servo/pwm/hbridge/input)
// port descriptor block starting at p[off], advancing off as it reads.
// Same shape as PORT_LIST_RESP (ports.h) — duplicated here so this
// package doesn't depend on a partial-decode method on ports.
func decodeInlinePortBlock(p []byte, off int) (ports.PortList, int, error) {
	var pl ports.PortList
	read := func(label string) ([]ports.PortDescriptor, error) {
		if off >= len(p) {
			return nil, fmt.Errorf("missing %s count", label)
		}
		n := int(p[off])
		off++
		if off+2*n > len(p) {
			return nil, fmt.Errorf("truncated %s entries", label)
		}
		entries := make([]ports.PortDescriptor, n)
		for i := 0; i < n; i++ {
			entries[i] = ports.PortDescriptor{Index: p[off], Flags: p[off+1]}
			off += 2
		}
		return entries, nil
	}
	var err error
	if pl.Servos, err = read("servo"); err != nil {
		return pl, off, err
	}
	if pl.Pwms, err = read("pwm"); err != nil {
		return pl, off, err
	}
	if pl.HBridges, err = read("hbridge"); err != nil {
		return pl, off, err
	}
	if pl.Inputs, err = read("input"); err != nil {
		return pl, off, err
	}
	return pl, off, nil
}

// Compile-time check the binary helper is referenced.
var _ = binary.LittleEndian

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		TopologyPortListReq:  "TOPOLOGY_PORT_LIST_REQ",
		TopologyPortListResp: "TOPOLOGY_PORT_LIST_RESP",
		TopologyRoleListReq:  "TOPOLOGY_ROLE_LIST_REQ",
		TopologyRoleListResp: "TOPOLOGY_ROLE_LIST_RESP",
		TopologyRoleAttach:   "TOPOLOGY_ROLE_ATTACH",
		TopologyRoleDetach:   "TOPOLOGY_ROLE_DETACH",
		TopologyRoleEvent:    "TOPOLOGY_ROLE_EVENT",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrUnknownGuid:      "UNKNOWN_GUID",
		ErrPendingGuid:      "PENDING_GUID",
		ErrPortNotFound:     "TOPOLOGY_PORT_NOT_FOUND",
		ErrRoleNotSupported: "ROLE_NOT_SUPPORTED",
		ErrForwardFailed:    "FORWARD_FAILED",
		ErrGuidCollision:    "TOPOLOGY_GUID_COLLISION",
		ErrHandshakePending: "HANDSHAKE_PENDING",
	})
}
