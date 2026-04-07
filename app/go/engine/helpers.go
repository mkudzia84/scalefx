package engine

// ScaleFX Engine - Shared Helpers
// DRY utilities for arg parsing, guards, and common command patterns.

import (
	"fmt"
	"scalefx/api"
	"strconv"
	"strings"
)

// ─── Parsing Helpers ───

// Atoi converts a string to int, returning 0 on error.
func Atoi(s string) int {
	v, _ := strconv.Atoi(s)
	return v
}

// ParseBool parses typical CLI boolean values (on/off/1/0/true/false).
func ParseBool(s string) bool {
	switch strings.ToLower(s) {
	case "on", "1", "true", "yes":
		return true
	}
	return false
}

// OnOff returns "ON" or "OFF" for display.
func OnOff(b bool) string {
	if b {
		return "ON"
	}
	return "OFF"
}

// ─── Guard Helpers ───

// requireArgs validates minimum arg count, printing usage on failure.
func (e *Engine) requireArgs(args []string, min int, usage string) bool {
	if len(args) < min {
		e.Out.Error("Usage: %s", usage)
		return false
	}
	return true
}

// requireConn checks for an active serial connection.
func (e *Engine) requireConn() bool {
	if e.Conn == nil {
		e.Out.Error("Not connected")
		return false
	}
	return true
}

// ─── API Presentation Helpers ───

// ack prints the result of an ACK-based API call.
func (e *Engine) ack(r api.ApiResult, msg string) {
	if r.Response != nil {
		e.PrintACKResult(r.Response, msg)
	} else {
		e.Out.Error("%s", r.Error)
	}
}

// query prints the result of a query-based API call using the given parser.
func (e *Engine) query(r api.ApiResult, parser func([]byte)) {
	if r.OK && r.Response != nil {
		parser(r.Response.Payload)
	} else if r.Error != "" {
		e.Out.Error("%s", r.Error)
	}
}

// ─── Shared Command Patterns (Strategy Pattern) ───

// servoSet handles "xxx.servo set <id> <pulse_us>" for any controller.
func (e *Engine) servoSet(args []string, usage string, action func(byte, uint16) api.ApiResult) {
	if len(args) < 3 || strings.ToLower(args[0]) != "set" {
		e.Out.Error("Usage: %s", usage)
		return
	}
	id, pulse := Atoi(args[1]), Atoi(args[2])
	e.ack(action(byte(id), uint16(pulse)), fmt.Sprintf("Servo %d → %dµs", id, pulse))
}

// servoConfig handles "xxx.servo.config <id> <min> <max> [spd] [acc] [dec]" for any controller.
func (e *Engine) servoConfig(args []string, usage string, action func(byte, uint16, uint16, uint16, uint16, uint16) api.ApiResult) {
	if !e.requireArgs(args, 3, usage) {
		return
	}
	id, min, max := Atoi(args[0]), Atoi(args[1]), Atoi(args[2])
	speed, accel, decel := 4000, 8000, 8000
	if len(args) > 3 {
		speed = Atoi(args[3])
	}
	if len(args) > 4 {
		accel = Atoi(args[4])
	}
	if len(args) > 5 {
		decel = Atoi(args[5])
	}
	e.ack(action(byte(id), uint16(min), uint16(max), uint16(speed), uint16(accel), uint16(decel)),
		fmt.Sprintf("Servo %d: %d-%dµs, speed=%d", id, min, max, speed))
}
