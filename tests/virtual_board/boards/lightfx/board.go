// LightFX virtual board — packet handlers + STATUS module-data builder.

package lightfx

import (
	"time"

	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	plfx "scalefx/protocol/lightfx"
	"scalefx/tests/virtual_board/fauxfs"
	"scalefx/tests/virtual_board/server"
	"scalefx/tests/virtual_board/shared"
)

const landingTransitionMs = 1500

// Board emulates a LightFX controller.
type Board struct {
	name string
	st   *state
	now  func() time.Time
	fs   *fauxfs.FS
}

// New constructs a LightFX board with the given device name. Pass "" to
// use the default ("LightFX-Virtual"). The board comes with its own
// fauxfs (flash + sd targets, both initially empty); LoadDemo seeds
// /lightfx.yaml on flash so Studio's File Manager and config loader
// have something to read.
func New(name string) *Board {
	if name == "" {
		name = "LightFX-Virtual"
	}
	return &Board{name: name, st: newState(), now: time.Now, fs: fauxfs.New()}
}

// FS exposes the per-board faux filesystem to the server's FILE_*
// handler. Implements server.Board.FS().
func (b *Board) FS() *fauxfs.FS { return b.fs }

// SetClock overrides the time source — used by tests for deterministic
// LED-event timing checks. Must be set before any sequence is started.
func (b *Board) SetClock(now func() time.Time) {
	if now == nil {
		now = time.Now
	}
	b.now = now
}

// Snapshot copies a small subset of state for tests to inspect without
// taking locks. Field set is intentionally narrow — extend as tests
// require.
type Snapshot struct {
	Channels         [8]uint8 // brightness 0..100 per channel
	SeqPlaying       [8]bool
	Servos           [3]ServoState
	Landing          [3]LandingState
	MasterBrightness uint8
	ActiveProgram    int8 // -1 if none
}

// SimulateRxPulse injects an RC PWM pulse width on SRV1 (the LightFX
// dual-role pin GP8). When `us` is non-zero:
//   - STATUS bytes 9-10 (servo0_us) report this value instead of the
//     servo's commanded position — matches firmware behaviour in
//     IDLE / STANDALONE / DIRECT board states.
//   - If light-program InputBands are configured, the next tick
//     auto-selects the matching program (500 ms debounce).
//
// Pass 0 to clear (no-signal). Tests use this to drive the auto-switch
// path without a physical RC receiver.
func (b *Board) SimulateRxPulse(us uint16) {
	b.st.mu.Lock()
	b.st.simulatedRxPulse_us = us
	b.st.mu.Unlock()
}

// SimulateBatteryVoltage drops the simulated pack voltage to `mv` and
// triggers the auto-cutoff side effect if the per-cell voltage falls
// below 3.3 V (the firmware's default LiPo low threshold) AND
// auto-cutoff is currently armed. Used by tests + diag tooling so a
// developer can verify the cutoff path without a real ADC.
//
// Side effects mirror controllers/lightfx/pico/src/lightfx_pico.ino:560-567:
//   - sets lowVoltageTriggered = true
//   - disables every LED channel (.Enabled = false), zeroing brightness
//
// Channels stay disabled until LED_RESET / LED_ENABLE re-arms them.
// To clear the lowVoltageTriggered flag without re-arming channels,
// disable auto-cutoff via BATTERY_AUTO_CUTOFF(0) — same as firmware.
func (b *Board) SimulateBatteryVoltage(mv uint16) {
	b.st.mu.Lock()
	b.st.batteryMv = mv
	cells := b.st.batteryCells
	if cells == 0 {
		cells = 1
	}
	perCell := uint32(mv) / uint32(cells)
	threshold := uint32(3300) // 3.3 V/cell — matches firmware LiPo default
	if perCell <= threshold && b.st.batteryAutoCutoff {
		b.st.batteryLowFired = true
		for i := range b.st.channels {
			b.st.channels[i].Enabled = false
			b.st.channels[i].SeqPlaying = false
			b.st.channels[i].Brightness = 0
		}
	}
	// Approximate percentage off a simple linear curve from 3.3 → 4.2 V/cell.
	if perCell <= threshold {
		b.st.batteryPct = 0
	} else if perCell >= 4200 {
		b.st.batteryPct = 100
	} else {
		b.st.batteryPct = uint8((perCell - 3300) * 100 / 900)
	}
	b.st.mu.Unlock()
}

// Snapshot returns a point-in-time copy of the relevant state. Cheap
// enough to call inside a test loop after each tick.
func (b *Board) Snapshot() Snapshot {
	b.st.mu.Lock()
	defer b.st.mu.Unlock()
	s := Snapshot{
		MasterBrightness: b.st.masterBrightness,
		Servos:           b.st.servos,
		Landing:          b.st.landing,
		ActiveProgram:    b.st.activeProgram,
	}
	for i, c := range b.st.channels {
		s.Channels[i] = c.Brightness
		s.SeqPlaying[i] = c.SeqPlaying
	}
	return s
}

func (b *Board) Name() string         { return b.name }
func (b *Board) Version() string      { return "" } // server defaults
func (b *Board) Platform() string     { return "" }
func (b *Board) BoardKind() string    { return "lightfx" }
func (b *Board) Capabilities() uint32 { return pcore.CapFlash | pcore.CapConfig }

// Tick advances the simulation. The sender lets the state machine
// fire async LANDING_LIGHT_STATUS packets when a slot transitions
// from DEPLOYING → DEPLOYED or RETRACTING → RETRACTED (the firmware
// fires the same packet from the landing-light progress callback in
// landing_light.h).
func (b *Board) Tick(s server.Sender, now time.Time) {
	// Input-band auto program-select runs BEFORE state.tick so the
	// rest of the tick sees the freshly-selected program's events.
	// Mirrors LightProgramManager::update() in firmware: when the
	// simulated RX pulse falls in a new band and the debounce window
	// has elapsed, switch programs.
	if idx, ok := b.pickProgramFromInput(now); ok {
		b.SelectProgram(idx)
	}
	b.st.tick(s, now)
}

// pickProgramFromInput returns the program index selected by the
// current RX pulse + input bands, applying the firmware's debounce.
// Returns (idx, false) when there's nothing to do (no pulse / no bands
// / no band match / debounce window not elapsed / already on the
// matching program).
func (b *Board) pickProgramFromInput(now time.Time) (uint8, bool) {
	b.st.mu.Lock()
	defer b.st.mu.Unlock()
	pulse := b.st.simulatedRxPulse_us
	if pulse == 0 || len(b.st.inputBands) == 0 {
		return 0, false
	}
	if now.Sub(b.st.lastBandSwitch).Milliseconds() < switchDebounceMs {
		return 0, false
	}
	for _, band := range b.st.inputBands {
		if pulse >= band.MinUs && pulse <= band.MaxUs {
			if int8(band.Program) == b.st.activeProgram {
				return 0, false
			}
			if int(band.Program) >= len(b.st.programs) {
				return 0, false
			}
			b.st.lastBandSwitch = now
			return band.Program, true
		}
	}
	return 0, false
}

// ─── Program runtime (in-process API used by tests + the wire path) ───

// LoadProgramConfig populates the board's program model. Mirrors the
// firmware's LightProgramManager.loadConfig: applies master brightness,
// stores landing-group bindings (so DEPLOY/RETRACT mask the right LEDs),
// and stores the program list (no program selected yet).
func (b *Board) LoadProgramConfig(cfg LightProgramConfig) {
	b.st.mu.Lock()
	defer b.st.mu.Unlock()

	if cfg.MasterBrightness > 0 {
		b.st.masterBrightness = cfg.MasterBrightness
	}
	b.st.landingGroups = append(b.st.landingGroups[:0], cfg.LandingGroups...)
	b.st.programs = append(b.st.programs[:0], cfg.Programs...)
	b.st.inputBands = append(b.st.inputBands[:0], cfg.InputBands...)
	b.st.activeProgram = -1

	// Bind each group to its slot (1-based slot id = index + 1).
	for i, g := range b.st.landingGroups {
		if i >= len(b.st.landing) {
			break
		}
		l := &b.st.landing[i]
		l.Slot = uint8(i + 1)
		l.ServoID = g.ServoID
		l.ChannelMask = g.ChannelMask
		l.Brightness = g.Brightness
	}
}

// SelectProgram activates program `idx` — clears every channel,
// re-loads the program's events, then applies group policies (deploy
// for ON, retract for OFF). Mirrors LightProgramManager::selectProgram
// (controllers/lib/sfx_peripherals/led/light_program_manager.cpp).
//
// Returns false if the program index is out of range.
func (b *Board) SelectProgram(idx uint8) bool {
	now := b.now()

	b.st.mu.Lock()
	if int(idx) >= len(b.st.programs) {
		b.st.mu.Unlock()
		return false
	}
	prog := b.st.programs[idx]

	// 1. Stop all sequences + zero all brightness (clean slate).
	for i := range b.st.channels {
		b.st.channels[i].SeqPlaying = false
		b.st.channels[i].SeqEvents = nil
		b.st.channels[i].SeqIndex = 0
		b.st.channels[i].SeqLoops = 0
		b.st.channels[i].Brightness = 0
	}

	// 2. Load events for each channel that isn't group-controlled.
	for _, ch := range prog.Channels {
		if ch.Channel == 0 || ch.Channel > 8 {
			continue
		}
		if ch.GroupIndex != 0xFF {
			continue // group-controlled — handled in step 3
		}
		if len(ch.Events) == 0 {
			continue
		}
		c := &b.st.channels[ch.Channel-1]
		c.SeqEvents = append([]shared.SeqEvent(nil), ch.Events...)
		c.SeqPlaying = true
		c.SeqIndex = 0
		c.SeqLoops = 0
		c.SeqStart = now
	}

	// Snapshot the data we need for step 3 outside the lock so the
	// landing-status emitter doesn't deadlock against status broadcasts.
	policies := append([]uint8(nil), prog.GroupPolicies...)
	groupCount := len(b.st.landingGroups)
	b.st.activeProgram = int8(idx)
	b.st.mu.Unlock()

	// 3. Apply group policies — deploy on ON, retract on OFF, do nothing
	//    for GEAR (external gear signal drives it). No Sender here:
	//    deploys/retracts go through the in-process state path.
	for i := 0; i < groupCount && i < len(b.st.landing); i++ {
		policy := uint8(GroupPolicyOff)
		if i < len(policies) {
			policy = policies[i]
		}
		switch policy {
		case GroupPolicyOn:
			b.localDeploy(uint8(i+1), now)
		case GroupPolicyGear:
			// no-op — gear input drives this slot
		default:
			b.localRetract(uint8(i+1), now)
		}
	}
	return true
}

// ResetProgram stops every sequence, retracts every bound landing group,
// and clears the active-program slot. Mirrors LightProgramManager::resetProgram.
func (b *Board) ResetProgram() {
	now := b.now()
	b.st.mu.Lock()
	for i := range b.st.channels {
		b.st.channels[i].SeqPlaying = false
		b.st.channels[i].Brightness = 0
	}
	groupCount := len(b.st.landingGroups)
	b.st.activeProgram = -1
	b.st.mu.Unlock()

	for i := 0; i < groupCount && i < len(b.st.landing); i++ {
		b.localRetract(uint8(i+1), now)
	}
}

// localDeploy / localRetract are the in-process equivalents of the wire
// LANDING_LIGHT_DEPLOY / RETRACT handlers — same state transitions, no
// async LANDING_LIGHT_STATUS emission (no Sender available).
func (b *Board) localDeploy(slot uint8, now time.Time) {
	if slot < 1 || slot > 3 {
		return
	}
	b.st.mu.Lock()
	defer b.st.mu.Unlock()
	l := &b.st.landing[slot-1]
	if l.Phase == phaseDeployed {
		return
	}
	l.Phase = phaseDeploying
	l.PhaseDeadline = now
	if l.ServoID >= 1 && l.ServoID <= 3 {
		sv := &b.st.servos[l.ServoID-1]
		if sv.Reversed {
			sv.TargetUs = sv.MinUs
		} else {
			sv.TargetUs = sv.MaxUs
		}
	}
	for ch := 0; ch < 8; ch++ {
		if l.ChannelMask&(1<<ch) != 0 {
			b.st.channels[ch].SeqPlaying = false
			b.st.channels[ch].Brightness = l.Brightness
		}
	}
}

func (b *Board) localRetract(slot uint8, now time.Time) {
	if slot < 1 || slot > 3 {
		return
	}
	b.st.mu.Lock()
	defer b.st.mu.Unlock()
	l := &b.st.landing[slot-1]
	if l.Phase == phaseRetracted {
		return
	}
	l.Phase = phaseRetracting
	l.PhaseDeadline = now
	if l.ServoID >= 1 && l.ServoID <= 3 {
		sv := &b.st.servos[l.ServoID-1]
		if sv.Reversed {
			sv.TargetUs = sv.MaxUs
		} else {
			sv.TargetUs = sv.MinUs
		}
	}
	for ch := 0; ch < 8; ch++ {
		if l.ChannelMask&(1<<ch) != 0 {
			b.st.channels[ch].SeqPlaying = false
			b.st.channels[ch].Brightness = 0
		}
	}
}

// HandlePacket dispatches LightFX-range (0x40-0x5F) packets and core
// 0xEE BatteryConfig.
func (b *Board) HandlePacket(s server.Sender, ptype protocol.PacketType, tag byte, payload []byte) bool {
	if ptype == pcore.BatteryConfig {
		b.handleBatteryConfig(s, tag, payload)
		return true
	}
	if ptype < 0x40 || ptype > 0x5F {
		return false
	}
	switch ptype {
	case plfx.LedSet:
		b.handleLedSet(s, tag, payload)
	case plfx.LedOff:
		b.handleLedOff(s, tag, payload)
	case plfx.LedSeqClear:
		b.handleLedSeqClear(s, tag, payload)
	case plfx.LedSeqAdd:
		b.handleLedSeqAdd(s, tag, payload)
	case plfx.LedSeqStart, plfx.LedSeqRestart:
		b.handleLedSeqStart(s, tag, payload)
	case plfx.LedSeqStop:
		b.handleLedSeqStop(s, tag, payload)
	case plfx.LedMasterBrightness:
		b.handleLedMasterBrightness(s, tag, payload)
	case plfx.LedReset:
		b.handleLedReset(s, tag, payload)
	case plfx.LedEnable:
		b.handleLedEnable(s, tag, payload)
	case plfx.ServoSet:
		b.handleServoSet(s, tag, payload)
	case plfx.ServoSettings:
		b.handleServoSettings(s, tag, payload)
	case plfx.LandingLightBind:
		b.handleLandingBind(s, tag, payload)
	case plfx.LandingLightUnbind:
		b.handleLandingUnbind(s, tag, payload)
	case plfx.LandingLightDeploy:
		b.handleLandingDeploy(s, tag, payload)
	case plfx.LandingLightRetract:
		b.handleLandingRetract(s, tag, payload)
	case plfx.BatteryAutoCutoff:
		b.handleBatteryAutoCutoff(s, tag, payload)
	case plfx.LightProgramSelect:
		if len(payload) < 1 {
			s.Nack(tag, plfx.ErrInvalidParam)
			return true
		}
		if !b.SelectProgram(payload[0]) {
			s.Nack(tag, plfx.ErrInvalidProgram)
			return true
		}
		s.Ack(tag)
	case plfx.LightProgramReset:
		b.ResetProgram()
		s.Ack(tag)
	case plfx.LedStatus:
		b.handleLedStatus(s, tag)
	case plfx.LedSeqStatus:
		b.handleLedSeqStatus(s, tag, payload)
	case plfx.LedSeqQueue:
		b.handleLedSeqQueue(s, tag, payload)
	default:
		s.Nack(tag, plfx.ErrInvalidParam)
	}
	return true
}

// ─── BATTERY_CONFIG (core 0xEE) ───

func (b *Board) handleBatteryConfig(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 2 {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	cells := payload[1]
	b.st.mu.Lock()
	b.st.batteryCells = cells
	b.st.batteryMv = uint16(uint32(cells) * 4000)
	b.st.batteryPct = 80
	b.st.mu.Unlock()
	s.Ack(tag)
}

// ─── LED direct ───

func (b *Board) handleLedSet(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 2 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	ch, bright := payload[0], payload[1]
	if ch < 1 || ch > 8 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	b.st.mu.Lock()
	b.st.channels[ch-1].SeqPlaying = false
	b.st.channels[ch-1].Brightness = bright
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleLedOff(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	ch := payload[0]
	b.st.mu.Lock()
	for i := range b.st.channels {
		if ch == 0 || ch == uint8(i+1) {
			b.st.channels[i].SeqPlaying = false
			b.st.channels[i].Brightness = 0
		}
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

// ─── LED sequence ───

func (b *Board) handleLedSeqClear(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	ch := payload[0]
	b.st.mu.Lock()
	for i := range b.st.channels {
		if ch == 0 || ch == uint8(i+1) {
			b.st.channels[i].SeqEvents = nil
			b.st.channels[i].SeqPlaying = false
			b.st.channels[i].SeqIndex = 0
		}
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

// LED_SEQ_ADD: [ch:u8][type:u8][p1:u16][p2:u16][p3:u8][p4:u8][p5:u8?]
func (b *Board) handleLedSeqAdd(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 8 {
		s.Nack(tag, plfx.ErrInvalidEvent)
		return
	}
	ch := payload[0]
	if ch < 1 || ch > 8 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	evt := shared.SeqEvent{
		Type: payload[1],
		P1:   protocol.ReadU16LE(payload, 2),
		P2:   protocol.ReadU16LE(payload, 4),
		P3:   payload[6],
		P4:   payload[7],
	}
	if len(payload) >= 9 {
		evt.P5 = payload[8]
	}
	b.st.mu.Lock()
	if len(b.st.channels[ch-1].SeqEvents) >= 24 {
		b.st.mu.Unlock()
		s.Nack(tag, plfx.ErrSeqFull)
		return
	}
	b.st.channels[ch-1].SeqEvents = append(b.st.channels[ch-1].SeqEvents, evt)
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleLedSeqStart(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	ch := payload[0]
	now := b.now()
	b.st.mu.Lock()
	for i := range b.st.channels {
		if ch == 0 || ch == uint8(i+1) {
			c := &b.st.channels[i]
			if len(c.SeqEvents) > 0 {
				c.SeqPlaying = true
				c.SeqIndex = 0
				c.SeqLoops = 0
				c.SeqStart = now
			}
		}
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleLedSeqStop(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	ch := payload[0]
	b.st.mu.Lock()
	for i := range b.st.channels {
		if ch == 0 || ch == uint8(i+1) {
			b.st.channels[i].SeqPlaying = false
		}
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleLedMasterBrightness(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 || payload[0] > 100 {
		s.Nack(tag, plfx.ErrInvalidParam)
		return
	}
	b.st.mu.Lock()
	b.st.masterBrightness = payload[0]
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleLedReset(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	ch := payload[0]
	b.st.mu.Lock()
	for i := range b.st.channels {
		if ch == 0 || ch == uint8(i+1) {
			b.st.channels[i] = LedChannel{Enabled: true}
		}
	}
	if ch == 0 {
		b.st.masterBrightness = 100
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleLedEnable(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 2 {
		s.Nack(tag, plfx.ErrInvalidParam)
		return
	}
	ch, enabled := payload[0], payload[1] != 0
	b.st.mu.Lock()
	for i := range b.st.channels {
		if ch == 0 || ch == uint8(i+1) {
			b.st.channels[i].Enabled = enabled
			if !enabled {
				b.st.channels[i].SeqPlaying = false
				b.st.channels[i].Brightness = 0
			}
		}
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

// ─── Servos ───

func (b *Board) handleServoSet(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 3 {
		s.Nack(tag, plfx.ErrInvalidServo)
		return
	}
	id := payload[0]
	if id < 1 || id > 3 {
		s.Nack(tag, plfx.ErrInvalidServo)
		return
	}
	pulse := int16(protocol.ReadU16LE(payload, 1))
	b.st.mu.Lock()
	if pulse >= 0 {
		sv := &b.st.servos[id-1]
		if uint16(pulse) < sv.MinUs {
			pulse = int16(sv.MinUs)
		}
		if uint16(pulse) > sv.MaxUs {
			pulse = int16(sv.MaxUs)
		}
		// Set the target only — `Position` ramps toward it through
		// `updateMotion` on subsequent ticks. Mirrors firmware's
		// ServoControl::setTarget() behaviour.
		sv.TargetUs = uint16(pulse)
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleServoSettings(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 11 {
		s.Nack(tag, plfx.ErrInvalidServo)
		return
	}
	id := payload[0]
	if id < 1 || id > 3 {
		s.Nack(tag, plfx.ErrInvalidServo)
		return
	}
	b.st.mu.Lock()
	sv := &b.st.servos[id-1]
	sv.MinUs = protocol.ReadU16LE(payload, 1)
	sv.MaxUs = protocol.ReadU16LE(payload, 3)
	sv.SpeedUs = protocol.ReadU16LE(payload, 5)
	sv.AccelUs = protocol.ReadU16LE(payload, 7)
	sv.DecelUs = protocol.ReadU16LE(payload, 9)
	if len(payload) >= 12 {
		sv.Reversed = payload[11] != 0
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

// ─── Landing-light ───

func (b *Board) handleLandingBind(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 4 {
		s.Nack(tag, plfx.ErrInvalidSlot)
		return
	}
	slot := payload[0]
	if slot < 1 || slot > 3 {
		s.Nack(tag, plfx.ErrInvalidSlot)
		return
	}
	b.st.mu.Lock()
	l := &b.st.landing[slot-1]
	l.Slot = slot
	l.ServoID = payload[1]
	l.ChannelMask = payload[2]
	l.Brightness = payload[3]
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleLandingUnbind(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidSlot)
		return
	}
	slot := payload[0]
	b.st.mu.Lock()
	for i := range b.st.landing {
		if slot == 0 || slot == uint8(i+1) {
			b.st.landing[i] = LandingState{Slot: uint8(i + 1)}
		}
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

func (b *Board) handleLandingDeploy(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidSlot)
		return
	}
	slot := payload[0]
	now := b.now()
	b.st.mu.Lock()
	for i := range b.st.landing {
		if slot == 0 || slot == uint8(i+1) {
			l := &b.st.landing[i]
			if l.Phase == phaseDeployed {
				continue
			}
			l.Phase = phaseDeploying
			l.PhaseDeadline = now // unused now (kept for back-compat with Snapshot)
			// Drive the bound servo toward its OPEN position. Mirrors
			// landing_light.h: open = maxUs (or minUs if reversed).
			if l.ServoID >= 1 && l.ServoID <= 3 {
				sv := &b.st.servos[l.ServoID-1]
				if sv.Reversed {
					sv.TargetUs = sv.MinUs
				} else {
					sv.TargetUs = sv.MaxUs
				}
			}
			for ch := 0; ch < 8; ch++ {
				if l.ChannelMask&(1<<ch) != 0 {
					b.st.channels[ch].SeqPlaying = false
					b.st.channels[ch].Brightness = l.Brightness
				}
			}
		}
	}
	b.st.mu.Unlock()
	s.Ack(tag)
	emitLandingStartStatus(s, slot, phaseDeploying)
}

func (b *Board) handleLandingRetract(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidSlot)
		return
	}
	slot := payload[0]
	now := b.now()
	b.st.mu.Lock()
	for i := range b.st.landing {
		if slot == 0 || slot == uint8(i+1) {
			l := &b.st.landing[i]
			if l.Phase == phaseRetracted {
				continue
			}
			l.Phase = phaseRetracting
			l.PhaseDeadline = now
			// Drive the bound servo toward its CLOSED position.
			if l.ServoID >= 1 && l.ServoID <= 3 {
				sv := &b.st.servos[l.ServoID-1]
				if sv.Reversed {
					sv.TargetUs = sv.MaxUs
				} else {
					sv.TargetUs = sv.MinUs
				}
			}
			for ch := 0; ch < 8; ch++ {
				if l.ChannelMask&(1<<ch) != 0 {
					b.st.channels[ch].SeqPlaying = false
					b.st.channels[ch].Brightness = 0
				}
			}
		}
	}
	b.st.mu.Unlock()
	s.Ack(tag)
	emitLandingStartStatus(s, slot, phaseRetracting)
}

// emitLandingStartStatus fires the async LANDING_LIGHT_STATUS that
// signals "transition started" (finished=0). The matching "finished=1"
// emission used to be scheduled with a goroutine + time.Sleep, but now
// fires from `state.tick` when the bound servo's AtTarget() returns
// true — matching firmware (landing_light.h:120-127) which waits for
// motion to settle before reporting completion.
func emitLandingStartStatus(s server.Sender, slot, phase byte) {
	s.Send(plfx.LandingLightStatus, protocol.TagAsync,
		[]byte{slot, phase, 0 /*finished*/})
}

func (b *Board) handleBatteryAutoCutoff(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidParam)
		return
	}
	enabled := payload[0] != 0
	b.st.mu.Lock()
	b.st.batteryAutoCutoff = enabled
	// Mirror firmware (lightfx_pico.ino:423-424): disabling auto-cutoff
	// re-arms by clearing the latched flag. The channels are NOT
	// auto-re-enabled — the user must explicitly LED_RESET / LED_ENABLE
	// to acknowledge they're aware of the recent low-voltage event.
	if !enabled {
		b.st.batteryLowFired = false
	}
	b.st.mu.Unlock()
	s.Ack(tag)
}

// ─── STATUS module-data builder ───

// BuildStatusModuleData mirrors the LightFX firmware's onStatusData
// callback (lightfx_pico.ino:626). 64-byte block: 8 channel brightness,
// seq-playing flags, 3 servo positions, 3 landing-light phases, master,
// enabled flags, battery + per-servo config tail.
func (b *Board) BuildStatusModuleData() []byte {
	const fullLen = 64
	buf := make([]byte, fullLen)

	b.st.mu.Lock()
	defer b.st.mu.Unlock()

	for i := 0; i < 8; i++ {
		buf[i] = b.st.channels[i].Brightness
	}
	var seqFlags, enFlags uint8
	for i, c := range b.st.channels {
		if c.SeqPlaying {
			seqFlags |= 1 << i
		}
		if c.Enabled {
			enFlags |= 1 << i
		}
	}
	buf[8] = seqFlags

	// Servo 0 (SRV1, GP8): when an RC pulse is being injected, report
	// it in place of the servo position — matches firmware
	// (lightfx_pico.ino:641-642). srv1Input.isEnabled() ? liveInput_us
	// : servos[0].position(). Servos 1-2 always report position.
	if b.st.simulatedRxPulse_us > 0 {
		copy(buf[9:], protocol.U16LE(b.st.simulatedRxPulse_us))
	} else {
		copy(buf[9:], protocol.U16LE(b.st.servos[0].Position))
	}
	copy(buf[11:], protocol.U16LE(b.st.servos[1].Position))
	copy(buf[13:], protocol.U16LE(b.st.servos[2].Position))
	for i := 0; i < 3; i++ {
		buf[15+i] = b.st.landing[i].Phase
	}
	buf[18] = b.st.masterBrightness
	buf[19] = enFlags

	copy(buf[20:], protocol.U16LE(b.st.batteryMv))
	buf[22] = b.st.batteryCells
	buf[23] = b.st.batteryPct

	var battFlags byte
	if b.st.batteryAutoCutoff {
		battFlags |= 0x01
	}
	if b.st.batteryLowFired {
		battFlags |= 0x02
	}
	buf[24] = battFlags

	off := 25
	for i := 0; i < 3; i++ {
		sv := b.st.servos[i]
		copy(buf[off+0:], protocol.U16LE(sv.MinUs))
		copy(buf[off+2:], protocol.U16LE(sv.MaxUs))
		copy(buf[off+4:], protocol.U16LE(sv.TargetUs))
		copy(buf[off+6:], protocol.U16LE(sv.SpeedUs))
		copy(buf[off+8:], protocol.U16LE(sv.AccelUs))
		copy(buf[off+10:], protocol.U16LE(sv.DecelUs))
		if sv.Reversed {
			buf[off+12] = 1
		}
		off += 13
	}
	return buf
}
