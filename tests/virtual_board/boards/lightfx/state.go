// LightFX virtual board — runtime state model.
//
// Holds enough device state to give Studio's panels something to display
// and react to: 8 LED channels with brightness + sequence-playing flags,
// 3 servos with positions + limits, 3 landing-light slots with phases,
// master brightness, battery.

package lightfx

import (
	"sync"
	"time"

	"scalefx/protocol"
	"scalefx/tests/virtual_board/shared"
)

const (
	// Landing-light phase values match LandingLightPhase::* in firmware.
	phaseRetracted  = 0
	phaseDeploying  = 1
	phaseDeployed   = 2
	phaseRetracting = 3
)

// LedChannel mirrors a LightFX LED channel.
type LedChannel struct {
	Brightness uint8 // 0-100
	Enabled    bool
	SeqEvents  []shared.SeqEvent
	SeqPlaying bool
	SeqIndex   uint8
	SeqLoops   uint32
	SeqStart   time.Time
}

// ServoState tracks a single LightFX servo (3 total).
//
// Motion: we mirror the firmware's trapezoidal velocity profiler in
// controllers/lib/sfx_peripherals/servo/srv_control.cpp:189 so a
// landing-light deploy waits the same wall-clock time it does on the
// real board. `posF` / `velocity` are the floating-point integrators
// the firmware uses internally; `Position` is the rounded value
// reported in STATUS.
type ServoState struct {
	Position uint16  // last reported integer position (µs) — what STATUS shows
	MinUs    uint16
	MaxUs    uint16
	TargetUs uint16
	SpeedUs  uint16
	AccelUs  uint16
	DecelUs  uint16
	Reversed bool

	// Motion-profiler state — not exposed in the wire format.
	posF        float32 // continuous position
	velocity    float32 // µs/sec
	lastUpdate  time.Time
	wasMoving   bool
}

// Servo motion tolerances — copied verbatim from
// controllers/lib/sfx_peripherals/servo/srv_control.h ServoControlConfig.
const (
	servoPosTolUs     = 0.5
	servoVelTolUsPerS = 1.0
)

// AtTarget reports whether the servo has settled at its commanded
// position — used by the landing-light state machine to detect "open"
// or "closed". Mirrors `ServoControl::atTarget()` in the firmware.
func (s *ServoState) AtTarget() bool {
	dist := float32(s.TargetUs) - s.posF
	if dist < 0 {
		dist = -dist
	}
	v := s.velocity
	if v < 0 {
		v = -v
	}
	return dist <= servoPosTolUs && v < servoVelTolUsPerS
}

// updateMotion advances the profile by `dt` seconds toward the
// configured target, respecting speed / accel / decel. Mirrors
// controllers/lib/sfx_peripherals/servo/srv_control.cpp:189-274.
func (s *ServoState) updateMotion(now time.Time) {
	if s.lastUpdate.IsZero() {
		s.lastUpdate = now
		s.posF = float32(s.Position)
		return
	}
	dt := float32(now.Sub(s.lastUpdate).Seconds())
	if dt <= 0 {
		return
	}
	s.lastUpdate = now

	maxSpeed := float32(s.SpeedUs)
	accel := float32(s.AccelUs)
	decel := float32(s.DecelUs)

	// Clamp target to limits continuously.
	target := float32(s.TargetUs)
	if target < float32(s.MinUs) {
		target = float32(s.MinUs)
	}
	if target > float32(s.MaxUs) {
		target = float32(s.MaxUs)
	}

	dist := target - s.posF
	dir := float32(0)
	switch {
	case dist > servoPosTolUs:
		dir = 1
	case dist < -servoPosTolUs:
		dir = -1
	}

	// At target with negligible velocity → snap.
	if dir == 0 && abs32(s.velocity) < servoVelTolUsPerS {
		s.posF = target
		s.velocity = 0
		s.wasMoving = false
		s.Position = uint16(s.posF + 0.5)
		return
	}
	s.wasMoving = true

	// Stopping distance from v² / (2·decel)
	var stopDist float32
	if decel > 0 {
		stopDist = (s.velocity * s.velocity) / (2 * decel)
	}
	movingToward := s.velocity*dist > 0

	switch {
	case s.velocity == 0:
		s.velocity += dir * accel * dt
	case movingToward:
		if abs32(dist) <= stopDist {
			s.velocity = approachZero(s.velocity, decel*dt)
		} else {
			s.velocity += dir * accel * dt
		}
	default:
		// Moving away — decelerate first.
		s.velocity = approachZero(s.velocity, decel*dt)
	}

	// Cap speed.
	if s.velocity > maxSpeed {
		s.velocity = maxSpeed
	}
	if s.velocity < -maxSpeed {
		s.velocity = -maxSpeed
	}

	// Integrate.
	s.posF += s.velocity * dt

	// Prevent overshoot.
	if dir > 0 && s.posF > target {
		s.posF = target
		s.velocity = 0
	}
	if dir < 0 && s.posF < target {
		s.posF = target
		s.velocity = 0
	}
	s.Position = uint16(s.posF + 0.5)
}

func abs32(v float32) float32 {
	if v < 0 {
		return -v
	}
	return v
}

func approachZero(v, delta float32) float32 {
	if v > 0 {
		v -= delta
		if v < 0 {
			v = 0
		}
	} else if v < 0 {
		v += delta
		if v > 0 {
			v = 0
		}
	}
	return v
}

// LandingState mirrors LandingLight::state(): RETRACTED/DEPLOYING/
// DEPLOYED/RETRACTING.
type LandingState struct {
	Slot          uint8
	ServoID       uint8 // 0 = no servo
	ChannelMask   uint8
	Brightness    uint8
	Phase         uint8
	PhaseDeadline time.Time
}

// LightProgram models a single program loaded into the board's
// program-runtime — enough for the merged event-timing tests to
// exercise selectProgram + applyGroupPolicies. Mirrors the firmware's
// LightProgramConfig::Program subset that the tests care about.
type LightProgram struct {
	Name           string
	Channels       []ChannelDef
	GroupPolicies  []uint8 // 0=OFF, 1=ON, 2=GEAR
}

// ChannelDef binds a sequence (or a landing-group control) to a channel.
type ChannelDef struct {
	Channel    uint8        // 1-based
	GroupIndex uint8        // 0xFF = events mode, otherwise landing-group slot index (0-based)
	Events     []shared.SeqEvent
}

// LandingGroupDef binds a landing-group slot to a channel mask. Mirrors
// firmware's LandingGroup with the lightfx-side fields only.
type LandingGroupDef struct {
	Name        string
	ServoID     uint8
	ChannelMask uint8 // bit 0 → ch1, … bit 7 → ch8
	Brightness  uint8
}

// LightProgramConfig is the test-time program configuration. Loaded
// in-process via Board.LoadProgramConfig — there is no YAML parsing.
type LightProgramConfig struct {
	MasterBrightness uint8
	LandingGroups    []LandingGroupDef
	Programs         []LightProgram

	// InputBands — optional RC PWM → program-index map. When a pulse
	// arrives via SimulateRxPulse, the tick selects the matching
	// program (with a 500 ms debounce, mirroring firmware
	// LightProgramManager::update). Empty = manual program-select only.
	InputBands []InputBand
}

// Group policy constants — match LightProgramConfig::GROUP_POLICY_* in
// firmware (light_program_config.h).
const (
	GroupPolicyOff  = 0
	GroupPolicyOn   = 1
	GroupPolicyGear = 2
)

// phaseEmitter is the slice of server.Sender that the state machine
// uses to fire async LANDING_LIGHT_STATUS packets when a slot's phase
// transitions. Declared locally so state.go doesn't need to import
// server (which would cycle).
type phaseEmitter interface {
	Send(ptype protocol.PacketType, tag byte, payload []byte)
}

// phaseTransition is the deferred emission record collected inside
// tick() and flushed after the lock is released.
type phaseTransition struct {
	Slot  uint8
	Phase byte // final phase (DEPLOYED or RETRACTED)
}

// state is the per-instance LightFX model.
type state struct {
	mu sync.Mutex

	masterBrightness uint8
	channels         [8]LedChannel
	servos           [3]ServoState
	landing          [3]LandingState

	batteryMv         uint16
	batteryCells      uint8
	batteryPct        uint8
	batteryAutoCutoff bool
	batteryLowFired   bool

	// Program-runtime — populated by Board.LoadProgramConfig and
	// activated by Board.SelectProgram. -1 = no program active.
	programs       []LightProgram
	landingGroups  []LandingGroupDef
	activeProgram  int8

	// SRV1 RC PWM input — when SRV1 is in input role (the firmware's
	// IDLE/STANDALONE/DIRECT board states), STATUS bytes 9-10 carry
	// the latest captured pulse width instead of the servo's
	// commanded position. Mirrors `liveInput_us` in
	// controllers/lightfx/pico/src/lightfx_pico.ino:163.
	simulatedRxPulse_us uint16

	// Light-program input bands — RX pulse → program-index lookup table
	// the firmware populates from /lightfx.yaml; we only fill it via
	// LoadProgramConfig.Input below.
	inputBands       []InputBand
	lastBandSwitch   time.Time // for debounce
}

// InputBand maps an RC-pulse range to a program index. Mirrors the
// firmware's LightProgramConfig::Input::Band.
type InputBand struct {
	MinUs   uint16
	MaxUs   uint16
	Program uint8
}

// SwitchDebounceMs matches LightProgramManagerConfig::SWITCH_DEBOUNCE_MS
// in light_program_manager.h:72.
const switchDebounceMs = 500

func newState() *state {
	s := &state{
		masterBrightness: 100,
		batteryMv:        8200,
		batteryCells:     2,
		batteryPct:       85,
		activeProgram:    -1,
	}
	for i := range s.channels {
		s.channels[i].Enabled = true
	}
	for i := range s.servos {
		s.servos[i] = ServoState{
			Position: 1500, MinUs: 500, MaxUs: 2500, TargetUs: 1500,
			SpeedUs: 4000, AccelUs: 8000, DecelUs: 8000,
		}
	}
	for i := range s.landing {
		s.landing[i].Slot = uint8(i + 1)
	}
	return s
}

// Tick advances time-driven state. Called from the server's 50 ms
// simulation tick — every cadence-sensitive integrator (servo motion,
// landing-light state machine, LED sequence playback) is a `dt`-aware
// step rather than a fixed-period update.
//
// `sender` may be nil — tests drive `tick` without a server. Async
// LANDING_LIGHT_STATUS emissions are skipped in that case (the test
// observes phase via Snapshot()).
func (s *state) tick(sender phaseEmitter, now time.Time) {
	s.mu.Lock()

	// Servo motion — must run before the landing-light state machine
	// since the latter polls `AtTarget()` to decide phase transitions.
	for i := range s.servos {
		s.servos[i].updateMotion(now)
	}

	// Landing-light state machine — DEPLOYING / RETRACTING wait for
	// the bound servo to settle (firmware: landing_light.h:120-127).
	// Slots with no servo (servoId == 0) skip the wait and complete
	// immediately on the same tick they started.
	//
	// Collect "transitioned" events under the lock; emit them after
	// releasing it so the sender's network write doesn't deadlock
	// against a parallel STATUS broadcast.
	var transitions []phaseTransition
	for i := range s.landing {
		l := &s.landing[i]
		if l.Phase != phaseDeploying && l.Phase != phaseRetracting {
			continue
		}
		settled := true
		if l.ServoID >= 1 && l.ServoID <= 3 {
			settled = s.servos[l.ServoID-1].AtTarget()
		}
		if !settled {
			continue
		}
		final := byte(phaseRetracted)
		if l.Phase == phaseDeploying {
			final = phaseDeployed
		}
		l.Phase = final
		transitions = append(transitions, phaseTransition{
			Slot:  l.Slot,
			Phase: final,
		})
	}

	// LED sequences.
	for i := range s.channels {
		ch := &s.channels[i]
		if !ch.SeqPlaying || len(ch.SeqEvents) == 0 {
			continue
		}
		elapsed := uint32(now.Sub(ch.SeqStart) / time.Millisecond)
		ev := ch.SeqEvents[ch.SeqIndex]
		bright, complete := shared.EvaluateEvent(ev, elapsed)
		if complete {
			ch.SeqIndex++
			if int(ch.SeqIndex) >= len(ch.SeqEvents) {
				ch.SeqIndex = 0
				ch.SeqLoops++
			}
			ch.SeqStart = now
			elapsed = 0
			ev = ch.SeqEvents[ch.SeqIndex]
			bright, _ = shared.EvaluateEvent(ev, elapsed)
		}
		ch.Brightness = bright
	}
	s.mu.Unlock()

	// Fire async LANDING_LIGHT_STATUS for every transition that just
	// landed. Skip if the caller didn't supply a sender (tests).
	if sender == nil || len(transitions) == 0 {
		return
	}
	const landingLightStatus protocol.PacketType = 0x56
	for _, t := range transitions {
		sender.Send(landingLightStatus, protocol.TagAsync,
			[]byte{t.Slot, t.Phase, 1 /*finished*/})
	}
}
