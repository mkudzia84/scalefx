// Package api — typed master-side API for the generic-slave protocol.
//
// SlaveApi wraps a protocol.Connection with typed method calls for
// every wire packet, plus a registration surface for async-event
// observers.  The CLI / Studio / effect orchestrators all consume
// SlaveApi rather than raw packets — Rule 3 in CLAUDE.md ("Client
// methods return CommandResult, never bool").
//
// This is the Go side of the dual-language client; the C++ master-
// side equivalent lives at
// controllers/lib/sfx_slave/client/slave_client.h.

package api

import (
	"context"
	"fmt"
	"sync"

	"scalefx/protocol"
	slavep "scalefx/protocol/slave"
)

// ── Async event types delivered to observers ─────────────────────────

type ServoTargetReachedEvent = slavep.ServoTargetReachedEvent
type ServoMotionUpdateEvent  = slavep.ServoMotionUpdateEvent
type PwmStallEvent           = slavep.PwmStallEvent
type LedProgramDoneEvent     = slavep.LedProgramDoneEvent
type SlaveStatus             = slavep.SlaveStatus

// ── SlaveApi ─────────────────────────────────────────────────────────

type SlaveApi struct {
	c    *protocol.Connection
	mu   sync.Mutex // observer-list mutation guard
	obs  observerSet
}

type observerSet struct {
	onTargetReached []func(ServoTargetReachedEvent)
	onMotionUpdate  []func(ServoMotionUpdateEvent)
	onPwmStall      []func(PwmStallEvent)
	onProgramDone   []func(LedProgramDoneEvent)
	onStatus        []func(SlaveStatus)
}

// NewSlaveApi returns a SlaveApi wrapping `conn`.  The caller must
// have already opened the connection and run the standard handshake
// (INIT_READY exchange).  SlaveApi installs its own async-packet
// router on the connection — overrides any prior router.
func NewSlaveApi(conn *protocol.Connection) *SlaveApi {
	api := &SlaveApi{c: conn}
	conn.OnAsync(api.routeAsync)
	return api
}

// routeAsync is the connection's async-packet hook; SlaveApi
// dispatches to the registered observer chains based on packet type.
func (s *SlaveApi) routeAsync(pkt protocol.Packet) {
	switch pkt.Type {
	case slavep.ServoTargetReached:
		ev, err := slavep.DecodeServoTargetReached(pkt.Payload)
		if err == nil {
			s.fanoutTargetReached(ev)
		}
	case slavep.ServoMotionUpdate:
		ev, err := slavep.DecodeServoMotionUpdate(pkt.Payload)
		if err == nil {
			s.fanoutMotionUpdate(ev)
		}
	case slavep.PwmStall:
		ev, err := slavep.DecodePwmStall(pkt.Payload)
		if err == nil {
			s.fanoutPwmStall(ev)
		}
	case slavep.LedProgramDone:
		ev, err := slavep.DecodeLedProgramDone(pkt.Payload)
		if err == nil {
			s.fanoutProgramDone(ev)
		}
	case slavep.SlaveStatusBroadcast:
		st, err := slavep.DecodeStatus(pkt.Payload)
		if err == nil {
			s.fanoutStatus(st)
		}
	}
}

// ── Observer registration ────────────────────────────────────────────

func (s *SlaveApi) OnServoTargetReached(fn func(ServoTargetReachedEvent)) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.obs.onTargetReached = append(s.obs.onTargetReached, fn)
}

func (s *SlaveApi) OnServoMotionUpdate(fn func(ServoMotionUpdateEvent)) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.obs.onMotionUpdate = append(s.obs.onMotionUpdate, fn)
}

func (s *SlaveApi) OnPwmStall(fn func(PwmStallEvent)) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.obs.onPwmStall = append(s.obs.onPwmStall, fn)
}

func (s *SlaveApi) OnLedProgramDone(fn func(LedProgramDoneEvent)) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.obs.onProgramDone = append(s.obs.onProgramDone, fn)
}

func (s *SlaveApi) OnStatusBroadcast(fn func(SlaveStatus)) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.obs.onStatus = append(s.obs.onStatus, fn)
}

// ── Internal fanout (snapshot under lock, then call without it) ──────

func (s *SlaveApi) fanoutTargetReached(ev ServoTargetReachedEvent) {
	s.mu.Lock()
	cbs := append([]func(ServoTargetReachedEvent)(nil), s.obs.onTargetReached...)
	s.mu.Unlock()
	for _, cb := range cbs {
		cb(ev)
	}
}

func (s *SlaveApi) fanoutMotionUpdate(ev ServoMotionUpdateEvent) {
	s.mu.Lock()
	cbs := append([]func(ServoMotionUpdateEvent)(nil), s.obs.onMotionUpdate...)
	s.mu.Unlock()
	for _, cb := range cbs {
		cb(ev)
	}
}

func (s *SlaveApi) fanoutPwmStall(ev PwmStallEvent) {
	s.mu.Lock()
	cbs := append([]func(PwmStallEvent)(nil), s.obs.onPwmStall...)
	s.mu.Unlock()
	for _, cb := range cbs {
		cb(ev)
	}
}

func (s *SlaveApi) fanoutProgramDone(ev LedProgramDoneEvent) {
	s.mu.Lock()
	cbs := append([]func(LedProgramDoneEvent)(nil), s.obs.onProgramDone...)
	s.mu.Unlock()
	for _, cb := range cbs {
		cb(ev)
	}
}

func (s *SlaveApi) fanoutStatus(st SlaveStatus) {
	s.mu.Lock()
	cbs := append([]func(SlaveStatus)(nil), s.obs.onStatus...)
	s.mu.Unlock()
	for _, cb := range cbs {
		cb(st)
	}
}

// ── Identity / enumeration ───────────────────────────────────────────

func (s *SlaveApi) RequestComponentList(ctx context.Context) ([]slavep.ComponentInfo, ApiResult) {
	resp, ar := s.c.SendQuery(ctx, slavep.ComponentListReq, nil, slavep.ComponentListResp)
	if !ar.OK {
		return nil, ar
	}
	out, err := slavep.DecodeComponentList(resp.Payload)
	if err != nil {
		return nil, FailF("decode COMPONENT_LIST_RESP: %v", err)
	}
	return out, OK()
}

func (s *SlaveApi) GetIdentifier(ctx context.Context) (slavep.IdentResponse, ApiResult) {
	resp, ar := s.c.SendQuery(ctx, slavep.IdentGetReq, nil, slavep.IdentGetResp)
	if !ar.OK {
		return slavep.IdentResponse{}, ar
	}
	out, err := slavep.DecodeIdentResp(resp.Payload)
	if err != nil {
		return slavep.IdentResponse{}, FailF("decode IDENT_GET_RESP: %v", err)
	}
	return out, OK()
}

func (s *SlaveApi) SetIdentifier(ctx context.Context, name string) ApiResult {
	if len(name) > 32 {
		return FailF("identifier too long: %d > 32 bytes", len(name))
	}
	payload := append([]byte{byte(len(name))}, []byte(name)...)
	return s.c.SendCommand(ctx, slavep.IdentSet, payload)
}

// RequestStatus polls a synchronous status snapshot.  `kindsMask`
// filters which sections appear (slavep.StatusKind* — 0 = all).
func (s *SlaveApi) RequestStatus(ctx context.Context, kindsMask uint8) (SlaveStatus, ApiResult) {
	resp, ar := s.c.SendQuery(ctx, slavep.SlaveStatusReq, []byte{kindsMask}, slavep.SlaveStatusBroadcast)
	if !ar.OK {
		return SlaveStatus{}, ar
	}
	st, err := slavep.DecodeStatus(resp.Payload)
	if err != nil {
		return SlaveStatus{}, FailF("decode status: %v", err)
	}
	return st, OK()
}

// SetStatusRate configures the periodic broadcast.  hz=0 disables.
func (s *SlaveApi) SetStatusRate(ctx context.Context, hz uint8, kindsMask uint8) ApiResult {
	return s.c.SendCommand(ctx, slavep.SlaveStatusRate, []byte{hz, kindsMask})
}

// ── Servo ────────────────────────────────────────────────────────────

func (s *SlaveApi) ServoSet(ctx context.Context, idx uint8, pulseUs uint16) ApiResult {
	return s.c.SendCommand(ctx, slavep.ServoSet, slavep.EncodeServoSet(idx, pulseUs))
}

func (s *SlaveApi) ServoConfig(ctx context.Context, idx uint8, minUs, maxUs, centerUs, maxSpeed, accel, decel uint16) ApiResult {
	return s.c.SendCommand(ctx, slavep.ServoConfig,
		slavep.EncodeServoConfig(idx, minUs, maxUs, centerUs, maxSpeed, accel, decel))
}

func (s *SlaveApi) ServoSetMotion(ctx context.Context, idx uint8, maxSpeed, accel, decel uint16) ApiResult {
	return s.c.SendCommand(ctx, slavep.ServoSetMotion,
		slavep.EncodeServoSetMotion(idx, maxSpeed, accel, decel))
}

func (s *SlaveApi) ServoApplyJerk(ctx context.Context, idx uint8, offsetUs int16, durationMs uint16) ApiResult {
	return s.c.SendCommand(ctx, slavep.ServoApplyJerk,
		slavep.EncodeServoApplyJerk(idx, offsetUs, durationMs))
}

func (s *SlaveApi) ServoHold(ctx context.Context, idx uint8, hold bool) ApiResult {
	return s.c.SendCommand(ctx, slavep.ServoHold, slavep.EncodeServoHold(idx, hold))
}

func (s *SlaveApi) ServoQuery(ctx context.Context, idx uint8) (slavep.ServoQueryResponse, ApiResult) {
	resp, ar := s.c.SendQuery(ctx, slavep.ServoQuery, []byte{idx}, slavep.ServoQueryResp)
	if !ar.OK {
		return slavep.ServoQueryResponse{}, ar
	}
	out, err := slavep.DecodeServoQueryResp(resp.Payload)
	if err != nil {
		return slavep.ServoQueryResponse{}, FailF("decode SERVO_QUERY_RESP: %v", err)
	}
	return out, OK()
}

func (s *SlaveApi) ServoMotionUpdates(ctx context.Context, enable bool, rateHz uint8) ApiResult {
	return s.c.SendCommand(ctx, slavep.ServoMotionUpdates,
		slavep.EncodeServoMotionUpdates(enable, rateHz))
}

// ── PWM ──────────────────────────────────────────────────────────────

func (s *SlaveApi) PwmSetMode(ctx context.Context, idx uint8, mode slavep.ComponentKind) ApiResult {
	return s.c.SendCommand(ctx, slavep.PwmSetMode, slavep.EncodePwmSetMode(idx, mode))
}

func (s *SlaveApi) PwmSetDuty(ctx context.Context, idx uint8, dutyThou uint16) ApiResult {
	return s.c.SendCommand(ctx, slavep.PwmSetDuty, slavep.EncodePwmSetDuty(idx, dutyThou))
}

func (s *SlaveApi) PwmSetMotor(ctx context.Context, idx uint8, speedSigned int16) ApiResult {
	return s.c.SendCommand(ctx, slavep.PwmSetMotor, slavep.EncodePwmSetMotor(idx, speedSigned))
}

func (s *SlaveApi) PwmSetHeater(ctx context.Context, idx uint8, value uint16) ApiResult {
	return s.c.SendCommand(ctx, slavep.PwmSetHeater, slavep.EncodePwmSetHeater(idx, value))
}

func (s *SlaveApi) PwmSetFrequency(ctx context.Context, idx uint8, freqHz uint16) ApiResult {
	return s.c.SendCommand(ctx, slavep.PwmSetFreq, slavep.EncodePwmSetFreq(idx, freqHz))
}

func (s *SlaveApi) PwmReconfigure(ctx context.Context, idx uint8, mode slavep.ComponentKind, freqHz uint16, cfgFlags uint8, maxDuty uint16) ApiResult {
	return s.c.SendCommand(ctx, slavep.PwmReconfigure,
		slavep.EncodePwmReconfigure(idx, mode, freqHz, cfgFlags, maxDuty))
}

func (s *SlaveApi) PwmQuery(ctx context.Context, idx uint8) (slavep.PwmQueryResponse, ApiResult) {
	resp, ar := s.c.SendQuery(ctx, slavep.PwmQuery, []byte{idx}, slavep.PwmQueryResp)
	if !ar.OK {
		return slavep.PwmQueryResponse{}, ar
	}
	out, err := slavep.DecodePwmQueryResp(resp.Payload)
	if err != nil {
		return slavep.PwmQueryResponse{}, FailF("decode PWM_QUERY_RESP: %v", err)
	}
	return out, OK()
}

func (s *SlaveApi) PwmGetConfig(ctx context.Context, idx uint8) (slavep.PwmGetConfigResponse, ApiResult) {
	resp, ar := s.c.SendQuery(ctx, slavep.PwmGetConfig, []byte{idx}, slavep.PwmGetConfigResp)
	if !ar.OK {
		return slavep.PwmGetConfigResponse{}, ar
	}
	out, err := slavep.DecodePwmGetConfigResp(resp.Payload)
	if err != nil {
		return slavep.PwmGetConfigResponse{}, FailF("decode PWM_GET_CONFIG_RESP: %v", err)
	}
	return out, OK()
}

// PwmSetStallGuard arms (or disarms) the per-channel stall watchdog.
// flags = slavep.StallEnabled | StallAutoStop | StallBrakeOnStop | StallLatch.
// Set flags=0 to disable without losing the threshold/debounce config.
func (s *SlaveApi) PwmSetStallGuard(ctx context.Context, idx uint8, thresholdMA uint16, debounceMs, flags uint8) ApiResult {
	return s.c.SendCommand(ctx, slavep.PwmSetStallGuard,
		slavep.EncodePwmSetStallGuard(idx, thresholdMA, debounceMs, flags))
}

func (s *SlaveApi) PwmClearStall(ctx context.Context, idx uint8) ApiResult {
	return s.c.SendCommand(ctx, slavep.PwmClearStall, []byte{idx})
}

// ── LED ──────────────────────────────────────────────────────────────

func (s *SlaveApi) LedSetBrightness(ctx context.Context, addr, brightness uint8) ApiResult {
	return s.c.SendCommand(ctx, slavep.LedSetBrightness, slavep.EncodeLedSetBrightness(addr, brightness))
}

func (s *SlaveApi) LedLoadProgram(ctx context.Context, addr, progID uint8, events []slavep.LedEvent) ApiResult {
	return s.c.SendCommand(ctx, slavep.LedProgramLoad, slavep.EncodeLedProgramLoad(addr, progID, events))
}

func (s *SlaveApi) LedRunProgram(ctx context.Context, addr, progID, flags uint8) ApiResult {
	return s.c.SendCommand(ctx, slavep.LedProgramRun, slavep.EncodeLedRunProgram(addr, progID, flags))
}

func (s *SlaveApi) LedStopProgram(ctx context.Context, addr uint8) ApiResult {
	return s.c.SendCommand(ctx, slavep.LedProgramStop, []byte{addr})
}

func (s *SlaveApi) LedRestartProgram(ctx context.Context, addr uint8) ApiResult {
	return s.c.SendCommand(ctx, slavep.LedProgramRestart, []byte{addr})
}

func (s *SlaveApi) LedResetChannel(ctx context.Context, addr uint8) ApiResult {
	return s.c.SendCommand(ctx, slavep.LedResetChannel, []byte{addr})
}

func (s *SlaveApi) LedEnableChannel(ctx context.Context, addr uint8, enabled bool) ApiResult {
	en := uint8(0)
	if enabled {
		en = 1
	}
	return s.c.SendCommand(ctx, slavep.LedEnableChannel, []byte{addr, en})
}

func (s *SlaveApi) LedSetMasterBrightness(ctx context.Context, pct uint8) ApiResult {
	if pct > 100 {
		pct = 100
	}
	return s.c.SendCommand(ctx, slavep.LedSetMasterBri, []byte{pct})
}

func (s *SlaveApi) LedQuery(ctx context.Context, addr uint8) (slavep.LedQueryResponse, ApiResult) {
	resp, ar := s.c.SendQuery(ctx, slavep.LedQuery, []byte{addr}, slavep.LedQueryResp)
	if !ar.OK {
		return slavep.LedQueryResponse{}, ar
	}
	out, err := slavep.DecodeLedQueryResp(resp.Payload)
	if err != nil {
		return slavep.LedQueryResponse{}, FailF("decode LED_QUERY_RESP: %v", err)
	}
	return out, OK()
}

// ── ApiResult helpers ────────────────────────────────────────────────

// (The actual ApiResult type is defined in api/result.go; OK / FailF
// are existing helpers shared with the per-board APIs.)

func (s *SlaveApi) String() string {
	return fmt.Sprintf("SlaveApi(connection=%v)", s.c)
}
