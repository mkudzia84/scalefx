// Virtual board — Board interface.
//
// Each emulated board (LightFX, GearControl, GunFX, HubFX) implements
// this minimum surface; the Server in server.go owns the TCP
// connection, framing, and core-packet routing (INIT/IDENTIFY/STATUS/
// KEEPALIVE) and delegates everything else to the Board.

package server

import (
	"time"

	"scalefx/protocol"
	"scalefx/tests/virtual_board/fauxfs"
)

// Sender is the slice of Server callable from a Board's handlers — keeps
// boards from depending on the full Server type.
type Sender interface {
	Send(ptype protocol.PacketType, tag byte, payload []byte)
	Ack(tag byte)
	Nack(tag byte, code protocol.ErrorCode)
}

// Board is implemented by each emulated board.
type Board interface {
	// Name returns the device name advertised in INIT_READY. Must start
	// with one of the prefixes recognised by `core.DetectControllerType`
	// ("LightFX", "GearControl", "GunFX", "HubFX") so the CLI/GUI tag
	// the connection with the right controller type. By convention the
	// virtual variants append "-Virtual": "LightFX-Virtual", etc.
	Name() string

	// Version returns the firmware version string ("0.99.0-virt").
	Version() string

	// Platform returns the platform string ("VIRT-x86" by convention).
	Platform() string

	// Capabilities returns the CoreCapability bitmask the board
	// advertises (mirrors `server.core().addCapability(...)`).
	Capabilities() uint32

	// BoardKind returns the canonical short identifier ("lightfx",
	// "gearcontrol", "gunfx", "hubfx") used by discovery + flag dispatch.
	BoardKind() string

	// BuildStatusModuleData fills in the per-board module-data block
	// appended after the 22-byte STATUS core header. Returning an empty
	// slice is fine — STATUS still carries the core header.
	BuildStatusModuleData() []byte

	// Tick advances the simulation by one step. Called from the
	// server's 50 ms simulation tick; `s` lets the board emit
	// unsolicited packets when a state machine transitions (e.g.
	// LANDING_LIGHT_STATUS when the bound servo settles).
	Tick(s Sender, now time.Time)

	// HandlePacket dispatches a board-specific packet. Return true if
	// the packet was recognised (even if it failed validation — the
	// board is responsible for ACK/NACK in that case). Return false to
	// let the Server NACK it with InvalidCommand.
	HandlePacket(s Sender, ptype protocol.PacketType, tag byte, payload []byte) bool

	// FS returns the board's faux filesystem (or nil if the board does
	// not advertise CapFlash / CapSd in its capabilities). The server
	// uses this to handle FILE_* / STREAM_* generically — boards that
	// support storage just return their fauxfs.FS instance.
	FS() *fauxfs.FS
}
