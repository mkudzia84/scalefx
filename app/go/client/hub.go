package client

import (
	"encoding/binary"
	"fmt"
	"strings"
	"time"

	"scalefx/protocol"
	"scalefx/protocol/core"
)

// Hub exposes the core wire surface every ScaleFX board speaks: INIT,
// IDENTIFY, STATUS, REBOOT, KEEPALIVE, etc.
type Hub struct{ c *Client }

// Identity is the decoded INIT_READY / IDENTIFY payload.  Subsystem,
// service, and port-kind presence are ALL surfaced through the
// `Capabilities` bitmask (see `protocol/core` for the catalog) — there
// is no separate numeric tail.  Numeric details (exact port counts,
// audio channel count) are queried on demand via PORT_LIST_REQ /
// AUDIO_STATUS_REQ when the relevant bit is set.
type Identity struct {
	DeviceName      string `json:"deviceName"`
	GUID            string `json:"guid"` // 4-hex-char suffix of DeviceName
	FirmwareVersion string `json:"firmwareVersion"`
	Platform        string `json:"platform"`
	CPUFreqMHz      uint32 `json:"cpuFreqMHz"`
	FreeRAMBytes    uint32 `json:"freeRamBytes"`
	BuildNumber     uint32 `json:"buildNumber"`
	// Compiled-in feature bitmask — what the firmware can do AT ALL.
	Capabilities uint32 `json:"capabilities"`
	// Runtime-enabled subset of `Capabilities` — what the firmware is
	// configured to do RIGHT NOW.  Hosts use this to gate command
	// visibility separately from "is the feature even compiled in".
	// Pre-v2.4 firmware doesn't emit this field; we default it to
	// `Capabilities` so older boards keep working unchanged.
	EnabledCapabilities uint32 `json:"enabledCapabilities"`
}

// BoardKind is the canonical board-family label derived from the
// DeviceName prefix.  Used by callers (CLI, Studio) to drive
// per-board UI without hardcoding GUID lookups.
type BoardKind string

const (
	BoardHubFX       BoardKind = "hubfx"
	BoardLightFX     BoardKind = "lightfx"
	BoardGunFX       BoardKind = "gunfx"
	BoardGearControl BoardKind = "gearcontrol"
	BoardUnknown     BoardKind = ""
)

// Kind returns the BoardKind inferred from the device-name prefix.
func (i Identity) Kind() BoardKind {
	switch {
	case strings.HasPrefix(i.DeviceName, "HubFx"):
		return BoardHubFX
	case strings.HasPrefix(i.DeviceName, "LightFx"):
		return BoardLightFX
	case strings.HasPrefix(i.DeviceName, "GunFx"):
		return BoardGunFX
	case strings.HasPrefix(i.DeviceName, "GearControl"):
		return BoardGearControl
	}
	return BoardUnknown
}

// Has reports whether every bit in `want` is set in the
// COMPILED-IN `Capabilities` mask.  Use this for "could this firmware
// ever do X" — for "is X live right now" use `IsEnabled`.  Use the
// `core.Cap*` constants:
//
//	id.Has(core.CapAudio)                    // audio mixer present
//	id.Has(core.CapTopology)                 // GUID-addressed master
//	id.Has(core.CapHasServoPorts)            // at least one servo port
//	id.Has(core.CapSd | core.CapFlash)       // either storage backend
func (i Identity) Has(want uint32) bool {
	return core.HasCapability(i.Capabilities, want)
}

// HasAny reports whether ANY bit in `want` is set in `Capabilities`.
func (i Identity) HasAny(want uint32) bool { return i.Capabilities&want != 0 }

// IsEnabled reports whether every bit in `want` is set in
// `EnabledCapabilities` — i.e. the feature is BOTH compiled in AND
// runtime-enabled by the current config.  This is the predicate the
// CLI verb-filter and Studio UI should use to decide whether a
// command is callable right now.  For "is this firmware capable in
// principle" use `Has`.
func (i Identity) IsEnabled(want uint32) bool {
	return core.HasCapability(i.EnabledCapabilities, want)
}

// CapabilityNames returns symbolic names of every COMPILED-IN feature.
func (i Identity) CapabilityNames() []string {
	return core.CapabilityNames(i.Capabilities)
}

// EnabledCapabilityNames returns symbolic names of every RUNTIME-ENABLED
// feature (subset of `CapabilityNames`).
func (i Identity) EnabledCapabilityNames() []string {
	return core.CapabilityNames(i.EnabledCapabilities)
}

// DisabledCapabilityNames returns symbolic names of features that are
// compiled in but NOT runtime-enabled — i.e. config-disabled.
func (i Identity) DisabledCapabilityNames() []string {
	return core.CapabilityNames(i.Capabilities & ^i.EnabledCapabilities)
}

// ─── Convenience predicates ──────────────────────────────────────────

func (i Identity) HasAudio() bool     { return i.Has(core.CapAudio) }
func (i Identity) HasStorage() bool   { return i.HasAny(core.CapSd | core.CapFlash) }
func (i Identity) HasUSBHost() bool   { return i.Has(core.CapUsbHost) }
func (i Identity) HasExpanders() bool { return i.Has(core.CapExpanderBus) }
func (i Identity) HasTopology() bool  { return i.Has(core.CapTopology) }
func (i Identity) HasBattery() bool   { return i.Has(core.CapBattery) }
func (i Identity) HasConfig() bool    { return i.Has(core.CapConfig) }
func (i Identity) HasEngine() bool    { return i.Has(core.CapEngine) }

func (i Identity) HasServoPorts() bool   { return i.Has(core.CapHasServoPorts) }
func (i Identity) HasPwmPorts() bool     { return i.Has(core.CapHasPwmPorts) }
func (i Identity) HasHBridgePorts() bool { return i.Has(core.CapHasHBridgePorts) }
func (i Identity) HasInputPorts() bool   { return i.Has(core.CapHasInputPorts) }

// HasAnyPorts is true iff at least one HAS_*_PORTS bit is set — i.e.
// this firmware declared any port at all.
func (i Identity) HasAnyPorts() bool {
	return i.HasAny(core.CapHasServoPorts | core.CapHasPwmPorts |
		core.CapHasHBridgePorts | core.CapHasInputPorts)
}

// Identify sends IDENTIFY and decodes the response.  Safe before INIT —
// does not activate hardware.  The firmware may reply with either
// IDENTIFY (echo, master query path) or INIT_READY (post-init); both
// carry the same payload shape.
func (h *Hub) Identify() (Identity, error) {
	resp, err := h.c.conn.SendAndWait(core.CmdIdentify())
	if err != nil {
		return Identity{}, err
	}
	if resp.IsNACK() {
		return Identity{}, errFromNack(resp)
	}
	if resp.PacketType != core.Identify && resp.PacketType != core.InitReady {
		return Identity{}, fmt.Errorf("expected IDENTIFY or INIT_READY, got %s",
			resp.PacketType)
	}
	return decodeIdentity(resp.Payload)
}

// Init sends a default-mode INIT (SLAVE, flags=0).  Idempotent.
func (h *Hub) Init() error {
	return h.c.sendExpectACK(core.CmdInit())
}

// InitMode sends INIT with explicit (mode, flags).
func (h *Hub) InitMode(mode, flags byte) error {
	return h.c.sendExpectACK(core.CmdInitMode(mode, flags))
}

// Shutdown sends SHUTDOWN.  The board stays connected but releases its
// init state.
func (h *Hub) Shutdown() error {
	return h.c.sendExpectACK(core.CmdShutdown())
}

// Reboot fires REBOOT (fire-and-forget — the board resets before any
// ACK can land).
func (h *Hub) Reboot() error {
	return h.c.conn.Send(core.CmdReboot())
}

// Bootsel asks the board to enter the bootloader.  Pico-only on the
// firmware side; ESP32 returns NOT_SUPPORTED.
func (h *Hub) Bootsel() error {
	return h.c.conn.Send(core.CmdBootsel())
}

// Keepalive sends a single KEEPALIVE packet (no ACK expected).
func (h *Hub) Keepalive() error {
	return h.c.conn.Send(core.CmdKeepalive())
}

// StartKeepalive begins a background keepalive loop at `interval`.
// Stops automatically on Close().
func (h *Hub) StartKeepalive(interval time.Duration) {
	h.c.conn.StartKeepalive(interval)
}

// StopKeepalive stops the background keepalive loop.
func (h *Hub) StopKeepalive() { h.c.conn.StopKeepalive() }

// Status is the decoded STATUS payload — the 22-byte core header, the
// module-data trailer (each policy renders its own shape into it),
// then an optional 8-byte memory extension at the very end (firmware
// builds that ship this field after 2026-05-28 — Rule 11 append-only).
type Status struct {
	Counter         uint32 `json:"counter"`
	UptimeMs        uint32 `json:"uptimeMs"`
	FreeRAMBytes    uint32 `json:"freeRamBytes"`     // total heap (DRAM+PSRAM)
	LastActivityMs  uint32 `json:"lastActivityMs"`
	KeepaliveCount  uint32 `json:"keepaliveCount"`
	BoardState      byte   `json:"boardState"`
	BoardStateName  string `json:"boardStateName"`
	InitFlags       byte   `json:"initFlags"`
	ModuleData      []byte `json:"-"`

	// Memory-extension trailer (firmware ≥2026-05-28).
	// `HasMemExtension` reports whether the trailer was present.
	FreeDramBytes   uint32 `json:"freeDramBytes"`
	FreePsramBytes  uint32 `json:"freePsramBytes"`
	HasMemExtension bool   `json:"hasMemExtension"`
}

// Status requests + decodes the periodic STATUS packet.
func (h *Hub) Status() (Status, error) {
	resp, err := h.c.sendForResp(core.CmdStatusReq(), core.Status)
	if err != nil {
		return Status{}, err
	}
	return decodeStatus(resp.Payload)
}

// DiagEntry is one decoded LOG_MESSAGE captured during a DIAG_HISTORY
// snapshot.
type DiagEntry struct {
	Level     byte   `json:"level"`     // 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
	LevelName string `json:"levelName"`
	Millis    uint32 `json:"millis"`
	Message   string `json:"message"`
}

// DiagHistory asks the board to replay its ring buffer of buffered log
// entries (DIAG_HISTORY → flush as TAG_ASYNC LOG_MESSAGE packets, then
// ACK with [count:u16LE]).  Returns the collected entries in
// oldest-first order.
//
// `max` caps how many entries the caller wants — pass 0 for "everything
// in the buffer".
//
// Timeout: the firmware flushes the ENTIRE ring buffer as TAG_ASYNC
// LOG_MESSAGE packets BEFORE the ACK lands.  On an ESP32-S3 with 512
// entries × ~140 bytes that's ~71 KB at 6 Mbps — usually <200 ms on
// wire but USB CDC buffering on slow hosts has been observed to push
// the round-trip past 2 s (the default).  Using a 15 s ceiling here
// covers the worst case (full buffer + slow host) while still letting
// the user notice a genuinely dead device.  Pair this with a non-zero
// `max` to short-circuit the dump.
//
// Filter channel is sized for the worst case (one slot per ring entry).
func (h *Hub) DiagHistory(max byte) ([]DiagEntry, error) {
	const diagHistoryTimeout = 15 * time.Second
	// Drain channel so we can tap LOG_MESSAGE without racing the
	// connection's standard async callback.  Buffer >= ESP32 ring size
	// (512) so a full dump can't block the reader goroutine on backpressure.
	ch := make(chan *protocol.Response, 600)
	h.c.conn.RegisterAsyncFilter(core.LogMessage, ch)
	defer h.c.conn.UnregisterAsyncFilter(core.LogMessage)

	resp, err := h.c.conn.SendExpectACKTimeout(core.CmdDiagHistory(max), diagHistoryTimeout)
	if err != nil {
		return nil, err
	}
	if resp.IsNACK() {
		return nil, errFromNack(resp)
	}

	// The firmware sends every ring entry as TAG_ASYNC LOG_MESSAGE
	// packets BEFORE the ACK, but the reader goroutine may still be
	// dispatching the last few when the ACK arrives.  Drain with a
	// short tail-end window — 250 ms is generous on USB CDC.
	tailWindow := 250 * time.Millisecond
	tailDeadline := time.NewTimer(tailWindow)
	defer tailDeadline.Stop()

	var entries []DiagEntry
	for {
		select {
		case r := <-ch:
			if e := decodeDiagEntry(r.Payload); e != nil {
				entries = append(entries, *e)
			}
			// Reset the tail-window: as long as new entries keep
			// arriving, we keep draining — the timer only fires
			// after `tailWindow` of silence on the filter channel.
			if !tailDeadline.Stop() {
				select {
				case <-tailDeadline.C:
				default:
				}
			}
			tailDeadline.Reset(tailWindow)
		case <-tailDeadline.C:
			return entries, nil
		}
	}
}

func decodeDiagEntry(p []byte) *DiagEntry {
	if len(p) < 5 {
		return nil
	}
	level := p[0]
	return &DiagEntry{
		Level:     level,
		LevelName: core.DiagLevelName(level),
		Millis:    binary.LittleEndian.Uint32(p[1:5]),
		Message:   string(p[5:]),
	}
}

// I2CDevice is one entry in an I2C_SCAN_RESULT.
type I2CDevice struct {
	Address byte `json:"address"`
	Found   bool `json:"found"`
	ID      byte `json:"id"`
}

// I2CScan triggers an I2C bus scan; the firmware returns the result on
// the same correlation tag as I2C_SCAN_RESULT.
func (h *Hub) I2CScan() ([]I2CDevice, error) {
	resp, err := h.c.sendForResp(core.CmdI2cScan(), core.I2cScanRes)
	if err != nil {
		return nil, err
	}
	if len(resp.Payload) < 1 {
		return nil, fmt.Errorf("i2c scan: empty result")
	}
	n := int(resp.Payload[0])
	if len(resp.Payload) < 1+3*n {
		return nil, fmt.Errorf("i2c scan: truncated (need %d entries)", n)
	}
	out := make([]I2CDevice, n)
	for i := 0; i < n; i++ {
		off := 1 + 3*i
		out[i] = I2CDevice{
			Address: resp.Payload[off],
			Found:   resp.Payload[off+1] != 0,
			ID:      resp.Payload[off+2],
		}
	}
	return out, nil
}

// BatteryConfig pushes a chemistry + cell-count override.  Cell count
// of 0 re-arms the firmware's auto-detect.
func (h *Hub) BatteryConfig(chemistry, cellCount byte) error {
	return h.c.sendExpectACK(core.CmdBatteryConfig(chemistry, cellCount))
}

// ─── Decoders ────────────────────────────────────────────────────────

// decodeIdentity unpacks the length-prefixed INIT_READY/IDENTIFY block.
//
//	[nameLen:u8][name][verLen:u8][ver][platLen:u8][plat]
//	[cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]
//	[caps:u32LE][enabledCaps:u32LE]
//
// `enabledCaps` is append-only (Rule 11) — pre-v2.4 firmware doesn't
// emit it, in which case we default it to `caps` so the
// "is feature live" predicate behaves like "is feature compiled in"
// on older boards.
func decodeIdentity(p []byte) (Identity, error) {
	var id Identity
	n, off, err := readLenStr(p, 0, "name")
	if err != nil {
		return id, err
	}
	id.DeviceName = n
	id.GUID = guidSuffixFromDeviceName(n)

	v, off, err := readLenStr(p, off, "version")
	if err != nil {
		return id, err
	}
	id.FirmwareVersion = v

	pl, off, err := readLenStr(p, off, "platform")
	if err != nil {
		return id, err
	}
	id.Platform = pl

	if off+12 > len(p) {
		return id, fmt.Errorf("identity: truncated trailer (need 12 bytes, have %d)", len(p)-off)
	}
	id.CPUFreqMHz = binary.LittleEndian.Uint32(p[off : off+4])
	id.FreeRAMBytes = binary.LittleEndian.Uint32(p[off+4 : off+8])
	id.BuildNumber = binary.LittleEndian.Uint32(p[off+8 : off+12])
	off += 12
	if off+4 <= len(p) {
		id.Capabilities = binary.LittleEndian.Uint32(p[off : off+4])
		off += 4
	}
	// Pre-v2.4 fallback: treat "compiled" as "enabled" when the field
	// is absent.  Newer firmware overrides this with the explicit value.
	id.EnabledCapabilities = id.Capabilities
	if off+4 <= len(p) {
		id.EnabledCapabilities = binary.LittleEndian.Uint32(p[off : off+4])
	}
	return id, nil
}

func decodeStatus(p []byte) (Status, error) {
	if len(p) < core.StatusCoreHeaderSize {
		return Status{}, fmt.Errorf("status: short header (%d < %d)", len(p), core.StatusCoreHeaderSize)
	}
	s := Status{
		Counter:        binary.LittleEndian.Uint32(p[0:4]),
		UptimeMs:       binary.LittleEndian.Uint32(p[4:8]),
		FreeRAMBytes:   binary.LittleEndian.Uint32(p[8:12]),
		LastActivityMs: binary.LittleEndian.Uint32(p[12:16]),
		KeepaliveCount: binary.LittleEndian.Uint32(p[16:20]),
		BoardState:     p[20],
		InitFlags:      p[21],
	}
	s.BoardStateName = core.BoardStateName(s.BoardState)

	// Module data + optional 8-byte mem-extension trailer.  The
	// trailer was added 2026-05-28; older firmware emits only the
	// core header + module data with no trailer.  We trust the
	// trailer is the LAST 8 bytes whenever the payload has at least
	// 8 bytes beyond the core header.  Older firmware without a
	// mem-ext-callback module data is not at risk because we never
	// shipped a 8-byte fixed module-data section that would collide.
	const memExtBytes = 8
	tailStart := core.StatusCoreHeaderSize
	tailEnd := len(p)
	if len(p) >= core.StatusCoreHeaderSize+memExtBytes {
		extOff := len(p) - memExtBytes
		s.FreeDramBytes = binary.LittleEndian.Uint32(p[extOff : extOff+4])
		s.FreePsramBytes = binary.LittleEndian.Uint32(p[extOff+4 : extOff+8])
		s.HasMemExtension = true
		tailEnd = extOff
	}
	s.ModuleData = append([]byte(nil), p[tailStart:tailEnd]...)
	return s, nil
}

func readLenStr(p []byte, off int, label string) (string, int, error) {
	if off >= len(p) {
		return "", off, fmt.Errorf("missing %s length", label)
	}
	n := int(p[off])
	off++
	if off+n > len(p) {
		return "", off, fmt.Errorf("truncated %s (need %d, have %d)", label, n, len(p)-off)
	}
	return string(p[off : off+n]), off + n, nil
}

// guidSuffixFromDeviceName extracts the 4-hex-char GUID suffix from a
// canonical deviceName ("HubFx-3C4D" → "3C4D").  Returns "" if absent.
func guidSuffixFromDeviceName(name string) string {
	for i := len(name) - 1; i >= 0; i-- {
		if name[i] == '-' {
			return name[i+1:]
		}
	}
	return ""
}

// Compile-time refs so vet doesn't drop unused imports in stripped builds.
var (
	_ protocol.PacketType
)
