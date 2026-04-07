package firmware

import (
	"fmt"
	"time"

	"scalefx/protocol"
	"scalefx/protocol/core"
)

// ─── Post-Flash Verification ───
// Connects to the device after flash, sends INIT, and parses the INIT_READY
// response to confirm the firmware version and build number.

// DeviceInfo holds verified device information from INIT_READY response.
type DeviceInfo struct {
	Name           string
	Version        string
	Platform       string
	Build          uint32
	CPUMHz         uint32
	FreeRAM        uint32
	ControllerType string
}

// VerifyDevice connects to the freshly-flashed device and verifies the firmware.
func VerifyDevice(opts *Options, ctrl Controller) error {
	port := opts.Port
	var err error

	if port == "" {
		// Wait for the device to reappear
		opts.info("Waiting for device to appear...")
		port, err = waitForPort(opts, ctrl, 15*time.Second)
		if err != nil {
			return fmt.Errorf("device not found: %w", err)
		}
		opts.info("Device appeared on %s", port)
	}

	// Try to connect and send INIT
	var lastErr error
	maxRetries := 4
	if ctrl.IsESP32() {
		maxRetries = 6 // ESP32 takes longer to boot
	}

	for attempt := 1; attempt <= maxRetries; attempt++ {
		opts.info("Verification attempt %d/%d...", attempt, maxRetries)

		info, err := tryVerify(port, ctrl)
		if err != nil {
			lastErr = err
			opts.warn("Attempt %d: %s", attempt, err)
			time.Sleep(2 * time.Second)
			continue
		}

		// Success
		opts.ok("Device verified: %s v%s (build %d)", info.Name, info.Version, info.Build)
		opts.info("Platform: %s @ %dMHz, RAM: %d bytes free", info.Platform, info.CPUMHz, info.FreeRAM)
		return nil
	}

	return fmt.Errorf("verification failed after %d attempts: %w", maxRetries, lastErr)
}

// tryVerify makes a single verification attempt: connect → INIT → parse INIT_READY.
func tryVerify(port string, ctrl Controller) (*DeviceInfo, error) {
	conn := protocol.NewConnection(port, protocol.DefaultBaud, false)

	if err := conn.Connect(); err != nil {
		return nil, fmt.Errorf("cannot connect: %w", err)
	}
	defer conn.Close()

	// Send INIT and wait for INIT_READY
	initPkt := core.CmdInit()
	resp, err := conn.SendAndWait(initPkt)
	if err != nil {
		return nil, fmt.Errorf("INIT failed: %w", err)
	}

	if resp == nil {
		return nil, fmt.Errorf("no response to INIT")
	}

	if !resp.IsInitReady() {
		return nil, fmt.Errorf("unexpected response: 0x%02X (expected INIT_READY 0xF3)", resp.PacketType)
	}

	// Parse the INIT_READY payload
	info := parseInitReady(resp.Payload)
	if info == nil {
		return nil, fmt.Errorf("cannot parse INIT_READY payload (%d bytes)", len(resp.Payload))
	}

	return info, nil
}

// parseInitReady parses INIT_READY/IDENTIFY payload.
// Format: [nameLen:u8][name][verLen:u8][ver][platLen:u8][plat][cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]
func parseInitReady(payload []byte) *DeviceInfo {
	if len(payload) < 3 {
		return nil
	}

	info := &DeviceInfo{}
	offset := 0

	nameLen := int(payload[offset])
	offset++
	if offset+nameLen > len(payload) {
		return nil
	}
	info.Name = string(payload[offset : offset+nameLen])
	offset += nameLen

	if offset >= len(payload) {
		return nil
	}
	verLen := int(payload[offset])
	offset++
	if offset+verLen > len(payload) {
		return nil
	}
	info.Version = string(payload[offset : offset+verLen])
	offset += verLen

	if offset >= len(payload) {
		return nil
	}
	platLen := int(payload[offset])
	offset++
	if offset+platLen > len(payload) {
		return nil
	}
	info.Platform = string(payload[offset : offset+platLen])
	offset += platLen

	if offset+12 > len(payload) {
		return nil
	}
	info.CPUMHz = protocol.ReadU32LE(payload, offset)
	offset += 4
	info.FreeRAM = protocol.ReadU32LE(payload, offset)
	offset += 4
	info.Build = protocol.ReadU32LE(payload, offset)

	info.ControllerType = core.DetectControllerType(info.Name)
	return info
}

// waitForPort polls for a known serial port to appear.
func waitForPort(opts *Options, ctrl Controller, timeout time.Duration) (string, error) {
	deadline := time.Now().Add(timeout)

	for time.Now().Before(deadline) {
		if ctrl.IsESP32() {
			port, err := DetectESP32Port()
			if err == nil {
				return port, nil
			}
		} else {
			port, err := DetectPicoPort()
			if err == nil {
				return port, nil
			}
		}
		time.Sleep(1 * time.Second)
	}

	return "", fmt.Errorf("timed out waiting for device after %v", timeout)
}
