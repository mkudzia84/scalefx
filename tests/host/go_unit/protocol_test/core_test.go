package protocol_test

// Tests for core protocol constants, command builders, and STATUS parsing.
// Mirrors the C++ protocol definitions in serial/core/core.h.

import (
	"scalefx/protocol"
	"scalefx/protocol/core"
	"testing"
)

// ─── Board State Constants ───

func TestBoardStateConstants(t *testing.T) {
	// Must match BoardState namespace in core.h
	tests := []struct {
		name  string
		value byte
		label string
	}{
		{"IDLE", core.BoardStateIdle, "IDLE"},
		{"STANDALONE", core.BoardStateStandalone, "STANDALONE"},
		{"SLAVE", core.BoardStateSlave, "SLAVE"},
		{"DIRECT", core.BoardStateDirect, "DIRECT"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := core.BoardStateName(tt.value)
			if got != tt.label {
				t.Errorf("BoardStateName(0x%02X) = %q, want %q", tt.value, got, tt.label)
			}
		})
	}
}

func TestBoardStateValues(t *testing.T) {
	// Wire values must match C++ header exactly
	if core.BoardStateIdle != 0x00 {
		t.Errorf("BoardStateIdle = 0x%02X, want 0x00", core.BoardStateIdle)
	}
	if core.BoardStateStandalone != 0x01 {
		t.Errorf("BoardStateStandalone = 0x%02X, want 0x01", core.BoardStateStandalone)
	}
	if core.BoardStateSlave != 0x02 {
		t.Errorf("BoardStateSlave = 0x%02X, want 0x02", core.BoardStateSlave)
	}
	if core.BoardStateDirect != 0x03 {
		t.Errorf("BoardStateDirect = 0x%02X, want 0x03", core.BoardStateDirect)
	}
}

func TestBoardStateUnknown(t *testing.T) {
	name := core.BoardStateName(0xFF)
	if name == "IDLE" || name == "STANDALONE" || name == "SLAVE" || name == "DIRECT" {
		t.Errorf("BoardStateName(0xFF) should not match a known state, got %q", name)
	}
}

// ─── Init Mode Constants ───

func TestInitModeConstants(t *testing.T) {
	// Wire values must match C++ header
	if core.InitModeSlave != 0x00 {
		t.Errorf("InitModeSlave = 0x%02X, want 0x00", core.InitModeSlave)
	}
	if core.InitModeDirect != 0x01 {
		t.Errorf("InitModeDirect = 0x%02X, want 0x01", core.InitModeDirect)
	}
}

func TestInitModeNames(t *testing.T) {
	tests := []struct {
		mode byte
		name string
	}{
		{core.InitModeSlave, "SLAVE"},
		{core.InitModeDirect, "DIRECT"},
	}
	for _, tt := range tests {
		got := core.InitModeName(tt.mode)
		if got != tt.name {
			t.Errorf("InitModeName(0x%02X) = %q, want %q", tt.mode, got, tt.name)
		}
	}
}

// ─── Init Command Builders ───

func TestCmdInit(t *testing.T) {
	pkt := core.CmdInit()
	if len(pkt) == 0 {
		t.Fatal("CmdInit() returned empty packet")
	}
	// Verify packet type after COBS decode
	decoded := protocol.COBSDecode(pkt[:len(pkt)-1]) // strip frame delimiter
	if decoded == nil || len(decoded) < 5 {
		t.Fatal("CmdInit() COBS decode failed or too short")
	}
	if protocol.PacketType(decoded[0]) != core.Init {
		t.Errorf("CmdInit() type = 0x%02X, want 0x%02X (INIT)", decoded[0], core.Init)
	}
}

func TestCmdInitMode(t *testing.T) {
	pkt := core.CmdInitMode(core.InitModeDirect, core.InitFlagVerbose)
	if len(pkt) == 0 {
		t.Fatal("CmdInitMode() returned empty packet")
	}
	decoded := protocol.COBSDecode(pkt[:len(pkt)-1])
	if decoded == nil || len(decoded) < 5 {
		t.Fatal("CmdInitMode() COBS decode failed or too short")
	}
	if protocol.PacketType(decoded[0]) != core.Init {
		t.Errorf("CmdInitMode() type = 0x%02X, want 0x%02X (INIT)", decoded[0], core.Init)
	}
}

// ─── Core Packet Type Constants ───

func TestCorePacketTypes(t *testing.T) {
	// Verify core packet type values match C++ header
	tests := []struct {
		name  string
		value protocol.PacketType
		want  byte
	}{
		{"INIT", core.Init, 0xF0},
		{"SHUTDOWN", core.Shutdown, 0xF1},
		{"KEEPALIVE", core.Keepalive, 0xF2},
		{"INIT_READY", core.InitReady, 0xF3},
		{"STATUS", core.Status, 0xF4},
		{"ERROR", core.Error, 0xF5},
		{"ACK", core.Ack, 0xF6},
		{"NACK", core.Nack, 0xF7},
		{"REBOOT", core.Reboot, 0xF8},
		{"BOOTSEL", core.Bootsel, 0xF9},
		{"STATUS_REQ", core.StatusReq, 0xFA},
		{"I2C_SCAN", core.I2cScan, 0xFB},
		{"I2C_SCAN_RESULT", core.I2cScanRes, 0xFC},
		{"LOG_MESSAGE", core.LogMessage, 0xFD},
		{"IDENTIFY", core.Identify, 0xFE},
		{"DIAG_HISTORY", core.DiagHistory, 0xFF},
		{"STATUS_UPDATE", core.StatusUpdate, 0xEF},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if byte(tt.value) != tt.want {
				t.Errorf("%s = 0x%02X, want 0x%02X", tt.name, byte(tt.value), tt.want)
			}
		})
	}
}

// ─── Error Code Constants ───

func TestGenericErrorCodes(t *testing.T) {
	tests := []struct {
		name string
		code protocol.ErrorCode
		want byte
	}{
		{"OK", core.ErrOk, 0x00},
		{"UNKNOWN", core.ErrUnknown, 0x01},
		{"NOT_INITIALIZED", core.ErrNotInitialized, 0x02},
		{"INVALID_COMMAND", core.ErrInvalidCommand, 0x03},
		{"MISSING_PARAM", core.ErrMissingParam, 0x04},
		{"BUSY", core.ErrBusy, 0x05},
		{"NOT_SUPPORTED", core.ErrNotSupported, 0x06},
		{"INTERNAL", core.ErrInternal, 0xF0},
		{"TIMEOUT", core.ErrTimeout, 0xF1},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if byte(tt.code) != tt.want {
				t.Errorf("%s = 0x%02X, want 0x%02X", tt.name, byte(tt.code), tt.want)
			}
		})
	}
}

// ─── Controller Type Detection ───

func TestDetectControllerType(t *testing.T) {
	tests := []struct {
		name       string
		deviceName string
		want       string
	}{
		{"GunFX", "GunFX-A1B2", core.CtrlGunFX},
		{"LightFX", "LightFX-C3D4", core.CtrlLightFX},
		{"GearControl", "GearControl-E5F6", core.CtrlGearControl},
		{"HubFX", "HubFX-0102", core.CtrlHubFX},
		{"NoOp", "NoOp-ABCD", core.CtrlNoOp},
		{"Unknown", "FooBar-1234", ""},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := core.DetectControllerType(tt.deviceName)
			if got != tt.want {
				t.Errorf("DetectControllerType(%q) = %q, want %q", tt.deviceName, got, tt.want)
			}
		})
	}
}

// ─── STATUS Payload Parsing Helpers ───

// buildStatusPayloadV2 builds a 22-byte core header with board state and flags.
func buildStatusPayloadV2(counter, uptime, freeRam, lastAct, keepalives uint32, boardState, initFlags byte) []byte {
	buf := make([]byte, 0, 22)
	buf = append(buf, protocol.U32LE(counter)...)
	buf = append(buf, protocol.U32LE(uptime)...)
	buf = append(buf, protocol.U32LE(freeRam)...)
	buf = append(buf, protocol.U32LE(lastAct)...)
	buf = append(buf, protocol.U32LE(keepalives)...)
	buf = append(buf, boardState, initFlags)
	return buf
}

// buildStatusPayloadLegacy builds a legacy 20-byte core header (no board state).
func buildStatusPayloadLegacy(counter, uptime, freeRam, lastAct, keepalives uint32) []byte {
	buf := make([]byte, 0, 20)
	buf = append(buf, protocol.U32LE(counter)...)
	buf = append(buf, protocol.U32LE(uptime)...)
	buf = append(buf, protocol.U32LE(freeRam)...)
	buf = append(buf, protocol.U32LE(lastAct)...)
	buf = append(buf, protocol.U32LE(keepalives)...)
	return buf
}

func TestStatusPayloadV2Layout(t *testing.T) {
	payload := buildStatusPayloadV2(42, 60000, 200000, 500, 10,
		core.BoardStateSlave, core.InitFlagVerbose)
	if len(payload) != 22 {
		t.Fatalf("V2 payload length = %d, want 22", len(payload))
	}

	// Verify fields
	counter := protocol.ReadU32LE(payload, 0)
	uptime := protocol.ReadU32LE(payload, 4)
	freeRam := protocol.ReadU32LE(payload, 8)
	lastAct := protocol.ReadU32LE(payload, 12)
	keepalives := protocol.ReadU32LE(payload, 16)
	boardState := payload[20]
	initFlags := payload[21]

	if counter != 42 {
		t.Errorf("counter = %d, want 42", counter)
	}
	if uptime != 60000 {
		t.Errorf("uptime = %d, want 60000", uptime)
	}
	if freeRam != 200000 {
		t.Errorf("freeRam = %d, want 200000", freeRam)
	}
	if lastAct != 500 {
		t.Errorf("lastAct = %d, want 500", lastAct)
	}
	if keepalives != 10 {
		t.Errorf("keepalives = %d, want 10", keepalives)
	}
	if boardState != core.BoardStateSlave {
		t.Errorf("boardState = 0x%02X, want 0x%02X (SLAVE)", boardState, core.BoardStateSlave)
	}
	if initFlags != core.InitFlagVerbose {
		t.Errorf("initFlags = 0x%02X, want 0x%02X (VERBOSE)", initFlags, core.InitFlagVerbose)
	}
}

func TestStatusPayloadLegacyLayout(t *testing.T) {
	payload := buildStatusPayloadLegacy(1, 5000, 100000, 200, 3)
	if len(payload) != 20 {
		t.Fatalf("Legacy payload length = %d, want 20", len(payload))
	}

	counter := protocol.ReadU32LE(payload, 0)
	uptime := protocol.ReadU32LE(payload, 4)
	freeRam := protocol.ReadU32LE(payload, 8)
	lastAct := protocol.ReadU32LE(payload, 12)
	keepalives := protocol.ReadU32LE(payload, 16)

	if counter != 1 {
		t.Errorf("counter = %d, want 1", counter)
	}
	if uptime != 5000 {
		t.Errorf("uptime = %d, want 5000", uptime)
	}
	if freeRam != 100000 {
		t.Errorf("freeRam = %d, want 100000", freeRam)
	}
	if lastAct != 200 {
		t.Errorf("lastAct = %d, want 200", lastAct)
	}
	if keepalives != 3 {
		t.Errorf("keepalives = %d, want 3", keepalives)
	}
}

// ─── CRC-8 ───

func TestCRC8(t *testing.T) {
	// Empty data
	if protocol.CRC8(nil) != 0x00 {
		t.Errorf("CRC8(nil) != 0x00")
	}

	// Known value: CRC-8 of [0xF0, 0x01, 0x00, 0x00] (INIT packet, tag=1, len=0)
	data := []byte{0xF0, 0x01, 0x00, 0x00}
	crc := protocol.CRC8(data)
	// Verify CRC is deterministic
	crc2 := protocol.CRC8(data)
	if crc != crc2 {
		t.Errorf("CRC8 not deterministic: %02X vs %02X", crc, crc2)
	}
}

// ─── Wire Format Helpers ───

func TestU32LEEndianness(t *testing.T) {
	// 0x12345678 should encode as [0x78, 0x56, 0x34, 0x12] (little-endian)
	buf := protocol.U32LE(0x12345678)
	if len(buf) != 4 {
		t.Fatalf("U32LE length = %d, want 4", len(buf))
	}
	if buf[0] != 0x78 || buf[1] != 0x56 || buf[2] != 0x34 || buf[3] != 0x12 {
		t.Errorf("U32LE(0x12345678) = %02X %02X %02X %02X, want 78 56 34 12",
			buf[0], buf[1], buf[2], buf[3])
	}

	// Round-trip
	val := protocol.ReadU32LE(buf, 0)
	if val != 0x12345678 {
		t.Errorf("ReadU32LE round-trip = 0x%08X, want 0x12345678", val)
	}
}

func TestU16LEEndianness(t *testing.T) {
	buf := []byte{0xCD, 0xAB}
	val := protocol.ReadU16LE(buf, 0)
	if val != 0xABCD {
		t.Errorf("ReadU16LE([0xCD, 0xAB]) = 0x%04X, want 0xABCD", val)
	}
}

// ─── COBS Encode/Decode ───

func TestCOBSRoundTrip(t *testing.T) {
	original := []byte{0x01, 0x00, 0x03, 0x00, 0x05}
	encoded := protocol.COBSEncode(original)
	decoded := protocol.COBSDecode(encoded)
	if decoded == nil {
		t.Fatal("COBSDecode returned nil")
	}
	if len(decoded) != len(original) {
		t.Fatalf("COBS round-trip length: got %d, want %d", len(decoded), len(original))
	}
	for i := range original {
		if decoded[i] != original[i] {
			t.Errorf("COBS round-trip byte %d: got 0x%02X, want 0x%02X", i, decoded[i], original[i])
		}
	}
}

func TestCOBSNoZeros(t *testing.T) {
	// Data with no zeros should encode cleanly
	original := []byte{0x01, 0x02, 0x03}
	encoded := protocol.COBSEncode(original)
	decoded := protocol.COBSDecode(encoded)
	if decoded == nil {
		t.Fatal("COBSDecode returned nil")
	}
	for i := range original {
		if decoded[i] != original[i] {
			t.Errorf("byte %d: got 0x%02X, want 0x%02X", i, decoded[i], original[i])
		}
	}
}
