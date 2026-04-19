package firmware

import (
	"fmt"
	"strings"
	"time"

	"scalefx/protocol"
	"scalefx/protocol/core"

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

	// WCH CH343 USB-UART bridge (used on HubFX v1 board)
	ch343VID = "1A86"
	ch343PID = "55D3"
)

// isESP32Port returns true if the VID/PID matches any known ESP32-S3 interface.
func isESP32Port(vid, pid string) bool {
	return (vid == espVID && pid == espPID) || (vid == ch343VID && pid == ch343PID)
}

// PortInfo holds detected serial port details.
type PortInfo struct {
	Name        string
	Description string
	VID         string
	PID         string

	// Populated by ProbeDevice / ListScaleFXPortsWithIdentity
	DeviceName     string // e.g. "HubFX-6940", "GunFX-1234"
	DeviceVersion  string // e.g. "0.31.0"
	DeviceBuild    uint32
	DevicePlatform string // e.g. "ESP32-S3", "RP2040"
	ControllerType string // e.g. "hubfx", "gunfx"
	Identified     bool   // true if IDENTIFY succeeded

	// Capabilities advertised by the firmware in the IDENTIFY payload —
	// mirrors core.Cap* bits. 0 means "legacy firmware that pre-dates the
	// field"; clients should fall back to probing in that case.
	Capabilities uint32
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
		if isESP32Port(vid, pid) {
			return p.Name, nil
		}
	}

	return "", fmt.Errorf("no ESP32-S3 device found (VID %s/%s or %s/%s)", espVID, espPID, ch343VID, ch343PID)
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

		if vid == picoVID || isESP32Port(vid, pid) {
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

// ProbeDevice connects to a serial port, sends IDENTIFY, and returns device info.
// This is non-destructive — IDENTIFY doesn't trigger init callbacks or state changes.
// Returns nil DeviceInfo (no error) if the port doesn't respond to IDENTIFY.
func ProbeDevice(portName string) *DeviceInfo {
	conn := protocol.NewConnection(portName, protocol.DefaultBaud, false)

	if err := conn.Connect(); err != nil {
		return nil
	}
	defer conn.Close()

	// Small delay to let the reader goroutine start
	time.Sleep(50 * time.Millisecond)

	// Send IDENTIFY (0xFE) — same payload format as INIT_READY but non-destructive
	identifyPkt := core.CmdIdentify()
	resp, err := conn.SendAndWait(identifyPkt)
	if err != nil {
		return nil
	}
	if resp == nil {
		return nil
	}

	// IDENTIFY response is always packet type 0xFE — see
	// CoreCommandServer::sendIdentify() in the shared firmware library.
	if !resp.IsIdentify() {
		return nil
	}

	return parseInitReady(resp.Payload)
}

// ListScaleFXPortsWithIdentity enumerates ports and probes each with IDENTIFY.
func ListScaleFXPortsWithIdentity() ([]PortInfo, error) {
	ports, err := ListScaleFXPorts()
	if err != nil {
		return nil, err
	}

	for i := range ports {
		info := ProbeDevice(ports[i].Name)
		if info != nil {
			ports[i].DeviceName = info.Name
			ports[i].DeviceVersion = info.Version
			ports[i].DeviceBuild = info.Build
			ports[i].DevicePlatform = info.Platform
			ports[i].ControllerType = info.ControllerType
			ports[i].Capabilities = info.Capabilities
			ports[i].Identified = true
		}
	}

	return ports, nil
}
