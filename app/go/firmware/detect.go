package firmware

import (
	"fmt"
	"strings"

	"go.bug.st/serial/enumerator"
)

// ─── USB VID/PID Constants ───

const (
	// Raspberry Pi Pico (RP2040/RP2350) CDC serial
	picoVID = "2E8A"
	picoProductPID = "000A" // Pico SDK CDC

	// Espressif ESP32-S3 USB JTAG/serial
	espVID = "303A"
	espPID = "1001" // ESP32-S3 USB JTAG/serial (CDC)
)

// PortInfo holds detected serial port details.
type PortInfo struct {
	Name        string
	Description string
	VID         string
	PID         string
}

// DetectPicoPort finds the first Raspberry Pi Pico serial port.
func DetectPicoPort() (string, error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return "", fmt.Errorf("cannot enumerate ports: %w", err)
	}

	for _, p := range ports {
		if !p.IsUSB {
			continue
		}
		vid := strings.ToUpper(p.VID)
		if vid == picoVID {
			return p.Name, nil
		}
	}

	return "", fmt.Errorf("no Pico device found (VID %s)", picoVID)
}

// DetectESP32Port finds the first ESP32-S3 serial port.
func DetectESP32Port() (string, error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return "", fmt.Errorf("cannot enumerate ports: %w", err)
	}

	for _, p := range ports {
		if !p.IsUSB {
			continue
		}
		vid := strings.ToUpper(p.VID)
		pid := strings.ToUpper(p.PID)
		if vid == espVID && pid == espPID {
			return p.Name, nil
		}
	}

	return "", fmt.Errorf("no ESP32-S3 device found (VID %s, PID %s)", espVID, espPID)
}

// DetectAnyPort finds the first Pico or ESP32 serial port.
func DetectAnyPort() (string, error) {
	port, err := DetectPicoPort()
	if err == nil {
		return port, nil
	}
	port, err = DetectESP32Port()
	if err == nil {
		return port, nil
	}
	return "", fmt.Errorf("no Pico or ESP32 device found")
}

// ListScaleFXPorts returns all detected ScaleFX-compatible serial ports.
func ListScaleFXPorts() ([]PortInfo, error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return nil, fmt.Errorf("cannot enumerate ports: %w", err)
	}

	var result []PortInfo
	for _, p := range ports {
		if !p.IsUSB {
			continue
		}
		vid := strings.ToUpper(p.VID)
		pid := strings.ToUpper(p.PID)

		if vid == picoVID || (vid == espVID && pid == espPID) {
			result = append(result, PortInfo{
				Name:        p.Name,
				Description: p.Product,
				VID:         vid,
				PID:         pid,
			})
		}
	}

	return result, nil
}
