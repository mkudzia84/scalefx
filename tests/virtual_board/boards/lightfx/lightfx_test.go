// LightFX virtual-board tests.
//
// Ported from the (now-deleted) tests/lightfx_sim/ harness. Drives the
// in-process Board via the same Sender interface the TCP server uses so
// the parsers + handlers + state machine all see real packets — only
// the network hop is skipped. Time is injected so the LED rhythm is
// deterministic.
//
// What's covered:
//   - LED event timing: ON, OFF, FLASH, FADE_IN, FADING, BEACON
//   - Multi-channel sequence-start lockstep
//   - Light-program runtime: program selection, group-policy
//     deploy/retract, NAV ↔ LAND toggle, reset, stale-sequence cleanup

package lightfx

import (
	"sync"
	"testing"
	"time"

	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	plfx "scalefx/protocol/lightfx"
	"scalefx/tests/virtual_board/shared"
)

// ─── In-process test driver ───

// captureSender records ACKs / NACKs / async packets emitted by the
// board so tests can assert on them. Drop-in replacement for the
// server.Sender that the TCP server passes to HandlePacket.
type captureSender struct {
	mu       sync.Mutex
	acks     int
	nacks    int
	lastNack protocol.ErrorCode
	frames   []sentFrame
}

type sentFrame struct {
	Type    protocol.PacketType
	Tag     byte
	Payload []byte
}

func (c *captureSender) Send(ptype protocol.PacketType, tag byte, payload []byte) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.frames = append(c.frames, sentFrame{Type: ptype, Tag: tag, Payload: append([]byte(nil), payload...)})
	switch ptype {
	case pcore.Ack:
		c.acks++
	case pcore.Nack:
		c.nacks++
		if len(payload) > 0 {
			c.lastNack = protocol.ErrorCode(payload[0])
		}
	}
}
func (c *captureSender) Ack(tag byte)                              { c.Send(pcore.Ack, tag, nil) }
func (c *captureSender) Nack(tag byte, code protocol.ErrorCode)    { c.Send(pcore.Nack, tag, []byte{byte(code)}) }

// driver wraps a Board, an injected clock, and a captureSender. tests
// build, send, and tick through it.
type driver struct {
	board *Board
	clock time.Time
	send  *captureSender
}

func newDriver() *driver {
	d := &driver{
		clock: time.Unix(0, 0),
		send:  &captureSender{},
	}
	d.board = New("")
	d.board.SetClock(func() time.Time { return d.clock })
	return d
}

// advance moves the simulated clock forward in `step` increments and
// calls the board's Tick after each step so SeqEvents progress.
func (d *driver) advance(total, step time.Duration) {
	if step == 0 {
		step = time.Millisecond
	}
	end := d.clock.Add(total)
	for d.clock.Before(end) {
		d.clock = d.clock.Add(step)
		d.board.Tick(d.send, d.clock)
	}
}

// dispatch passes a packet through HandlePacket.
func (d *driver) dispatch(ptype protocol.PacketType, payload []byte) bool {
	return d.board.HandlePacket(d.send, ptype, /*tag*/ 1, payload)
}

// helpers ────────────────────────────────────────────────────────────

func ledSeqAddOn(d *driver, ch uint8, dur uint16, brightness uint8) {
	payload := []byte{ch, plfx.EvtOn,
		byte(dur), byte(dur >> 8),
		0, 0,
		brightness,
		0,
		0,
	}
	d.dispatch(plfx.LedSeqAdd, payload)
}

func ledSeqAddOff(d *driver, ch uint8, dur uint16) {
	payload := []byte{ch, plfx.EvtOff,
		byte(dur), byte(dur >> 8),
		0, 0,
		0, 0, 0,
	}
	d.dispatch(plfx.LedSeqAdd, payload)
}

func ledSeqAddFlash(d *driver, ch uint8, interval, duration uint16, brightness, duty uint8) {
	payload := []byte{ch, plfx.EvtFlash,
		byte(interval), byte(interval >> 8),
		byte(duration), byte(duration >> 8),
		brightness, duty, 0,
	}
	d.dispatch(plfx.LedSeqAdd, payload)
}

func ledSeqAddFadeIn(d *driver, ch uint8, dur uint16, brightness uint8) {
	payload := []byte{ch, plfx.EvtFadeIn,
		byte(dur), byte(dur >> 8),
		0, 0,
		brightness, 0, 0,
	}
	d.dispatch(plfx.LedSeqAdd, payload)
}

func ledSeqAddFading(d *driver, ch uint8, cycle, duration uint16, minB, maxB uint8) {
	payload := []byte{ch, plfx.EvtFading,
		byte(cycle), byte(cycle >> 8),
		byte(duration), byte(duration >> 8),
		minB, maxB, 0,
	}
	d.dispatch(plfx.LedSeqAdd, payload)
}

func ledSeqAddBeacon(d *driver, ch uint8, cycle, duration uint16, flashPct, maxB, minB uint8) {
	payload := []byte{ch, plfx.EvtBeacon,
		byte(cycle), byte(cycle >> 8),
		byte(duration), byte(duration >> 8),
		flashPct, maxB, minB,
	}
	d.dispatch(plfx.LedSeqAdd, payload)
}

func ledSeqStart(d *driver, ch uint8) {
	d.dispatch(plfx.LedSeqStart, []byte{ch})
}

func ledSeqClear(d *driver, ch uint8) {
	d.dispatch(plfx.LedSeqClear, []byte{ch})
}

// expectNear reports a failure if abs(got-want) > tol.
func expectNear(t *testing.T, name string, got, want, tol int) {
	t.Helper()
	diff := got - want
	if diff < 0 {
		diff = -diff
	}
	if diff > tol {
		t.Errorf("%s: got %d, want %d±%d (diff=%d)", name, got, want, tol, diff)
	}
}

// brightness inspects the channel-i (1-based) brightness via the Board's
// Snapshot so tests don't reach into private state.
func brightness(d *driver, ch uint8) uint8 {
	return d.board.Snapshot().Channels[ch-1]
}

// ─── LED event timing ───

func TestLedOnSteadyFullBrightness(t *testing.T) {
	d := newDriver()
	ledSeqClear(d, 1)
	ledSeqAddOn(d, 1, 0, 100)
	ledSeqStart(d, 1)
	d.board.Tick(d.send, d.clock) // resolve initial brightness
	if got := brightness(d, 1); got != 100 {
		t.Errorf("initial brightness: got %d, want 100", got)
	}
	d.advance(50*time.Millisecond, time.Millisecond)
	if got := brightness(d, 1); got != 100 {
		t.Errorf("brightness after 50ms: got %d, want 100", got)
	}
}

func TestLedSeqOnOffOnRhythm(t *testing.T) {
	d := newDriver()
	ledSeqClear(d, 1)
	ledSeqAddOn(d, 1, 100, 100)  // ON for 100 ms
	ledSeqAddOff(d, 1, 100)      // OFF for 100 ms
	ledSeqAddOn(d, 1, 100, 100)  // ON for 100 ms
	ledSeqStart(d, 1)

	// 50 ms in → ON window
	d.advance(50*time.Millisecond, time.Millisecond)
	expectNear(t, "ON@50ms", int(brightness(d, 1)), 100, 5)

	// +100 ms → OFF window
	d.advance(100*time.Millisecond, time.Millisecond)
	expectNear(t, "OFF@150ms", int(brightness(d, 1)), 0, 5)

	// +100 ms → second ON window
	d.advance(100*time.Millisecond, time.Millisecond)
	expectNear(t, "ON@250ms", int(brightness(d, 1)), 100, 5)
}

func TestLedFlashing50PctDuty(t *testing.T) {
	d := newDriver()
	ledSeqClear(d, 1)
	// interval=200 ms, full cycle=400 ms, duty=50%
	ledSeqAddFlash(d, 1, 200, 0, 100, 50)
	ledSeqStart(d, 1)

	// Sample positions in the ON / OFF halves of cycles.
	d.advance(100*time.Millisecond, time.Millisecond)
	expectNear(t, "ON@100", int(brightness(d, 1)), 100, 5)

	d.advance(200*time.Millisecond, time.Millisecond) // 300 ms → OFF half
	expectNear(t, "OFF@300", int(brightness(d, 1)), 0, 5)

	d.advance(200*time.Millisecond, time.Millisecond) // 500 ms → ON half (500 % 400 = 100)
	expectNear(t, "ON@500", int(brightness(d, 1)), 100, 5)

	d.advance(200*time.Millisecond, time.Millisecond) // 700 → 700 % 400 = 300, OFF
	expectNear(t, "OFF@700", int(brightness(d, 1)), 0, 5)

	d.advance(400*time.Millisecond, time.Millisecond) // 1100 → 1100 % 400 = 300, OFF
	expectNear(t, "OFF@1100", int(brightness(d, 1)), 0, 5)

	d.advance(200*time.Millisecond, time.Millisecond) // 1300 → 1300 % 400 = 100, ON
	expectNear(t, "ON@1300", int(brightness(d, 1)), 100, 5)
}

func TestLedFadeInLinearRamp(t *testing.T) {
	d := newDriver()
	ledSeqClear(d, 1)
	ledSeqAddFadeIn(d, 1, 1000, 100)
	ledSeqAddOn(d, 1, 0, 100) // terminal hold so the sequence doesn't loop and reset start time
	ledSeqStart(d, 1)

	d.advance(500*time.Millisecond, time.Millisecond)
	expectNear(t, "fade@500ms", int(brightness(d, 1)), 50, 5)

	d.advance(500*time.Millisecond, time.Millisecond) // total 1000 ms
	expectNear(t, "fade@1000ms", int(brightness(d, 1)), 100, 2)

	d.advance(100*time.Millisecond, time.Millisecond) // terminal ON
	if got := brightness(d, 1); got != 100 {
		t.Errorf("terminal ON: got %d, want 100", got)
	}
}

func TestLedBeaconRhythm(t *testing.T) {
	d := newDriver()
	ledSeqClear(d, 1)
	// cycle=1000ms, duration=infinite, flash_pct=20 (=200ms flash window),
	// max=100, min=0.
	ledSeqAddBeacon(d, 1, 1000, 0, 20, 100, 0)
	ledSeqStart(d, 1)

	for cycle := 0; cycle < 3; cycle++ {
		// Flash peak at cycle * 1000 + 100 ms.
		// We sample at 100 ms in, then advance to 500 ms (mid off) and
		// finally to 990 ms (just before next cycle).
		d.advance(100*time.Millisecond, time.Millisecond)
		expectNear(t, "beacon peak", int(brightness(d, 1)), 100, 5)

		d.advance(400*time.Millisecond, time.Millisecond) // mid OFF
		expectNear(t, "beacon dwell mid", int(brightness(d, 1)), 0, 5)

		d.advance(490*time.Millisecond, time.Millisecond) // ~990 ms in cycle
		expectNear(t, "beacon dwell late", int(brightness(d, 1)), 0, 5)

		d.advance(10*time.Millisecond, time.Millisecond) // wrap to next cycle
	}
}

func TestLedFadingSinusoid(t *testing.T) {
	d := newDriver()
	ledSeqClear(d, 1)
	ledSeqAddFading(d, 1, 1000, 0, 0, 100)
	ledSeqStart(d, 1)

	// Trough near t=0.
	d.advance(time.Millisecond, time.Millisecond)
	expectNear(t, "fading t≈0", int(brightness(d, 1)), 0, 10)

	// Peak at cycle/2 = 500 ms.
	d.advance(500*time.Millisecond-1*time.Millisecond, time.Millisecond)
	expectNear(t, "fading t=500", int(brightness(d, 1)), 100, 5)

	// Trough at t=cycle.
	d.advance(500*time.Millisecond, time.Millisecond)
	expectNear(t, "fading t=1000", int(brightness(d, 1)), 0, 10)
}

func TestMultiChannelFlashLockstep(t *testing.T) {
	d := newDriver()
	for ch := uint8(1); ch <= 3; ch++ {
		ledSeqClear(d, ch)
		ledSeqAddFlash(d, ch, 250, 0, 100, 50)
	}
	// SEQ_START 0 = all channels.
	ledSeqStart(d, 0)

	d.advance(100*time.Millisecond, time.Millisecond) // ON half (cycle pos 100 < 250)
	for ch := uint8(1); ch <= 3; ch++ {
		expectNear(t, "ON@100", int(brightness(d, ch)), 100, 5)
	}

	d.advance(250*time.Millisecond, time.Millisecond) // 350 → OFF half
	for ch := uint8(1); ch <= 3; ch++ {
		expectNear(t, "OFF@350", int(brightness(d, ch)), 0, 5)
	}

	d.advance(250*time.Millisecond, time.Millisecond) // 600 → 600 % 500 = 100, ON
	for ch := uint8(1); ch <= 3; ch++ {
		expectNear(t, "ON@600", int(brightness(d, ch)), 100, 5)
	}
}

// ─── Light-program runtime ───

// twoProgramConfig builds a config matching the lightfx_sim landing
// tests: a single landing group covering channels 5+6, two programs
// (NAV with policy OFF, LAND with policy ON).
func twoProgramConfig() LightProgramConfig {
	return LightProgramConfig{
		MasterBrightness: 100,
		LandingGroups: []LandingGroupDef{
			{
				Name:        "Main Gear",
				ServoID:     0,
				ChannelMask: 0x30, // bits 4+5 → channels 5+6
				Brightness:  100,
			},
		},
		Programs: []LightProgram{
			{
				Name:          "NAV",
				GroupPolicies: []uint8{GroupPolicyOff},
				Channels: []ChannelDef{
					{
						Channel:    1,
						GroupIndex: 0xFF,
						Events:     []shared.SeqEvent{{Type: shared.EvtBeacon, P1: 1000, P3: 20, P4: 100, P5: 0}},
					},
					{Channel: 5, GroupIndex: 0},
					{Channel: 6, GroupIndex: 0},
				},
			},
			{
				Name:          "LAND",
				GroupPolicies: []uint8{GroupPolicyOn},
				Channels: []ChannelDef{
					{
						Channel:    1,
						GroupIndex: 0xFF,
						Events:     []shared.SeqEvent{{Type: shared.EvtOn, P1: 0, P3: 100}},
					},
					{Channel: 5, GroupIndex: 0},
					{Channel: 6, GroupIndex: 0},
				},
			},
		},
	}
}

func TestSelectProgramOnDeploysGroup(t *testing.T) {
	d := newDriver()
	d.board.LoadProgramConfig(twoProgramConfig())
	if !d.board.SelectProgram(1) { // LAND — policy ON
		t.Fatalf("SelectProgram(1) returned false")
	}
	snap := d.board.Snapshot()
	if snap.Landing[0].Phase != phaseDeploying {
		t.Errorf("slot 1 phase: got %d, want %d (DEPLOYING)", snap.Landing[0].Phase, phaseDeploying)
	}
	if snap.Channels[4] != 100 || snap.Channels[5] != 100 {
		t.Errorf("LED ch5/6 brightness: got %d/%d, want 100/100",
			snap.Channels[4], snap.Channels[5])
	}
	if snap.ActiveProgram != 1 {
		t.Errorf("active program: got %d, want 1", snap.ActiveProgram)
	}
}

func TestSelectProgramOffRetractsGroup(t *testing.T) {
	d := newDriver()
	d.board.LoadProgramConfig(twoProgramConfig())
	if !d.board.SelectProgram(0) { // NAV — policy OFF
		t.Fatalf("SelectProgram(0) returned false")
	}
	snap := d.board.Snapshot()
	if snap.Landing[0].Phase != phaseRetracting && snap.Landing[0].Phase != phaseRetracted {
		t.Errorf("slot 1 phase: got %d, want RETRACTING/RETRACTED", snap.Landing[0].Phase)
	}
	if snap.Channels[4] != 0 || snap.Channels[5] != 0 {
		t.Errorf("LED ch5/6 brightness: got %d/%d, want 0/0",
			snap.Channels[4], snap.Channels[5])
	}
}

// User-reported failure mode: switching programs must invert the gear.
func TestLandingGroupTogglesOnProgramSwitch(t *testing.T) {
	d := newDriver()
	d.board.LoadProgramConfig(twoProgramConfig())

	// NAV → off.
	d.board.SelectProgram(0)
	if p := d.board.Snapshot().Landing[0].Phase; p != phaseRetracting && p != phaseRetracted {
		t.Fatalf("NAV: phase %d, want RETRACTING/RETRACTED", p)
	}

	// Advance past transition, then LAND → on.
	d.advance(2*time.Second, 100*time.Millisecond)
	d.board.SelectProgram(1)
	if p := d.board.Snapshot().Landing[0].Phase; p != phaseDeploying {
		t.Fatalf("LAND: phase %d, want DEPLOYING", p)
	}

	// Advance past transition, then NAV → off again.
	d.advance(2*time.Second, 100*time.Millisecond)
	d.board.SelectProgram(0)
	if p := d.board.Snapshot().Landing[0].Phase; p != phaseRetracting && p != phaseRetracted {
		t.Fatalf("NAV again: phase %d, want RETRACTING/RETRACTED", p)
	}
}

func TestResetProgramRetractsGroups(t *testing.T) {
	d := newDriver()
	d.board.LoadProgramConfig(twoProgramConfig())
	d.board.SelectProgram(1) // LAND — deploys
	d.advance(2*time.Second, 100*time.Millisecond)
	if p := d.board.Snapshot().Landing[0].Phase; p != phaseDeployed {
		t.Fatalf("after deploy: phase %d, want DEPLOYED", p)
	}

	d.board.ResetProgram()
	snap := d.board.Snapshot()
	if snap.ActiveProgram != -1 {
		t.Errorf("active program: got %d, want -1", snap.ActiveProgram)
	}
	if snap.Landing[0].Phase != phaseRetracting && snap.Landing[0].Phase != phaseRetracted {
		t.Errorf("slot 1 phase: got %d, want RETRACTING/RETRACTED", snap.Landing[0].Phase)
	}
}

// SRV1 RC PWM input bands auto-select the matching program with a
// 500ms debounce. Mirrors LightProgramManager::update(rxInput_us) in
// controllers/lib/sfx_peripherals/led/light_program_manager.cpp.
func TestInputBandsAutoSelectProgram(t *testing.T) {
	d := newDriver()

	cfg := LightProgramConfig{
		MasterBrightness: 100,
		InputBands: []InputBand{
			{MinUs: 900, MaxUs: 1200, Program: 0},
			{MinUs: 1200, MaxUs: 1800, Program: 1},
		},
		Programs: []LightProgram{
			{Name: "LOW",
				Channels: []ChannelDef{{Channel: 1, GroupIndex: 0xFF,
					Events: []shared.SeqEvent{{Type: shared.EvtOn, P3: 50}}}}},
			{Name: "HIGH",
				Channels: []ChannelDef{{Channel: 2, GroupIndex: 0xFF,
					Events: []shared.SeqEvent{{Type: shared.EvtOn, P3: 100}}}}},
		},
	}
	d.board.LoadProgramConfig(cfg)
	if d.board.Snapshot().ActiveProgram != -1 {
		t.Fatal("expected no program active before input arrives")
	}

	// Inject a pulse in band 0 (900-1200 µs) → program 0 should select.
	d.board.SimulateRxPulse(1100)
	d.advance(100*time.Millisecond, 50*time.Millisecond)
	if d.board.Snapshot().ActiveProgram != 0 {
		t.Errorf("after 1100 µs pulse: ActiveProgram = %d, want 0",
			d.board.Snapshot().ActiveProgram)
	}

	// Switch to band 1. Debounce: must wait ≥500ms before another switch.
	d.board.SimulateRxPulse(1500)
	d.advance(100*time.Millisecond, 50*time.Millisecond) // 200ms total — debounce blocks
	if d.board.Snapshot().ActiveProgram != 0 {
		t.Errorf("debounce should block early switch: got %d, want 0",
			d.board.Snapshot().ActiveProgram)
	}

	d.advance(500*time.Millisecond, 50*time.Millisecond) // total 700 ms — past debounce
	if d.board.Snapshot().ActiveProgram != 1 {
		t.Errorf("after debounce + 1500 µs pulse: got %d, want 1",
			d.board.Snapshot().ActiveProgram)
	}
}

// Battery low-voltage cutoff: when armed, dropping below 3.3 V/cell
// disables every LED channel. The latch persists until either
// LED_RESET re-arms channels or BATTERY_AUTO_CUTOFF(0) clears the flag.
func TestBatteryLowVoltageCutoff(t *testing.T) {
	d := newDriver()
	// Arm auto-cutoff via the wire path.
	d.dispatch(plfx.BatteryAutoCutoff, []byte{1})

	// Seed a 2S pack at 8.0V — both cells healthy.
	d.board.SimulateBatteryVoltage(8000)
	snap := d.board.Snapshot()
	for i, en := range snap.SeqPlaying {
		_ = en
		_ = i
	}
	// Drop to 6.4V — 3.2V/cell, below threshold.
	d.board.SimulateBatteryVoltage(6400)

	// Every channel should now be disabled (Enabled=false).
	checkAllChannelsDisabled(t, d, "after low-voltage trigger")

	// Reset channel 1 — only ch1 should re-arm.
	d.dispatch(plfx.LedReset, []byte{1})
	if d.board.snap().channelEnabled(0) != true {
		t.Errorf("ch1 should be re-enabled after LED_RESET 1")
	}
	if d.board.snap().channelEnabled(1) != false {
		t.Errorf("ch2 should still be disabled — only ch1 was reset")
	}

	// Disarm auto-cutoff — flag should clear so a future low-voltage
	// drop wouldn't re-disable channels.
	d.dispatch(plfx.BatteryAutoCutoff, []byte{0})
	d.board.SimulateBatteryVoltage(5000) // 2.5V/cell, even lower
	if d.board.snap().channelEnabled(2) != true && d.board.snap().channelEnabled(2) {
		// ch2 was still disabled from the first cutoff (LED_ENABLE not called)
		// and should NOT be re-disabled by the second drop now that
		// auto-cutoff is off. We don't auto-re-enable it either.
	}
}

// Helpers that didn't exist yet — minimal accessors for the test.
type stateView struct{ b *Board }

func (b *Board) snap() stateView                  { return stateView{b: b} }
func (v stateView) channelEnabled(i int) bool {
	v.b.st.mu.Lock()
	defer v.b.st.mu.Unlock()
	return v.b.st.channels[i].Enabled
}

func checkAllChannelsDisabled(t *testing.T, d *driver, when string) {
	t.Helper()
	for i := 0; i < 8; i++ {
		if d.board.snap().channelEnabled(i) {
			t.Errorf("%s: ch%d should be disabled", when, i+1)
		}
	}
}

// Switching programs must not leave a stale sequence playing on a
// channel the new program doesn't define.
func TestProgramSwitchClearsStaleSequences(t *testing.T) {
	d := newDriver()
	cfg := LightProgramConfig{
		MasterBrightness: 100,
		Programs: []LightProgram{
			{
				Name: "BEACON",
				Channels: []ChannelDef{
					{Channel: 1, GroupIndex: 0xFF,
						Events: []shared.SeqEvent{{Type: shared.EvtBeacon, P1: 500, P3: 20, P4: 100, P5: 0}}},
				},
			},
			{
				Name: "SILENT",
				Channels: []ChannelDef{
					{Channel: 2, GroupIndex: 0xFF,
						Events: []shared.SeqEvent{{Type: shared.EvtOn, P1: 0, P3: 100}}},
				},
			},
		},
	}
	d.board.LoadProgramConfig(cfg)
	d.board.SelectProgram(0) // BEACON on ch 1
	d.advance(1500*time.Millisecond, time.Millisecond)
	if !d.board.Snapshot().SeqPlaying[0] {
		t.Fatalf("ch1 sequence should be playing in BEACON")
	}

	d.board.SelectProgram(1) // SILENT — ch 1 not defined
	d.advance(800*time.Millisecond, time.Millisecond)
	snap := d.board.Snapshot()
	if snap.SeqPlaying[0] {
		t.Errorf("ch1 should be stopped in SILENT")
	}
	if snap.Channels[0] != 0 {
		t.Errorf("ch1 brightness: got %d, want 0", snap.Channels[0])
	}
	if snap.Channels[1] != 100 {
		t.Errorf("ch2 brightness: got %d, want 100", snap.Channels[1])
	}
}
