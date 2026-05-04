// HubFX virtual board.
//
// Mirrors controllers/hubfx/esp32s3/. Master device — slave registry,
// audio mixer (stub), file storage (stub), I2C device map, INA226 rail
// voltages. STATUS module data is the 19-byte block emitted by
// hubfx_esp32s3.ino:1045.

package hubfx

import (
	"sync"
	"sync/atomic"
	"time"

	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	phub "scalefx/protocol/hubfx"
	"scalefx/tests/virtual_board/fauxfs"
	"scalefx/tests/virtual_board/server"
)

const (
	flagCore1Ready    = 1 << 0
	flagAudioInit     = 1 << 1
	flagFlashInit     = 1 << 2
	flagUsbReady      = 1 << 3
	flagSdReady       = 1 << 4

	slaveGunFX        = 1 << 0
	slaveLightFX      = 1 << 1
	slaveGearControl  = 1 << 2
)

type state struct {
	mu sync.Mutex

	flags        uint8
	slaveMask    uint8
	loop1Counter atomic.Uint32
	i2cMask      uint8         // bit0=AW9523B, bit1-6=INA226[0..5]
	railMv       [6]uint16     // INA226 bus voltage (mV)
}

func newState() *state {
	return &state{
		flags:     flagCore1Ready | flagAudioInit | flagFlashInit | flagSdReady,
		slaveMask: 0, // none attached by default
		i2cMask:   0x01 | 0x02,
		railMv:    [6]uint16{8200, 0, 0, 0, 0, 0},
	}
}

// Board emulates a HubFX controller.
type Board struct {
	name string
	st   *state
	fs   *fauxfs.FS
}

func New(name string) *Board {
	if name == "" {
		name = "HubFX-Virtual"
	}
	fs := fauxfs.New()
	// Hub firmware exposes both flash (config) and SD (audio assets).
	// Seed both so Studio's File Manager has folders to navigate. The
	// real hub creates /system, /audio, /assets at first boot.
	_ = fs.Seed(1 /*flash*/, "/hubfx.yaml", []byte(demoHubYaml))
	_ = fs.Seed(0 /*sd*/, "/audio/.keep", []byte("placeholder\n"))
	_ = fs.Seed(0 /*sd*/, "/assets/.keep", []byte("placeholder\n"))
	return &Board{name: name, st: newState(), fs: fs}
}

// FS exposes the per-board faux filesystem to the server's FILE_*
// handler. Implements server.Board.FS().
func (b *Board) FS() *fauxfs.FS { return b.fs }

const demoHubYaml = `# HubFX virtual board — placeholder config
audio:
  master_volume: 80
  mixer_channels: 4

slaves:
  expected:
    - lightfx
    - gearcontrol
`

func (b *Board) Name() string      { return b.name }
func (b *Board) Version() string   { return "" }
func (b *Board) Platform() string  { return "" }
func (b *Board) BoardKind() string { return "hubfx" }
func (b *Board) Capabilities() uint32 {
	return pcore.CapFlash | pcore.CapSd | pcore.CapAudio |
		pcore.CapUsbHost | pcore.CapEngine | pcore.CapConfig | pcore.CapSlaveBus
}

func (b *Board) Tick(_ server.Sender, now time.Time) {
	b.st.loop1Counter.Add(1)
}

func (b *Board) HandlePacket(s server.Sender, ptype protocol.PacketType, tag byte, payload []byte) bool {
	if ptype == pcore.BatteryConfig {
		s.Ack(tag)
		return true
	}
	if ptype < 0x80 || ptype > 0xAF {
		return false
	}
	switch ptype {
	case phub.SlaveList:
		b.handleSlaveList(s, tag)
	case phub.SlaveInit:
		s.Ack(tag)
	case phub.SlaveStatus:
		// Stub — would normally proxy a STATUS request to the slave.
		s.Ack(tag)
	case phub.AudioPlay, phub.AudioStop, phub.AudioVolume, phub.AudioFade,
		phub.AudioQueue, phub.AudioQueueClear:
		s.Ack(tag)
	case phub.AudioStatusReq:
		// Send back an empty AUDIO_STATUS_RESP so Studio's audio panel
		// at least recognises the response shape.
		s.Send(phub.AudioStatusResp, tag, []byte{0, 0, 0, 0})
	case phub.EngineStart, phub.EngineStop:
		s.Ack(tag)
	case phub.EngineStatusReq:
		s.Send(phub.EngineStatusResp, tag, []byte{0})
	case phub.ConfigReload, phub.ConfigSave:
		s.Ack(tag)
	case phub.ConfigStatus:
		s.Send(phub.ConfigStatusResp, tag, []byte{1}) // pretend "loaded"
	case phub.SdInit:
		s.Ack(tag)
	case phub.SdStatusReq:
		s.Send(phub.SdStatusResp, tag, []byte{1, 0}) // "ready"
	default:
		s.Ack(tag)
	}
	return true
}

func (b *Board) handleSlaveList(s server.Sender, tag byte) {
	b.st.mu.Lock()
	mask := b.st.slaveMask
	b.st.mu.Unlock()
	// SLAVE_LIST_RESP: [count:u8][slaveType:u8 × N] — empty by default
	// since the virtual hub has no real slaves attached yet. Studio
	// shows "no slaves attached" rather than a NACK.
	resp := []byte{0}
	_ = mask
	s.Send(phub.SlaveListResp, tag, resp)
}

// ─── STATUS module-data builder ───
//
// Mirrors hubfx_esp32s3.ino:1045 — 19-byte block.
func (b *Board) BuildStatusModuleData() []byte {
	const fullLen = 19
	buf := make([]byte, fullLen)

	b.st.mu.Lock()
	defer b.st.mu.Unlock()

	buf[0] = b.st.flags
	buf[1] = b.st.slaveMask
	copy(buf[2:], protocol.U32LE(b.st.loop1Counter.Load()))
	buf[6] = b.st.i2cMask
	for i := 0; i < 6; i++ {
		copy(buf[7+i*2:], protocol.U16LE(b.st.railMv[i]))
	}
	return buf
}
