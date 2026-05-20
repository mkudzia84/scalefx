package main

import (
	"fmt"
	"strconv"
	"strings"

	"scalefx/protocol/ports"
	"scalefx/protocol/storage"
)

// parseU8 reads "0x12", "18", "0o22", "0b00010010" into a byte.
func parseU8(s string) (byte, error) {
	v, err := strconv.ParseUint(strings.TrimPrefix(s, "0x"), 0, 8)
	if err != nil {
		return 0, fmt.Errorf("parse u8 %q: %w", s, err)
	}
	return byte(v), nil
}

// parsePortKind accepts "servo", "pwm", "hbridge", "input" or a u8
// literal; returns the wire kind byte.
func parsePortKind(s string) (byte, error) {
	switch strings.ToLower(s) {
	case "servo":
		return ports.KindServo, nil
	case "pwm":
		return ports.KindPwm, nil
	case "hbridge":
		return ports.KindHBridge, nil
	case "input":
		return ports.KindInput, nil
	}
	return parseU8(s)
}

// parseTarget accepts "sd" or "flash" and returns the wire byte.
func parseTarget(s string) (byte, error) {
	switch strings.ToLower(s) {
	case "sd":
		return storage.TargetSD, nil
	case "flash":
		return storage.TargetFlash, nil
	}
	return 0, fmt.Errorf("target must be sd|flash, got %q", s)
}

// humanBytes renders a byte count as B / KB / MB / GB.  Matches the
// CLAUDE.md "console output size formatting" rule.
func humanBytes(n uint64) string {
	switch {
	case n < 1024:
		return fmt.Sprintf("%d B", n)
	case n < 1024*1024:
		return fmt.Sprintf("%.1f KB", float64(n)/1024)
	case n < 1024*1024*1024:
		return fmt.Sprintf("%.1f MB", float64(n)/(1024*1024))
	default:
		return fmt.Sprintf("%.2f GB", float64(n)/(1024*1024*1024))
	}
}
