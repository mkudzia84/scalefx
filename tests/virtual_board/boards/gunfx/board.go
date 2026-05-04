// GunFX virtual board.
//
// Mirrors controllers/gunfx/pico/. Three gun servos, smoke generator
// (heater + fan with overcurrent throttling), muzzle-flash state, RC
// trigger input, battery. STATUS module data is the 69-byte block from
// gunfx_pico.ino:324.

package gunfx

import (
	"sync"
	"time"

	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	pgx "scalefx/protocol/gunfx"
	"scalefx/tests/virtual_board/fauxfs"
	"scalefx/tests/virtual_board/server"
)

// FS returns nil — GunFX firmware advertises no CapFlash / CapConfig
// (the real board has no on-device storage), so the server NACKs
// FILE_* commands. Stub kept here so the server.Board interface is
// satisfied uniformly.
func (b *Board) FS() *fauxfs.FS { return nil }

type Servo struct {
	Position uint16
	Target   uint16
	Min      uint16
	Max      uint16
	Speed    uint16
	Accel    uint16
	Decel    uint16
	Reversed bool
}

type state struct {
	mu sync.Mutex

	firing      bool
	flashOn     bool
	flashFading bool
	heaterOn    bool
	fanOn       bool
	fanSpindown bool

	fanSpeed             uint8
	fanSpindownRem_ms    uint16
	servos               [3]Servo
	rpm                  uint16
	shotsFired           uint32
	heaterOnTime_ms      uint32
	heaterError          uint8
	fanError             uint8
	heaterCappedDuty     uint8
	fanCappedDuty        uint8
	batteryMv            uint16
	batteryCells         uint8
	batteryPct           uint8
	triggerInput_us      uint16
	flashFadeDeadline_ms time.Time
}

func newState() *state {
	s := &state{
		fanSpeed:         0,
		heaterCappedDuty: 255,
		fanCappedDuty:    255,
		batteryMv:        12000, // 3S
		batteryCells:     3,
		batteryPct:       80,
	}
	for i := range s.servos {
		s.servos[i] = Servo{Min: 500, Max: 2500, Position: 1500, Target: 1500,
			Speed: 4000, Accel: 8000, Decel: 8000}
	}
	return s
}

// Board emulates a GunFX controller.
type Board struct {
	name string
	st   *state
}

func New(name string) *Board {
	if name == "" {
		name = "GunFX-Virtual"
	}
	return &Board{name: name, st: newState()}
}

func (b *Board) Name() string         { return b.name }
func (b *Board) Version() string      { return "" }
func (b *Board) Platform() string     { return "" }
func (b *Board) BoardKind() string    { return "gunfx" }
func (b *Board) Capabilities() uint32 { return 0 } // GunFX advertises no flash / config

func (b *Board) Tick(_ server.Sender, now time.Time) {
	b.st.mu.Lock()
	defer b.st.mu.Unlock()
	if b.st.firing {
		// Approximate shots fired ramp at the configured RPM.
		// This keeps Studio's "Shots" counter ticking up when firing.
		b.st.shotsFired++
		if b.st.heaterOn {
			b.st.heaterOnTime_ms += 1000
		}
	}
	if b.st.fanSpindown && b.st.fanSpindownRem_ms > 0 {
		if b.st.fanSpindownRem_ms <= 1000 {
			b.st.fanSpindownRem_ms = 0
			b.st.fanSpindown = false
			b.st.fanOn = false
			b.st.fanSpeed = 0
		} else {
			b.st.fanSpindownRem_ms -= 1000
		}
	}
}

func (b *Board) HandlePacket(s server.Sender, ptype protocol.PacketType, tag byte, payload []byte) bool {
	if ptype == pcore.BatteryConfig {
		b.handleBatteryConfig(s, tag, payload)
		return true
	}
	if ptype < 0x01 || ptype > 0x2F {
		return false
	}
	switch ptype {
	case pgx.TriggerOn:
		b.handleTriggerOn(s, tag)
	case pgx.TriggerOff:
		b.handleTriggerOff(s, tag)
	case pgx.ServoSet:
		b.handleServoSet(s, tag, payload)
	case pgx.ServoSettings:
		b.handleServoSettings(s, tag, payload)
	case pgx.ServoRecoil:
		s.Ack(tag) // simulated single-shot recoil
	case pgx.SmokeHeat:
		b.handleSmokeHeat(s, tag, payload)
	case pgx.SmokeSettings, pgx.SmokeReset, pgx.SmokeCurrentLimit:
		s.Ack(tag)
	default:
		s.Nack(tag, pcore.ErrInvalidCommand)
	}
	return true
}

func (b *Board) handleBatteryConfig(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 2 {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	cells := payload[1]
	b.st.mu.Lock()
	b.st.batteryCells = cells
	b.st.batteryMv = uint16(uint32(cells) * 4000)
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleTriggerOn(s server.Sender, tag byte) {
	b.st.mu.Lock()
	b.st.firing = true
	b.st.flashOn = true
	b.st.rpm = 600
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleTriggerOff(s server.Sender, tag byte) {
	b.st.mu.Lock()
	b.st.firing = false
	b.st.flashOn = false
	b.st.flashFading = true
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleServoSet(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 3 {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	id := payload[0]
	if id < 1 || id > 3 {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	pulse := protocol.ReadU16LE(payload, 1)
	b.st.mu.Lock()
	sv := &b.st.servos[id-1]
	if pulse < sv.Min {
		pulse = sv.Min
	}
	if pulse > sv.Max {
		pulse = sv.Max
	}
	sv.Target = pulse
	sv.Position = pulse
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleServoSettings(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 11 {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	id := payload[0]
	if id < 1 || id > 3 {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	b.st.mu.Lock()
	sv := &b.st.servos[id-1]
	sv.Min = protocol.ReadU16LE(payload, 1)
	sv.Max = protocol.ReadU16LE(payload, 3)
	sv.Speed = protocol.ReadU16LE(payload, 5)
	sv.Accel = protocol.ReadU16LE(payload, 7)
	sv.Decel = protocol.ReadU16LE(payload, 9)
	if len(payload) >= 12 {
		sv.Reversed = payload[11] != 0
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

// SMOKE_HEAT: [enabled:u8][fanSpeed:u8].
func (b *Board) handleSmokeHeat(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	enable := payload[0] != 0
	fanSpeed := uint8(0)
	if len(payload) >= 2 {
		fanSpeed = payload[1]
	}
	b.st.mu.Lock()
	b.st.heaterOn = enable
	b.st.fanOn = enable && fanSpeed > 0
	b.st.fanSpeed = fanSpeed
	if !enable {
		b.st.fanSpindown = true
		b.st.fanSpindownRem_ms = 5000
	} else {
		b.st.fanSpindown = false
		b.st.fanSpindownRem_ms = 0
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

// ─── STATUS module-data builder ───
//
// Mirrors gunfx_pico.ino:324 — up to 69 bytes (28 base + 39 servo cfg + 2 trigger).
func (b *Board) BuildStatusModuleData() []byte {
	const fullLen = 69
	buf := make([]byte, fullLen)

	b.st.mu.Lock()
	defer b.st.mu.Unlock()

	var flags uint8
	if b.st.firing {
		flags |= 0x01
	}
	if b.st.flashOn {
		flags |= 0x02
	}
	if b.st.flashFading {
		flags |= 0x04
	}
	if b.st.heaterOn {
		flags |= 0x08
	}
	if b.st.fanOn {
		flags |= 0x10
	}
	if b.st.fanSpindown {
		flags |= 0x20
	}
	buf[0] = flags
	buf[1] = b.st.fanSpeed
	copy(buf[2:], protocol.U16LE(b.st.fanSpindownRem_ms))
	copy(buf[4:], protocol.U16LE(b.st.servos[0].Position))
	copy(buf[6:], protocol.U16LE(b.st.servos[1].Position))
	copy(buf[8:], protocol.U16LE(b.st.servos[2].Position))
	copy(buf[10:], protocol.U16LE(b.st.rpm))
	copy(buf[12:], protocol.U32LE(b.st.shotsFired))
	copy(buf[16:], protocol.U32LE(b.st.heaterOnTime_ms))
	buf[20] = b.st.heaterError
	buf[21] = b.st.fanError
	buf[22] = b.st.heaterCappedDuty
	buf[23] = b.st.fanCappedDuty
	copy(buf[24:], protocol.U16LE(b.st.batteryMv))
	buf[26] = b.st.batteryCells
	buf[27] = b.st.batteryPct

	// Per-servo configs (3 × 13 = 39 bytes at offset 28).
	off := 28
	for i := 0; i < 3; i++ {
		sv := b.st.servos[i]
		copy(buf[off+0:], protocol.U16LE(sv.Min))
		copy(buf[off+2:], protocol.U16LE(sv.Max))
		copy(buf[off+4:], protocol.U16LE(sv.Target))
		copy(buf[off+6:], protocol.U16LE(sv.Speed))
		copy(buf[off+8:], protocol.U16LE(sv.Accel))
		copy(buf[off+10:], protocol.U16LE(sv.Decel))
		if sv.Reversed {
			buf[off+12] = 1
		}
		off += 13
	}
	copy(buf[67:], protocol.U16LE(b.st.triggerInput_us))
	return buf
}
