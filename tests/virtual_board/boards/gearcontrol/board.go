// GearControl virtual board.
//
// Mirrors the on-wire surface of controllers/gearcontrol/pico/. Three
// gears (each with two door servos), a yaw servo, gear input PWM, LED
// status flags, battery, and per-gear config. STATUS module data is the
// 149-byte block emitted by gearcontrol_pico.ino:942.

package gearcontrol

import (
	"sync"
	"time"

	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	pgc "scalefx/protocol/gearcontrol"
	"scalefx/tests/virtual_board/fauxfs"
	"scalefx/tests/virtual_board/server"
)

const (
	// Gear states (mirror GearState enum in firmware).
	gearRetracted  = 0
	gearDeploying  = 1
	gearDeployed   = 2
	gearRetracting = 3
	gearError      = 4

	// Door states (DoorSequencer).
	doorClosed  = 0
	doorOpening = 1
	doorOpen    = 2
	doorClosing = 3

	gearTransitionMs = 2000
)

// Servo holds runtime servo state — used for both door servos (6 of
// them, two per gear) and the yaw servo.
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

// Gear is one of three landing-gear assemblies.
type Gear struct {
	State           uint8
	StateDeadline   time.Time
	Doors           [2]Servo
	DoorState       uint8
	MotorCurrent_mA uint16
	StallLimit_mA   uint16
	ErrorReason     uint8
	ConfigFlags     uint8
	DoorPreDeploy   uint8
	DoorPostDeploy  uint8
	Enabled         bool
}

type state struct {
	mu sync.Mutex

	gears [3]Gear
	yaw   Servo

	gearInputPulse_us     uint16
	gearInputThreshold_us uint16
	gearInputEnabled      bool
	gearInputCmdValid     bool
	gearInputCmdLast      bool

	ledFlags uint8

	batteryMv         uint16
	batteryCells      uint8
	autoDeployOnLow   bool
	lowVoltageTrigged bool
	shuntMilliohms    uint16
}

func newState() *state {
	s := &state{
		gearInputThreshold_us: 1500,
		batteryMv:             8200,
		batteryCells:          2,
		shuntMilliohms:        100,
	}
	defServo := Servo{Min: 500, Max: 2500, Position: 1500, Target: 1500,
		Speed: 4000, Accel: 8000, Decel: 8000}
	for i := range s.gears {
		s.gears[i].Enabled = true
		s.gears[i].StallLimit_mA = 800
		s.gears[i].Doors[0] = defServo
		s.gears[i].Doors[1] = defServo
	}
	s.yaw = defServo
	return s
}

// Board emulates a GearControl controller.
type Board struct {
	name string
	st   *state
	fs   *fauxfs.FS
}

func New(name string) *Board {
	if name == "" {
		name = "GearControl-Virtual"
	}
	fs := fauxfs.New()
	// Seed a minimal /gearcontrol.yaml so Studio's File Manager and
	// config-loader see realistic content on a fresh virtual board.
	_ = fs.Seed(1 /*flash*/, "/gearcontrol.yaml", []byte(demoYaml))
	return &Board{name: name, st: newState(), fs: fs}
}

// FS exposes the per-board faux filesystem to the server's FILE_*
// handler. Implements server.Board.FS().
func (b *Board) FS() *fauxfs.FS { return b.fs }

// demoYaml is a placeholder /gearcontrol.yaml — not driven by an
// in-process program runtime (we don't have one for GearControl yet),
// but enough for Studio's config-loader to round-trip text.
const demoYaml = `# GearControl virtual board — placeholder config
master:
  battery_auto_deploy: false

gears:
  - id: 1
    enabled: true
  - id: 2
    enabled: true
  - id: 3
    enabled: true
`

func (b *Board) Name() string         { return b.name }
func (b *Board) Version() string      { return "" }
func (b *Board) Platform() string     { return "" }
func (b *Board) BoardKind() string    { return "gearcontrol" }
func (b *Board) Capabilities() uint32 { return pcore.CapFlash | pcore.CapConfig }

func (b *Board) Tick(_ server.Sender, now time.Time) {
	b.st.mu.Lock()
	defer b.st.mu.Unlock()
	for i := range b.st.gears {
		g := &b.st.gears[i]
		if (g.State == gearDeploying || g.State == gearRetracting) &&
			now.After(g.StateDeadline) {
			if g.State == gearDeploying {
				g.State = gearDeployed
				g.DoorState = doorOpen
				g.Doors[0].Position = g.Doors[0].Max
				g.Doors[1].Position = g.Doors[1].Max
			} else {
				g.State = gearRetracted
				g.DoorState = doorClosed
				g.Doors[0].Position = g.Doors[0].Min
				g.Doors[1].Position = g.Doors[1].Min
			}
		}
	}
}

func (b *Board) HandlePacket(s server.Sender, ptype protocol.PacketType, tag byte, payload []byte) bool {
	if ptype == pcore.BatteryConfig {
		b.handleBatteryConfig(s, tag, payload)
		return true
	}
	if ptype < 0x60 || ptype > 0x7F {
		return false
	}
	switch ptype {
	case pgc.GearDeploy:
		b.handleGearDeploy(s, tag, payload)
	case pgc.GearRetract:
		b.handleGearRetract(s, tag, payload)
	case pgc.GearStop:
		b.handleGearStop(s, tag, payload)
	case pgc.GearAll:
		b.handleGearAll(s, tag, payload)
	case pgc.ServoSet:
		b.handleServoSet(s, tag, payload)
	case pgc.ServoSettings:
		b.handleServoSettings(s, tag, payload)
	case pgc.GearReset:
		b.handleGearReset(s, tag, payload)
	case pgc.GearEnable:
		b.handleGearEnable(s, tag, payload)
	case pgc.BatteryAutoDeploy:
		b.handleBatteryAutoDeploy(s, tag, payload)
	case pgc.GearConfig, pgc.YawConfig, pgc.YawInput,
		pgc.GearCalibrate, pgc.GearCalibStatus, pgc.GearCalibCancel,
		pgc.DoorMode, pgc.GearSeqStatus, pgc.GearDoorStatus:
		// Stubbed — ACK so Studio panels don't surface NACKs while we
		// don't yet model calibration / door-mode / typed responses.
		s.Ack(tag)
	default:
		s.Nack(tag, pcore.ErrInvalidCommand)
	}
	return true
}

// ─── Handlers ───

func (b *Board) handleBatteryConfig(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 2 {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	b.st.mu.Lock()
	b.st.batteryCells = payload[1]
	b.st.batteryMv = uint16(uint32(payload[1]) * 4000)
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) gearForCmd(payload []byte) (uint8, bool) {
	if len(payload) < 1 {
		return 0, false
	}
	id := payload[0]
	if id == 0 || id > 3 {
		return 0, false
	}
	return id, true
}

func (b *Board) startTransition(g *Gear, now time.Time, deploying bool) {
	g.StateDeadline = now.Add(gearTransitionMs * time.Millisecond)
	if deploying {
		g.State = gearDeploying
		g.DoorState = doorOpening
	} else {
		g.State = gearRetracting
		g.DoorState = doorClosing
	}
}

func (b *Board) handleGearDeploy(s server.Sender, tag byte, payload []byte) {
	id, ok := b.gearForCmd(payload)
	if !ok {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	now := time.Now()
	b.st.mu.Lock()
	b.startTransition(&b.st.gears[id-1], now, true)
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleGearRetract(s server.Sender, tag byte, payload []byte) {
	id, ok := b.gearForCmd(payload)
	if !ok {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	now := time.Now()
	b.st.mu.Lock()
	b.startTransition(&b.st.gears[id-1], now, false)
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleGearStop(s server.Sender, tag byte, payload []byte) {
	id, ok := b.gearForCmd(payload)
	if !ok {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	b.st.mu.Lock()
	g := &b.st.gears[id-1]
	if g.State == gearDeploying {
		g.State = gearRetracted // half-deployed → bias to retracted to match firmware semantics
	} else if g.State == gearRetracting {
		g.State = gearDeployed
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

// GEAR_ALL: [op:u8] (0=deploy, 1=retract).
func (b *Board) handleGearAll(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	deploying := payload[0] == 0
	now := time.Now()
	b.st.mu.Lock()
	for i := range b.st.gears {
		b.startTransition(&b.st.gears[i], now, deploying)
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

// SERVO_SET: [id:u8][pulse:i16LE]. id 1..6 = doors (gear (id-1)/2 +
// door (id-1)%2), id 7 = yaw.
func (b *Board) handleServoSet(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 3 {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	id := payload[0]
	pulse := protocol.ReadU16LE(payload, 1)
	b.st.mu.Lock()
	defer b.st.mu.Unlock()
	srv := b.servoByID(id)
	if srv == nil {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	if pulse < srv.Min {
		pulse = srv.Min
	}
	if pulse > srv.Max {
		pulse = srv.Max
	}
	srv.Target = pulse
	srv.Position = pulse
	s.Ack(tag)
}

func (b *Board) handleServoSettings(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 11 {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	id := payload[0]
	b.st.mu.Lock()
	defer b.st.mu.Unlock()
	srv := b.servoByID(id)
	if srv == nil {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	srv.Min = protocol.ReadU16LE(payload, 1)
	srv.Max = protocol.ReadU16LE(payload, 3)
	srv.Speed = protocol.ReadU16LE(payload, 5)
	srv.Accel = protocol.ReadU16LE(payload, 7)
	srv.Decel = protocol.ReadU16LE(payload, 9)
	if len(payload) >= 12 {
		srv.Reversed = payload[11] != 0
	}
	s.Ack(tag)
}

// servoByID returns a pointer to the underlying Servo for 1-based servo
// ID 1..6 (doors) or 7 (yaw). Caller must hold b.st.mu.
func (b *Board) servoByID(id uint8) *Servo {
	if id >= 1 && id <= 6 {
		gear := (id - 1) / 2
		door := (id - 1) % 2
		return &b.st.gears[gear].Doors[door]
	}
	if id == 7 {
		return &b.st.yaw
	}
	return nil
}

func (b *Board) handleGearReset(s server.Sender, tag byte, payload []byte) {
	id, ok := b.gearForCmd(payload)
	if !ok {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	b.st.mu.Lock()
	g := &b.st.gears[id-1]
	g.State = gearRetracted
	g.DoorState = doorClosed
	g.ErrorReason = 0
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleGearEnable(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 2 {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	id, enabled := payload[0], payload[1] != 0
	if id == 0 {
		b.st.mu.Lock()
		for i := range b.st.gears {
			b.st.gears[i].Enabled = enabled
		}
		b.st.mu.Unlock()
		s.Ack(tag)
		return
	}
	if id > 3 {
		s.Nack(tag, pcore.ErrInvalidId)
		return
	}
	b.st.mu.Lock()
	b.st.gears[id-1].Enabled = enabled
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleBatteryAutoDeploy(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	b.st.mu.Lock()
	b.st.autoDeployOnLow = payload[0] != 0
	b.st.mu.Unlock()
	s.Ack(tag)
}

// ─── STATUS module-data builder ───
//
// Mirrors gearcontrol_pico.ino:942 — 149-byte block.
func (b *Board) BuildStatusModuleData() []byte {
	const fullLen = 149
	buf := make([]byte, fullLen)

	b.st.mu.Lock()
	defer b.st.mu.Unlock()

	for i := 0; i < 3; i++ {
		off := i * 11
		g := &b.st.gears[i]
		buf[off] = g.State
		copy(buf[off+1:], protocol.U16LE(g.MotorCurrent_mA))
		copy(buf[off+3:], protocol.U16LE(g.Doors[0].Position))
		copy(buf[off+5:], protocol.U16LE(g.Doors[1].Position))
		copy(buf[off+7:], protocol.U16LE(g.StallLimit_mA))
		// shunt voltage (10µV units) — virtual board reports 0
		copy(buf[off+9:], protocol.U16LE(0))
	}

	copy(buf[33:], protocol.U16LE(b.st.yaw.Position))
	buf[35] = b.st.ledFlags
	copy(buf[36:], protocol.U16LE(b.st.batteryMv))

	var battFlags byte
	if b.st.autoDeployOnLow {
		battFlags |= 0x01
	}
	if b.st.lowVoltageTrigged {
		battFlags |= 0x02
	}
	buf[38] = battFlags

	for i := 0; i < 3; i++ {
		buf[39+i] = b.st.gears[i].ErrorReason
	}
	copy(buf[42:], protocol.U16LE(b.st.shuntMilliohms))

	for i := 0; i < 3; i++ {
		g := &b.st.gears[i]
		buf[44+i] = (g.DoorPreDeploy & 0x0F) | ((g.DoorPostDeploy & 0x0F) << 4)
	}
	for i := 0; i < 3; i++ {
		g := &b.st.gears[i]
		flags := g.ConfigFlags
		if g.Enabled {
			flags |= 0x80
		}
		buf[47+i] = flags
	}
	for i := 0; i < 3; i++ {
		buf[50+i] = b.st.gears[i].DoorState
	}

	copy(buf[53:], protocol.U16LE(b.st.gearInputPulse_us))
	copy(buf[55:], protocol.U16LE(b.st.gearInputThreshold_us))
	var inputFlags byte
	if b.st.gearInputEnabled {
		inputFlags |= 0x01
	}
	if b.st.gearInputCmdValid && b.st.gearInputCmdLast {
		inputFlags |= 0x02
	}
	buf[57] = inputFlags

	// Per-servo configs — 7 servos × 13 bytes (6 doors + yaw).
	for i := uint8(0); i < 7; i++ {
		off := 58 + int(i)*13
		srv := b.servoByID(i + 1)
		if srv == nil {
			continue
		}
		copy(buf[off+0:], protocol.U16LE(srv.Min))
		copy(buf[off+2:], protocol.U16LE(srv.Max))
		copy(buf[off+4:], protocol.U16LE(srv.Target))
		copy(buf[off+6:], protocol.U16LE(srv.Speed))
		copy(buf[off+8:], protocol.U16LE(srv.Accel))
		copy(buf[off+10:], protocol.U16LE(srv.Decel))
		if srv.Reversed {
			buf[off+12] = 1
		}
	}
	return buf
}
