// Package slave — engine handler for the generic slave protocol.
//
// Commands prefixed `slave:` (matches Rule 30 — every board command
// group carries a prefix).  Provides typed observers for every async
// event from the slave and CLI commands for every typed action on
// SlaveApi.
//
// Mirrors the per-board handler pattern (see engine/handlers/lightfx,
// /gunfx, etc.) but speaks the unified slave protocol — one handler
// covers any slave board regardless of fingerprint.

package slave

import (
	"context"
	"fmt"
	"strconv"
	"strings"

	"scalefx/api"
	"scalefx/engine"
	slavep "scalefx/protocol/slave"
)

// Handler groups slave-protocol commands + observers.
//
// Async-event observers are pre-seeded with formatter callbacks so
// CLI users see live updates without explicit subscription.  Studio
// adds its own observers via .Add().
type Handler struct {
	E   *engine.Engine
	Api *api.SlaveApi

	OnTargetReached  engine.Observers[api.ServoTargetReachedEvent]
	OnMotionUpdate   engine.Observers[api.ServoMotionUpdateEvent]
	OnPwmStall       engine.Observers[api.PwmStallEvent]
	OnLedProgramDone engine.Observers[api.LedProgramDoneEvent]
	OnStatus         engine.Observers[api.SlaveStatus]
}

// Register installs the slave command group + async observers on the
// engine.  Returns the Handler so external consumers (Studio, tests)
// can install additional listeners.
func Register(eng *engine.Engine, slaveApi *api.SlaveApi) *Handler {
	h := &Handler{E: eng, Api: slaveApi}

	// Pre-seed CLI formatters — silent unless something else also
	// adds observers (Studio opts in independently).
	h.OnTargetReached.Add(h.formatTargetReached)
	h.OnMotionUpdate.Add(h.formatMotionUpdate)
	h.OnPwmStall.Add(h.formatPwmStall)
	h.OnLedProgramDone.Add(h.formatProgramDone)

	// Wire SlaveApi observer chain to the engine observer chain.
	slaveApi.OnServoTargetReached(func(ev api.ServoTargetReachedEvent) {
		h.OnTargetReached.Fire(ev)
	})
	slaveApi.OnServoMotionUpdate(func(ev api.ServoMotionUpdateEvent) {
		h.OnMotionUpdate.Fire(ev)
	})
	slaveApi.OnPwmStall(func(ev api.PwmStallEvent) {
		h.OnPwmStall.Fire(ev)
	})
	slaveApi.OnLedProgramDone(func(ev api.LedProgramDoneEvent) {
		h.OnLedProgramDone.Fire(ev)
	})
	slaveApi.OnStatusBroadcast(func(st api.SlaveStatus) {
		h.OnStatus.Fire(st)
	})

	// Register the command group.  Per Rule 30 every board command
	// group carries a `Prefix` ("slave" here for the generic slave).
	eng.AddCommandGroup(engine.CmdGroup{
		Name:       "Generic Slave",
		Prefix:     "slave",
		Controller: "",   // universal — works against any slave board
		Commands: []engine.CmdEntry{
			{Name: "components", Help: "list components on the attached slave (live runtime modes)",
				Handler: func(args []string) error { return h.cmdComponents(args) }},
			{Name: "ident",      Help: "get/set the slave's user identifier (board name)",
				Handler: func(args []string) error { return h.cmdIdent(args) }},
			{Name: "status",     Help: "synchronous status snapshot — servos + PWM + LED",
				Handler: func(args []string) error { return h.cmdStatus(args) }},
			{Name: "status.rate",Help: "configure periodic status broadcast: <hz> [kindsMask]",
				Handler: func(args []string) error { return h.cmdStatusRate(args) }},

			// Servo
			{Name: "servo.set",     Help: "<idx> <pulse_us> — command servo target",
				Handler: func(args []string) error { return h.cmdServoSet(args) }},
			{Name: "servo.config",  Help: "<idx> <min_us> <max_us> <center_us> <maxSpeed> <accel> <decel>",
				Handler: func(args []string) error { return h.cmdServoConfig(args) }},
			{Name: "servo.motion",  Help: "<idx> <maxSpeed> <accel> <decel> — motion-profile only",
				Handler: func(args []string) error { return h.cmdServoMotion(args) }},
			{Name: "servo.jerk",    Help: "<idx> <offset_us> <duration_ms> — apply transient jerk",
				Handler: func(args []string) error { return h.cmdServoJerk(args) }},
			{Name: "servo.hold",    Help: "<idx> <0|1> — release/hold servo PWM",
				Handler: func(args []string) error { return h.cmdServoHold(args) }},
			{Name: "servo.query",   Help: "<idx> — live position/target/velocity",
				Handler: func(args []string) error { return h.cmdServoQuery(args) }},
			{Name: "servo.updates", Help: "<0|1> [rate_hz] — toggle SERVO_MOTION_UPDATE emission",
				Handler: func(args []string) error { return h.cmdServoMotionUpdates(args) }},

			// PWM
			{Name: "pwm.mode",      Help: "<idx> <generic|led|motor|heater>",
				Handler: func(args []string) error { return h.cmdPwmMode(args) }},
			{Name: "pwm.duty",      Help: "<idx> <duty_thou> — 0..1000",
				Handler: func(args []string) error { return h.cmdPwmDuty(args) }},
			{Name: "pwm.motor",     Help: "<idx> <speed> — signed -1000..+1000",
				Handler: func(args []string) error { return h.cmdPwmMotor(args) }},
			{Name: "pwm.heater",    Help: "<idx> <value>",
				Handler: func(args []string) error { return h.cmdPwmHeater(args) }},
			{Name: "pwm.freq",      Help: "<idx> <freq_Hz>",
				Handler: func(args []string) error { return h.cmdPwmFreq(args) }},
			{Name: "pwm.query",     Help: "<idx> — mode + duty + freq + V/I",
				Handler: func(args []string) error { return h.cmdPwmQuery(args) }},
			{Name: "pwm.config",    Help: "<idx> — full per-channel runtime + hw config",
				Handler: func(args []string) error { return h.cmdPwmGetConfig(args) }},
			{Name: "pwm.stall",     Help: "<idx> <threshold_mA> <debounce_ms> <flags>",
				Handler: func(args []string) error { return h.cmdPwmStallGuard(args) }},
			{Name: "pwm.stall.clear", Help: "<idx>",
				Handler: func(args []string) error { return h.cmdPwmStallClear(args) }},

			// LED
			{Name: "led.set",     Help: "<addr> <brightness 0..255>",
				Handler: func(args []string) error { return h.cmdLedSet(args) }},
			{Name: "led.run",     Help: "<addr> <progId> [flags] — start program",
				Handler: func(args []string) error { return h.cmdLedRun(args) }},
			{Name: "led.stop",    Help: "<addr>",
				Handler: func(args []string) error { return h.cmdLedStop(args) }},
			{Name: "led.restart", Help: "<addr>",
				Handler: func(args []string) error { return h.cmdLedRestart(args) }},
			{Name: "led.reset",   Help: "<addr | 0xFF for broadcast>",
				Handler: func(args []string) error { return h.cmdLedReset(args) }},
			{Name: "led.enable",  Help: "<addr> <0|1>",
				Handler: func(args []string) error { return h.cmdLedEnable(args) }},
			{Name: "led.master",  Help: "<percent 0..100>",
				Handler: func(args []string) error { return h.cmdLedMaster(args) }},
			{Name: "led.query",   Help: "<addr>",
				Handler: func(args []string) error { return h.cmdLedQuery(args) }},
		},
	})
	return h
}

// ── Async-event formatters (CLI display) ─────────────────────────────

func (h *Handler) formatTargetReached(ev api.ServoTargetReachedEvent) {
	h.E.Out.Printf("  servo[%d]: target reached @ %d µs\n", ev.Index, ev.Position)
}

func (h *Handler) formatMotionUpdate(ev api.ServoMotionUpdateEvent) {
	h.E.Out.Printf("  servo[%d]: pos=%d µs target=%d µs vel=%d µs/s\n",
		ev.Index, ev.Position, ev.Target, ev.Velocity)
}

func (h *Handler) formatPwmStall(ev api.PwmStallEvent) {
	h.E.Out.Printf("  pwm[%d]: STALL — peak=%d mA dur=%d ms\n",
		ev.Index, ev.PeakMA, ev.DurationMs)
}

func (h *Handler) formatProgramDone(ev api.LedProgramDoneEvent) {
	pool := "dedicated"
	if slavep.LedAddrIsPwmBorrowed(ev.Address) {
		pool = "pwm-borrowed"
	}
	h.E.Out.Printf("  led[%s/%d]: program %d done\n",
		pool, slavep.LedAddrIndex(ev.Address), ev.ProgID)
}

// ── Command implementations ──────────────────────────────────────────

func (h *Handler) cmdComponents(args []string) error {
	comps, ar := h.Api.RequestComponentList(context.Background())
	if !ar.OK {
		return ar.Err
	}
	h.E.Out.Printf("Components on slave (%d):\n", len(comps))
	for _, c := range comps {
		h.E.Out.Printf("  idx=%d  kind=%s (0x%02X)  flags=0x%02X\n",
			c.Index, c.Kind, byte(c.Kind), c.Flags)
	}
	return nil
}

func (h *Handler) cmdIdent(args []string) error {
	if len(args) == 0 {
		ident, ar := h.Api.GetIdentifier(context.Background())
		if !ar.OK {
			return ar.Err
		}
		name := ident.Name
		if name == "" {
			name = "(unassigned)"
		}
		h.E.Out.Printf("identifier: %q  boardType=0x%02X\n", name, ident.BoardType)
		return nil
	}
	name := strings.Join(args, " ")
	if ar := h.Api.SetIdentifier(context.Background(), name); !ar.OK {
		return ar.Err
	}
	h.E.Out.Printf("identifier set to %q (persisted)\n", name)
	return nil
}

func (h *Handler) cmdStatus(args []string) error {
	mask := uint8(slavep.StatusKindAll)
	if len(args) >= 1 {
		v, err := parseUint8(args[0])
		if err != nil {
			return err
		}
		mask = v
	}
	st, ar := h.Api.RequestStatus(context.Background(), mask)
	if !ar.OK {
		return ar.Err
	}
	h.E.Out.Printf("boardState=0x%02X mode=0x%02X uptime=%d ms freeRam=%d B\n",
		st.BoardState, st.InitMode, st.UptimeMs, st.FreeRamB)
	for _, s := range st.Servos {
		h.E.Out.Printf("  servo idx=%d pos=%d target=%d vel=%d flags=0x%02X (port_id=0x%02X)\n",
			slavep.PortIdIndex(s.PortID), s.Position, s.Target, s.Velocity, s.Flags, s.PortID)
	}
	for _, p := range st.Pwms {
		h.E.Out.Printf("  pwm   idx=%d mode=%s duty=%d V=%d mV I=%d mA stallFlags=0x%02X peak=%d (port_id=0x%02X)\n",
			slavep.PortIdIndex(p.PortID), p.Mode, p.Duty,
			p.VoltageMV, p.CurrentMA, p.StallFlags, p.PeakMA, p.PortID)
	}
	for _, l := range st.Leds {
		h.E.Out.Printf("  led   idx=%d brightness=%d progState=0x%02X progId=%d (port_id=0x%02X)\n",
			slavep.PortIdIndex(l.PortID), l.Brightness, l.ProgState, l.ProgID, l.PortID)
	}
	return nil
}

func (h *Handler) cmdStatusRate(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:status.rate <hz> [kindsMask]")
	}
	hz, err := parseUint8(args[0])
	if err != nil {
		return err
	}
	mask := uint8(0)
	if len(args) >= 2 {
		mask, err = parseUint8(args[1])
		if err != nil {
			return err
		}
	}
	if ar := h.Api.SetStatusRate(context.Background(), hz, mask); !ar.OK {
		return ar.Err
	}
	h.E.Out.Printf("status broadcast: %d Hz, kindsMask=0x%02X\n", hz, mask)
	return nil
}

// ── Servo commands ───────────────────────────────────────────────────

func (h *Handler) cmdServoSet(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: slave:servo.set <idx> <pulse_us>")
	}
	idx, err := parseUint8(args[0])
	if err != nil {
		return err
	}
	us, err := parseUint16(args[1])
	if err != nil {
		return err
	}
	return h.Api.ServoSet(context.Background(), idx, us).AsError()
}

func (h *Handler) cmdServoConfig(args []string) error {
	if len(args) < 7 {
		return fmt.Errorf("usage: slave:servo.config <idx> <min> <max> <center> <maxSpeed> <accel> <decel>")
	}
	idx, _ := parseUint8(args[0])
	mn, _ := parseUint16(args[1])
	mx, _ := parseUint16(args[2])
	ct, _ := parseUint16(args[3])
	sp, _ := parseUint16(args[4])
	ac, _ := parseUint16(args[5])
	de, _ := parseUint16(args[6])
	return h.Api.ServoConfig(context.Background(), idx, mn, mx, ct, sp, ac, de).AsError()
}

func (h *Handler) cmdServoMotion(args []string) error {
	if len(args) < 4 {
		return fmt.Errorf("usage: slave:servo.motion <idx> <maxSpeed> <accel> <decel>")
	}
	idx, _ := parseUint8(args[0])
	sp, _ := parseUint16(args[1])
	ac, _ := parseUint16(args[2])
	de, _ := parseUint16(args[3])
	return h.Api.ServoSetMotion(context.Background(), idx, sp, ac, de).AsError()
}

func (h *Handler) cmdServoJerk(args []string) error {
	if len(args) < 3 {
		return fmt.Errorf("usage: slave:servo.jerk <idx> <offset_us> <duration_ms>")
	}
	idx, _ := parseUint8(args[0])
	off, err := strconv.Atoi(args[1])
	if err != nil {
		return err
	}
	dur, _ := parseUint16(args[2])
	return h.Api.ServoApplyJerk(context.Background(), idx, int16(off), dur).AsError()
}

func (h *Handler) cmdServoHold(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: slave:servo.hold <idx> <0|1>")
	}
	idx, _ := parseUint8(args[0])
	hold := args[1] == "1" || strings.EqualFold(args[1], "true")
	return h.Api.ServoHold(context.Background(), idx, hold).AsError()
}

func (h *Handler) cmdServoQuery(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:servo.query <idx>")
	}
	idx, _ := parseUint8(args[0])
	r, ar := h.Api.ServoQuery(context.Background(), idx)
	if !ar.OK {
		return ar.Err
	}
	h.E.Out.Printf("servo[%d]: pos=%d µs target=%d µs vel=%d µs/s flags=0x%02X\n",
		r.Index, r.Position, r.Target, r.Velocity, r.Flags)
	return nil
}

func (h *Handler) cmdServoMotionUpdates(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:servo.updates <0|1> [rate_hz]")
	}
	en := args[0] == "1" || strings.EqualFold(args[0], "true")
	rate := uint8(0)
	if len(args) >= 2 {
		v, err := parseUint8(args[1])
		if err != nil {
			return err
		}
		rate = v
	}
	return h.Api.ServoMotionUpdates(context.Background(), en, rate).AsError()
}

// ── PWM commands ─────────────────────────────────────────────────────

func parsePwmMode(s string) (slavep.ComponentKind, error) {
	switch strings.ToLower(s) {
	case "generic", "pwm":
		return slavep.KindPwmGeneric, nil
	case "led":
		return slavep.KindPwmLed, nil
	case "motor":
		return slavep.KindPwmMotor, nil
	case "heater":
		return slavep.KindPwmHeater, nil
	}
	return 0, fmt.Errorf("unknown PWM mode %q (want generic|led|motor|heater)", s)
}

func (h *Handler) cmdPwmMode(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: slave:pwm.mode <idx> <generic|led|motor|heater>")
	}
	idx, _ := parseUint8(args[0])
	mode, err := parsePwmMode(args[1])
	if err != nil {
		return err
	}
	return h.Api.PwmSetMode(context.Background(), idx, mode).AsError()
}

func (h *Handler) cmdPwmDuty(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: slave:pwm.duty <idx> <duty_thou>")
	}
	idx, _ := parseUint8(args[0])
	duty, _ := parseUint16(args[1])
	return h.Api.PwmSetDuty(context.Background(), idx, duty).AsError()
}

func (h *Handler) cmdPwmMotor(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: slave:pwm.motor <idx> <speed -1000..+1000>")
	}
	idx, _ := parseUint8(args[0])
	speed, err := strconv.Atoi(args[1])
	if err != nil {
		return err
	}
	return h.Api.PwmSetMotor(context.Background(), idx, int16(speed)).AsError()
}

func (h *Handler) cmdPwmHeater(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: slave:pwm.heater <idx> <value>")
	}
	idx, _ := parseUint8(args[0])
	v, _ := parseUint16(args[1])
	return h.Api.PwmSetHeater(context.Background(), idx, v).AsError()
}

func (h *Handler) cmdPwmFreq(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: slave:pwm.freq <idx> <freq_Hz>")
	}
	idx, _ := parseUint8(args[0])
	f, _ := parseUint16(args[1])
	return h.Api.PwmSetFrequency(context.Background(), idx, f).AsError()
}

func (h *Handler) cmdPwmQuery(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:pwm.query <idx>")
	}
	idx, _ := parseUint8(args[0])
	r, ar := h.Api.PwmQuery(context.Background(), idx)
	if !ar.OK {
		return ar.Err
	}
	h.E.Out.Printf("pwm[%d]: mode=%s duty=%d freq=%d Hz V=%d mV I=%d mA\n",
		r.Index, r.Mode, r.Duty, r.FreqHz, r.VoltageMV, r.CurrentMA)
	return nil
}

func (h *Handler) cmdPwmGetConfig(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:pwm.config <idx>")
	}
	idx, _ := parseUint8(args[0])
	r, ar := h.Api.PwmGetConfig(context.Background(), idx)
	if !ar.OK {
		return ar.Err
	}
	h.E.Out.Printf("pwm[%d] config: mode=%s freq=%d Hz cfgFlags=0x%02X maxDuty=%d hwFlags=0x%02X vSense=%d cSense=%d pair=%d\n",
		r.Index, r.Mode, r.FreqHz, r.CfgFlags, r.MaxDuty, r.HwFlags,
		r.VoltageSenseIdx, r.CurrentSenseIdx, r.PairedWith)
	return nil
}

func (h *Handler) cmdPwmStallGuard(args []string) error {
	if len(args) < 4 {
		return fmt.Errorf("usage: slave:pwm.stall <idx> <threshold_mA> <debounce_ms> <flags>")
	}
	idx, _ := parseUint8(args[0])
	thr, _ := parseUint16(args[1])
	deb, _ := parseUint8(args[2])
	flg, _ := parseUint8(args[3])
	return h.Api.PwmSetStallGuard(context.Background(), idx, thr, deb, flg).AsError()
}

func (h *Handler) cmdPwmStallClear(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:pwm.stall.clear <idx>")
	}
	idx, _ := parseUint8(args[0])
	return h.Api.PwmClearStall(context.Background(), idx).AsError()
}

// ── LED commands ─────────────────────────────────────────────────────

func parseLedAddr(s string) (uint8, error) {
	// Accept either raw byte ("0x82") or "<pool>:<idx>" (pool=ded|pwm).
	if strings.Contains(s, ":") {
		parts := strings.SplitN(s, ":", 2)
		idx, err := parseUint8(parts[1])
		if err != nil {
			return 0, err
		}
		switch strings.ToLower(parts[0]) {
		case "ded", "dedicated":
			return slavep.LedAddrDedicated(idx), nil
		case "pwm", "borrowed":
			return slavep.LedAddrPwmBorrowed(idx), nil
		}
		return 0, fmt.Errorf("unknown LED pool %q (want ded|pwm)", parts[0])
	}
	return parseUint8(s)
}

func (h *Handler) cmdLedSet(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: slave:led.set <addr> <brightness 0..255>")
	}
	addr, err := parseLedAddr(args[0])
	if err != nil {
		return err
	}
	bri, err := parseUint8(args[1])
	if err != nil {
		return err
	}
	return h.Api.LedSetBrightness(context.Background(), addr, bri).AsError()
}

func (h *Handler) cmdLedRun(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: slave:led.run <addr> <progId> [flags]")
	}
	addr, err := parseLedAddr(args[0])
	if err != nil {
		return err
	}
	pid, _ := parseUint8(args[1])
	flags := uint8(0)
	if len(args) >= 3 {
		flags, _ = parseUint8(args[2])
	}
	return h.Api.LedRunProgram(context.Background(), addr, pid, flags).AsError()
}

func (h *Handler) cmdLedStop(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:led.stop <addr>")
	}
	addr, err := parseLedAddr(args[0])
	if err != nil {
		return err
	}
	return h.Api.LedStopProgram(context.Background(), addr).AsError()
}

func (h *Handler) cmdLedRestart(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:led.restart <addr>")
	}
	addr, err := parseLedAddr(args[0])
	if err != nil {
		return err
	}
	return h.Api.LedRestartProgram(context.Background(), addr).AsError()
}

func (h *Handler) cmdLedReset(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:led.reset <addr | 0xFF>")
	}
	addr, err := parseLedAddr(args[0])
	if err != nil {
		return err
	}
	return h.Api.LedResetChannel(context.Background(), addr).AsError()
}

func (h *Handler) cmdLedEnable(args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: slave:led.enable <addr> <0|1>")
	}
	addr, err := parseLedAddr(args[0])
	if err != nil {
		return err
	}
	en := args[1] == "1" || strings.EqualFold(args[1], "true")
	return h.Api.LedEnableChannel(context.Background(), addr, en).AsError()
}

func (h *Handler) cmdLedMaster(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:led.master <0..100>")
	}
	pct, err := parseUint8(args[0])
	if err != nil {
		return err
	}
	return h.Api.LedSetMasterBrightness(context.Background(), pct).AsError()
}

func (h *Handler) cmdLedQuery(args []string) error {
	if len(args) < 1 {
		return fmt.Errorf("usage: slave:led.query <addr>")
	}
	addr, err := parseLedAddr(args[0])
	if err != nil {
		return err
	}
	r, ar := h.Api.LedQuery(context.Background(), addr)
	if !ar.OK {
		return ar.Err
	}
	pool := "dedicated"
	if slavep.LedAddrIsPwmBorrowed(r.Address) {
		pool = "pwm-borrowed"
	}
	h.E.Out.Printf("led[%s/%d]: brightness=%d progId=%d progState=0x%02X\n",
		pool, slavep.LedAddrIndex(r.Address), r.Brightness, r.ProgID, r.ProgState)
	return nil
}

// ── parse helpers ────────────────────────────────────────────────────

func parseUint8(s string) (uint8, error) {
	v, err := strconv.ParseUint(s, 0, 8)
	return uint8(v), err
}

func parseUint16(s string) (uint16, error) {
	v, err := strconv.ParseUint(s, 0, 16)
	return uint16(v), err
}
