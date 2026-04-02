package main

// ScaleFX CLI - Shared Helpers
// DRY utilities for arg parsing, guards, and common command patterns.

import (
	"fmt"
	"strconv"
	"strings"
)

// ─── Parsing Helpers ───

// atoi converts a string to int, returning 0 on error.
// Safe for CLI use — the device validates and NACKs bad values.
func atoi(s string) int {
	v, _ := strconv.Atoi(s)
	return v
}

// parseBool parses typical CLI boolean values (on/off/1/0/true/false).
func parseBool(s string) bool {
	switch strings.ToLower(s) {
	case "on", "1", "true", "yes":
		return true
	}
	return false
}

// onOff returns "ON" or "OFF" for display.
func onOff(b bool) string {
	if b {
		return "ON"
	}
	return "OFF"
}

// ─── Guard Helpers ───

// requireArgs validates minimum arg count, printing usage on failure.
func requireArgs(args []string, min int, usage string) bool {
	if len(args) < min {
		PrintError("Usage: %s", usage)
		return false
	}
	return true
}

// requireConn checks for an active serial connection.
func (c *CLI) requireConn() bool {
	if c.conn == nil {
		PrintError("Not connected")
		return false
	}
	return true
}

// ─── Shared Command Patterns (Strategy Pattern) ───
// These factor out identical handler logic that appears in multiple controllers.
// The build function is the "strategy" — different per controller, same skeleton.

// servoSet handles "xxx.servo set <id> <pulse_us>" for any controller.
func (c *CLI) servoSet(args []string, usage string, build func(byte, uint16) []byte) {
	if len(args) < 3 || strings.ToLower(args[0]) != "set" {
		PrintError("Usage: %s", usage)
		return
	}
	id, pulse := atoi(args[1]), atoi(args[2])
	c.sendACK(build(byte(id), uint16(pulse)), fmt.Sprintf("Servo %d → %dµs", id, pulse))
}

// servoConfig handles "xxx.servo.config <id> <min> <max> [spd] [acc] [dec]" for any controller.
func (c *CLI) servoConfig(args []string, usage string, build func(byte, uint16, uint16, uint16, uint16, uint16) []byte) {
	if !requireArgs(args, 3, usage) {
		return
	}
	id, min, max := atoi(args[0]), atoi(args[1]), atoi(args[2])
	speed, accel, decel := 4000, 8000, 8000
	if len(args) > 3 {
		speed = atoi(args[3])
	}
	if len(args) > 4 {
		accel = atoi(args[4])
	}
	if len(args) > 5 {
		decel = atoi(args[5])
	}
	c.sendACK(build(byte(id), uint16(min), uint16(max), uint16(speed), uint16(accel), uint16(decel)),
		fmt.Sprintf("Servo %d: %d-%dµs, speed=%d", id, min, max, speed))
}
