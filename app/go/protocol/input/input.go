// Package input mirrors the HubFX input-routing control packets
// (controllers/hubfx/esp32s3/src/effects/input/input_protocol.h).
//
// These toggle the master's global RC → effect routing gate.  When
// routing is disabled the InputDispatcher stops feeding effect triggers
// (RC sticks no longer drive engine / gun / etc.; effects hold their
// last state), while the input broadcast that monitors read keeps
// flowing.  Protocol-agnostic: one flag covers PPM / SBUS / Jeti EX.
package input

import "scalefx/protocol"

// Wire packet types — twin of InputRoutingPacket + TelemetryPacket in
// input_protocol.h.
const (
	RoutingSetEnabled protocol.PacketType = 0xE8 // [enabled:u8] → ACK
	RoutingGetReq     protocol.PacketType = 0xE9 // [] → RoutingResp
	RoutingResp       protocol.PacketType = 0xEA // [enabled:u8]

	// Telemetry collection (item 4).
	TelemetryGetReq protocol.PacketType = 0xEB // [] → TelemetryResp
	TelemetryResp   protocol.PacketType = 0xEC // snapshot (see DecodeTelemetry)
	TelemetryUpdate protocol.PacketType = 0xED // async live push (reserved)

	// Generic input connection-loss (item 5).
	ConnectionEvent  protocol.PacketType = 0xAD // async [link…][state][brownouts]
	ConnectionGetReq protocol.PacketType = 0xAE // [] → ConnectionResp
	ConnectionResp   protocol.PacketType = 0xAF // [linkLossMs:u16][count] then links
	ConnectionSetCfg protocol.PacketType = 0xA8 // [linkLossMs:u16] → ACK
)

// Connection-loss link states.
const (
	LinkUp   = 0 // up / recovered
	LinkLost = 1 // signal gone (holding + brownout)
	LinkDown = 2 // loss confirmed past the interval (actionable)
)

// CmdSetRouting builds the SET packet (enabled = RC drives effects).
func CmdSetRouting(enabled bool) []byte {
	b := byte(0)
	if enabled {
		b = 1
	}
	return protocol.BuildPacket(RoutingSetEnabled, []byte{b}, 0)
}

// CmdGetRouting builds the GET request (reply is RoutingResp).
func CmdGetRouting() []byte {
	return protocol.BuildPacket(RoutingGetReq, nil, 0)
}

// DecodeRouting reads the [enabled:u8] payload of a RoutingResp.
func DecodeRouting(payload []byte) (bool, bool) {
	if len(payload) < 1 {
		return false, false
	}
	return payload[0] != 0, true
}

// ─── Telemetry collection (item 4) ──────────────────────────────────

// TelemetrySensor is one metric in the collection.
type TelemetrySensor struct {
	ID       byte   `json:"id"`
	Type     byte   `json:"type"`     // Jeti ExDataType (Int6/14/22/30)
	Decimals byte   `json:"decimals"` // implied decimal places
	Active   bool   `json:"active"`
	Value    int32  `json:"value"` // raw scaled value (apply Decimals for display)
	Label    string `json:"label"`
	Unit     string `json:"unit"`
}

// TelemetryDevice is one source — the hub itself (Local) or an actively-polled
// input device (e.g. an ESC on IN_2).
type TelemetryDevice struct {
	USN     uint16            `json:"usn"`
	LSN     uint16            `json:"lsn"`
	Local   bool              `json:"local"`
	Active  bool              `json:"active"`
	Name    string            `json:"name"`
	Sensors []TelemetrySensor `json:"sensors"`
}

// TelemetrySnapshot is the whole collection plus the publish-rate stats.
type TelemetrySnapshot struct {
	PubIntervalMs uint16            `json:"pubIntervalMs"` // target reply interval
	RespHzX10     uint16            `json:"respHzX10"`     // publish rate × 10
	ActiveSensors byte              `json:"activeSensors"`
	Devices       []TelemetryDevice `json:"devices"`
}

// CmdGetTelemetry builds the snapshot request (reply is TelemetryResp).
func CmdGetTelemetry() []byte {
	return protocol.BuildPacket(TelemetryGetReq, nil, 0)
}

// DecodeTelemetry parses a TelemetryResp / TelemetryUpdate body. Layout mirrors
// InputDispatcher::buildTelemetrySnapshot (little-endian, length-prefixed
// strings). Returns false on a short/malformed payload.
func DecodeTelemetry(p []byte) (TelemetrySnapshot, bool) {
	var s TelemetrySnapshot
	r := reader{p, 0}
	pub, ok1 := r.u16()
	hz, ok2 := r.u16()
	active, ok3 := r.u8()
	devN, ok4 := r.u8()
	if !(ok1 && ok2 && ok3 && ok4) {
		return s, false
	}
	s.PubIntervalMs, s.RespHzX10, s.ActiveSensors = pub, hz, active
	for i := 0; i < int(devN); i++ {
		var d TelemetryDevice
		usn, a := r.u16()
		lsn, b := r.u16()
		flags, c := r.u8()
		senN, e := r.u8()
		name, f := r.str()
		if !(a && b && c && e && f) {
			return s, false
		}
		d.USN, d.LSN = usn, lsn
		d.Local = flags&0x01 != 0
		d.Active = flags&0x02 != 0
		d.Name = name
		for j := 0; j < int(senN); j++ {
			var sn TelemetrySensor
			id, a := r.u8()
			ty, b := r.u8()
			dec, c := r.u8()
			sf, e := r.u8()
			val, g := r.i32()
			label, h := r.str()
			unit, k := r.str()
			if !(a && b && c && e && g && h && k) {
				return s, false
			}
			sn.ID, sn.Type, sn.Decimals = id, ty, dec
			sn.Active = sf&0x01 != 0
			sn.Value = val
			sn.Label, sn.Unit = label, unit
			d.Sensors = append(d.Sensors, sn)
		}
		s.Devices = append(s.Devices, d)
	}
	return s, true
}

// ─── Connection-loss (item 5) ───────────────────────────────────────

// LinkStatus is one tracked input source's connection health.
type LinkStatus struct {
	PortKind  byte   `json:"portKind"`
	PortIdx   byte   `json:"portIdx"`
	GUID      string `json:"guid"` // "" = hub-local
	State     byte   `json:"state"`
	Brownouts uint16 `json:"brownouts"`
	SilenceMs uint16 `json:"silenceMs"`
}

// ConnectionStatus is the whole connection-health snapshot.
type ConnectionStatus struct {
	LinkLossMs uint16       `json:"linkLossMs"`
	Links      []LinkStatus `json:"links"`
}

// ConnectionEventT is one async CONNECTION_EVENT (a link changing state).
type ConnectionEventT struct {
	PortKind  byte   `json:"portKind"`
	PortIdx   byte   `json:"portIdx"`
	GUID      string `json:"guid"`
	State     byte   `json:"state"`
	Brownouts uint16 `json:"brownouts"`
}

// CmdGetConnection builds the connection-status request.
func CmdGetConnection() []byte { return protocol.BuildPacket(ConnectionGetReq, nil, 0) }

// CmdSetLinkLoss builds the SET packet (global link-loss interval; 0 disables).
func CmdSetLinkLoss(ms uint16) []byte {
	return protocol.BuildPacket(ConnectionSetCfg, []byte{byte(ms), byte(ms >> 8)}, 0)
}

// DecodeConnectionResp parses a ConnectionResp body.
func DecodeConnectionResp(p []byte) (ConnectionStatus, bool) {
	var s ConnectionStatus
	r := reader{p, 0}
	ms, ok := r.u16()
	cnt, ok2 := r.u8()
	if !(ok && ok2) {
		return s, false
	}
	s.LinkLossMs = ms
	for i := 0; i < int(cnt); i++ {
		var l LinkStatus
		pk, a := r.u8()
		pi, b := r.u8()
		g, c := r.str()
		st, d := r.u8()
		bo, e := r.u16()
		sil, f := r.u16()
		if !(a && b && c && d && e && f) {
			return s, false
		}
		l.PortKind, l.PortIdx, l.GUID = pk, pi, g
		l.State, l.Brownouts, l.SilenceMs = st, bo, sil
		s.Links = append(s.Links, l)
	}
	return s, true
}

// DecodeConnectionEvent parses an async CONNECTION_EVENT body.
func DecodeConnectionEvent(p []byte) (ConnectionEventT, bool) {
	var e ConnectionEventT
	r := reader{p, 0}
	pk, a := r.u8()
	pi, b := r.u8()
	g, c := r.str()
	st, d := r.u8()
	bo, f := r.u16()
	if !(a && b && c && d && f) {
		return e, false
	}
	e.PortKind, e.PortIdx, e.GUID, e.State, e.Brownouts = pk, pi, g, st, bo
	return e, true
}

// reader is a tiny bounds-checked little-endian cursor over the payload.
type reader struct {
	b []byte
	i int
}

func (r *reader) u8() (byte, bool) {
	if r.i+1 > len(r.b) {
		return 0, false
	}
	v := r.b[r.i]
	r.i++
	return v, true
}
func (r *reader) u16() (uint16, bool) {
	if r.i+2 > len(r.b) {
		return 0, false
	}
	v := uint16(r.b[r.i]) | uint16(r.b[r.i+1])<<8
	r.i += 2
	return v, true
}
func (r *reader) i32() (int32, bool) {
	if r.i+4 > len(r.b) {
		return 0, false
	}
	v := uint32(r.b[r.i]) | uint32(r.b[r.i+1])<<8 | uint32(r.b[r.i+2])<<16 | uint32(r.b[r.i+3])<<24
	r.i += 4
	return int32(v), true
}
func (r *reader) str() (string, bool) {
	n, ok := r.u8()
	if !ok || r.i+int(n) > len(r.b) {
		return "", false
	}
	v := string(r.b[r.i : r.i+int(n)])
	r.i += int(n)
	return v, true
}
