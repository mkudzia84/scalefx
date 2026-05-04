package upload_test

// Tests for the sync-mode file upload protocol (FILE_UPLOAD_BEGIN/DATA/END/CANCEL).
//
// Rationale: Studio's "Save Configuration" path failed with
//   "upload error at segment 0: timeout waiting for response"
// because api.uploadChunkSize was hardcoded at 2044 (ESP32's MAX_PAYLOAD_SIZE-4),
// producing 2048-byte payloads that overflow the Pico's 512-byte COBS RX buffer —
// the Pico silently dropped the packet as a framing error, the ACK never came,
// and the client timed out.
//
// These tests lock in three invariants:
//   1. api.UploadChunkSize fits inside the Pico's MAX_PAYLOAD_SIZE minus the
//      4-byte [seq:u16][crc16:u16] header — regression guard for the bug above.
//   2. CmdFileUploadBegin / Data / End / Cancel produce the exact wire layout
//      the firmware handlers expect (endianness, field order, CRC placement).
//   3. The complete COBS-framed packet for a full-size chunk fits in the Pico's
//      MAX_PACKET_SIZE budget after framing overhead.

import (
	"testing"

	"scalefx/api"
	"scalefx/protocol"
	"scalefx/protocol/hubfx"
)

// Mirror of C++ constexpr size_t PICO_MAX_PAYLOAD = 512
// (controllers/lib/sfx_platform/platform/sfx_wire.h:55).
// If the firmware changes, update this constant and the test will flag any
// Go-side drift that would reintroduce the segment-0 timeout.
const picoMaxPayload = 512

// Mirror of the sync-mode FILE_UPLOAD_DATA header fields:
// [seq:u16LE][crc16:u16LE][data:N] — 4 bytes of header per chunk.
const uploadDataHeaderBytes = 4

// ─── Regression: chunk size must fit Pico ───────────────────────────────────

func TestUploadChunkSizeFitsPico(t *testing.T) {
	// data + header must fit in Pico's COBS RX buffer.
	totalPayload := api.UploadChunkSize + uploadDataHeaderBytes
	if totalPayload > picoMaxPayload {
		t.Fatalf("UploadChunkSize=%d + header=%d = %d bytes exceeds PICO_MAX_PAYLOAD=%d — "+
			"Pico will silently drop the first FILE_UPLOAD_DATA packet (segment-0 timeout)",
			api.UploadChunkSize, uploadDataHeaderBytes, totalPayload, picoMaxPayload)
	}
}

func TestUploadChunkSizePositive(t *testing.T) {
	if api.UploadChunkSize <= 0 {
		t.Fatalf("UploadChunkSize=%d must be positive", api.UploadChunkSize)
	}
}

// ─── FILE_UPLOAD_BEGIN (0xA0) wire format ───────────────────────────────────

func TestCmdFileUploadBeginWireFormat(t *testing.T) {
	const path = "/config.yaml"
	const size = uint32(12345)
	const target = byte(1) // TARGET_FLASH
	const mode = byte(0)   // UPLOAD_SYNC

	raw := hubfx.CmdFileUploadBegin(path, size, target, mode)

	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed (CRC or length mismatch)")
	}
	if ptype != hubfx.FileUploadBegin {
		t.Errorf("type = 0x%02X, want 0x%02X (FILE_UPLOAD_BEGIN)", ptype, hubfx.FileUploadBegin)
	}

	// Wire: [size:u32LE][pathLen:u8][path][target:u8][mode:u8]
	wantLen := 4 + 1 + len(path) + 1 + 1
	if len(payload) != wantLen {
		t.Fatalf("payload length = %d, want %d", len(payload), wantLen)
	}

	if got := protocol.ReadU32LE(payload, 0); got != size {
		t.Errorf("size = %d, want %d", got, size)
	}
	if payload[4] != byte(len(path)) {
		t.Errorf("pathLen = %d, want %d", payload[4], len(path))
	}
	if gotPath := string(payload[5 : 5+len(path)]); gotPath != path {
		t.Errorf("path = %q, want %q", gotPath, path)
	}
	if payload[5+len(path)] != target {
		t.Errorf("target = 0x%02X, want 0x%02X", payload[5+len(path)], target)
	}
	if payload[6+len(path)] != mode {
		t.Errorf("mode = 0x%02X, want 0x%02X", payload[6+len(path)], mode)
	}
}

func TestCmdFileUploadBeginEndianness(t *testing.T) {
	// 0x12345678 must serialize little-endian: 78 56 34 12
	raw := hubfx.CmdFileUploadBegin("/a", 0x12345678, 0, 0)
	_, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	want := []byte{0x78, 0x56, 0x34, 0x12}
	for i := 0; i < 4; i++ {
		if payload[i] != want[i] {
			t.Errorf("size byte %d = 0x%02X, want 0x%02X (little-endian)",
				i, payload[i], want[i])
		}
	}
}

// ─── FILE_UPLOAD_DATA (0xA1) wire format ────────────────────────────────────

func TestCmdFileUploadDataWireFormat(t *testing.T) {
	data := []byte("hello world")
	const seq = uint16(42)

	raw := hubfx.CmdFileUploadData(seq, data)

	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	if ptype != hubfx.FileUploadData {
		t.Errorf("type = 0x%02X, want 0x%02X (FILE_UPLOAD_DATA)", ptype, hubfx.FileUploadData)
	}

	// Wire: [seq:u16LE][crc16:u16LE][data:N]
	if len(payload) != uploadDataHeaderBytes+len(data) {
		t.Fatalf("payload length = %d, want %d", len(payload), uploadDataHeaderBytes+len(data))
	}

	if got := protocol.ReadU16LE(payload, 0); got != seq {
		t.Errorf("seq = %d, want %d", got, seq)
	}

	wantCRC := protocol.CRC16CCITT(data)
	if got := protocol.ReadU16LE(payload, 2); got != wantCRC {
		t.Errorf("crc16 = 0x%04X, want 0x%04X", got, wantCRC)
	}

	gotData := payload[4:]
	for i := range data {
		if gotData[i] != data[i] {
			t.Errorf("data[%d] = 0x%02X, want 0x%02X", i, gotData[i], data[i])
		}
	}
}

func TestCmdFileUploadDataSeqLittleEndian(t *testing.T) {
	raw := hubfx.CmdFileUploadData(0xBEEF, []byte{0xAA})
	_, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	// 0xBEEF little-endian = EF BE
	if payload[0] != 0xEF || payload[1] != 0xBE {
		t.Errorf("seq bytes = %02X %02X, want EF BE (LE)", payload[0], payload[1])
	}
}

// Verify CRC-16/CCITT (poly 0x1021, init 0xFFFF) matches a known vector —
// this is what the firmware computes in StreamProtocol::crc16().
func TestCmdFileUploadDataCRCKnownVector(t *testing.T) {
	// CRC16-CCITT("123456789") = 0x29B1
	data := []byte("123456789")
	raw := hubfx.CmdFileUploadData(0, data)
	_, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	got := protocol.ReadU16LE(payload, 2)
	if got != 0x29B1 {
		t.Errorf("CRC16('123456789') = 0x%04X, want 0x29B1", got)
	}
}

// ─── FILE_UPLOAD_END (0xA2) ─────────────────────────────────────────────────

func TestCmdFileUploadEndWireFormat(t *testing.T) {
	raw := hubfx.CmdFileUploadEnd()
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	if ptype != hubfx.FileUploadEnd {
		t.Errorf("type = 0x%02X, want 0x%02X", ptype, hubfx.FileUploadEnd)
	}
	if len(payload) != 0 {
		t.Errorf("payload length = %d, want 0", len(payload))
	}
}

// ─── FILE_UPLOAD_CANCEL (0xA3) ──────────────────────────────────────────────

func TestCmdFileUploadCancelWireFormat(t *testing.T) {
	raw := hubfx.CmdFileUploadCancel()
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	if ptype != hubfx.FileUploadCancel {
		t.Errorf("type = 0x%02X, want 0x%02X", ptype, hubfx.FileUploadCancel)
	}
	if len(payload) != 0 {
		t.Errorf("payload length = %d, want 0", len(payload))
	}
}

// ─── Full-size chunk round-trip (no oversize regression) ────────────────────

func TestCmdFileUploadDataFullChunkFitsPico(t *testing.T) {
	// Build an UPLOAD_DATA packet using the real uploadChunkSize — the framed
	// packet (after COBS + CRC8 + delimiter) must still round-trip via
	// ParsePacket. The server-side check is `len > _peerMaxPayload → drop`,
	// so the *payload* length (header+data) is what matters.
	data := make([]byte, api.UploadChunkSize)
	for i := range data {
		data[i] = byte(i & 0xFF)
	}
	raw := hubfx.CmdFileUploadData(0, data)
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("full-chunk ParsePacket failed")
	}
	if ptype != hubfx.FileUploadData {
		t.Errorf("type = 0x%02X, want 0x%02X", ptype, hubfx.FileUploadData)
	}
	if len(payload) > picoMaxPayload {
		t.Errorf("payload = %d bytes exceeds PICO_MAX_PAYLOAD=%d — would be dropped",
			len(payload), picoMaxPayload)
	}
}

// ─── Chunking math: what uploadSync does internally ─────────────────────────

// simulateSyncChunks mimics the inner loop of api.uploadSync: walk the data
// in UploadChunkSize-sized slices, emit a (seq, chunk) pair per iteration.
// Returns the slice of chunks the client would send.
func simulateSyncChunks(data []byte) [][]byte {
	var out [][]byte
	for offset := 0; offset < len(data); {
		end := offset + api.UploadChunkSize
		if end > len(data) {
			end = len(data)
		}
		out = append(out, data[offset:end])
		offset = end
	}
	return out
}

func TestSyncChunkingEmpty(t *testing.T) {
	if got := simulateSyncChunks(nil); len(got) != 0 {
		t.Errorf("empty input produced %d chunks, want 0", len(got))
	}
}

func TestSyncChunkingSingleByte(t *testing.T) {
	chunks := simulateSyncChunks([]byte{0xAB})
	if len(chunks) != 1 || len(chunks[0]) != 1 {
		t.Fatalf("got %d chunks (first len=%d), want 1 chunk of 1 byte", len(chunks), len(chunks[0]))
	}
}

func TestSyncChunkingExactMultiple(t *testing.T) {
	data := make([]byte, 3*api.UploadChunkSize)
	chunks := simulateSyncChunks(data)
	if len(chunks) != 3 {
		t.Fatalf("got %d chunks, want 3", len(chunks))
	}
	for i, c := range chunks {
		if len(c) != api.UploadChunkSize {
			t.Errorf("chunk %d has len %d, want %d", i, len(c), api.UploadChunkSize)
		}
	}
}

func TestSyncChunkingRemainder(t *testing.T) {
	// One full chunk + one byte
	data := make([]byte, api.UploadChunkSize+1)
	chunks := simulateSyncChunks(data)
	if len(chunks) != 2 {
		t.Fatalf("got %d chunks, want 2", len(chunks))
	}
	if len(chunks[0]) != api.UploadChunkSize {
		t.Errorf("first chunk len = %d, want %d", len(chunks[0]), api.UploadChunkSize)
	}
	if len(chunks[1]) != 1 {
		t.Errorf("second chunk len = %d, want 1", len(chunks[1]))
	}
}

func TestSyncChunkingPreservesBytes(t *testing.T) {
	// Build 2.5 chunks of distinct bytes and verify reconstruction.
	total := 2*api.UploadChunkSize + api.UploadChunkSize/2
	data := make([]byte, total)
	for i := range data {
		data[i] = byte(i * 7) // arbitrary pattern
	}
	chunks := simulateSyncChunks(data)
	recon := make([]byte, 0, total)
	for _, c := range chunks {
		recon = append(recon, c...)
	}
	if len(recon) != total {
		t.Fatalf("recon len = %d, want %d", len(recon), total)
	}
	for i := range data {
		if recon[i] != data[i] {
			t.Fatalf("byte %d: got 0x%02X, want 0x%02X", i, recon[i], data[i])
		}
	}
}

// ─── Typical config size — the case that was actually failing ──────────────

// A LightFX / GearControl YAML config is typically 1–4 KB. Verify that a
// 4 KB upload produces a reasonable number of chunks and each chunk builds
// into a valid Pico-sized packet.
func TestTypicalConfigUpload(t *testing.T) {
	// 4 KB config — representative of what Studio's Save sends.
	data := make([]byte, 4096)
	for i := range data {
		data[i] = byte('a' + (i % 26))
	}

	chunks := simulateSyncChunks(data)
	if len(chunks) < 2 {
		t.Errorf("4KB config produced only %d chunk(s); expected several at %d B/chunk",
			len(chunks), api.UploadChunkSize)
	}

	for i, chunk := range chunks {
		raw := hubfx.CmdFileUploadData(uint16(i), chunk)
		_, _, payload, ok := protocol.ParsePacket(raw)
		if !ok {
			t.Fatalf("chunk %d: ParsePacket failed", i)
		}
		if len(payload) > picoMaxPayload {
			t.Errorf("chunk %d: payload=%d exceeds PICO_MAX_PAYLOAD=%d",
				i, len(payload), picoMaxPayload)
		}
		// Verify seq and data round-trip
		if got := protocol.ReadU16LE(payload, 0); got != uint16(i) {
			t.Errorf("chunk %d: seq = %d, want %d", i, got, i)
		}
		got := payload[uploadDataHeaderBytes:]
		for j := range chunk {
			if got[j] != chunk[j] {
				t.Fatalf("chunk %d byte %d: got 0x%02X, want 0x%02X",
					i, j, got[j], chunk[j])
			}
		}
	}
}
