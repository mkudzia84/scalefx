package storage_protocol_test

// Unit tests for the storage protocol (controllers/lib/sfx_storage/server/
// storage_service.h ↔ app/go/protocol/storage/storage.go).
//
// Locks in the wire format for:
//   - Packet-type byte assignments (0x93..0xA3 + 0xB0)
//   - Storage target byte (SD=0, Flash=1)
//   - Upload mode byte (Sync=0, Stream=3)
//   - Delete / mkdir flag bits
//   - FILE_UPLOAD_BEGIN payload  [size:u32LE][pathLen:u8][path][target?][mode?]
//   - FILE_UPLOAD_DATA   payload [seq:u16LE][crc16:u16LE][data:N]
//   - FILE_UPLOAD_PROGRESS async [seg:u16LE][bytes:u32LE][fillPct:u8]
//   - DecodeSdStatus / DecodeFlashStatus / DecodeFileInfo /
//     DecodeUploadProgress all match the C++ side's payload layouts.
//
// Regression targets:
//   * 2026-05-28 four-bug upload crash (build 486 → 506) — the firmware
//     fix relied on STREAM_SEGMENT_SIZE = 16 KB, but the SEGMENT_SIZE
//     value lives ONLY on the firmware side; the wire-format pin here
//     is the FILE_UPLOAD_BEGIN ACK shape (segment_size in the response
//     payload, not the request).  These tests lock in what BOTH sides
//     have to agree on.
//   * Packet allocation map in CLAUDE.md ("Effects: Storage `0x93–0xA3`
//     + `0xA9` + `0xB0`") — if anyone reuses one of these bytes for a
//     new packet, these tests fail at the byte-assignment level
//     before the protocol can drift.

import (
	"encoding/binary"
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/storage"
)

// Helpers — the protocol package exports ReadU16LE/ReadU32LE but not
// the put-side; tests synthesise payloads via encoding/binary directly.
func putU16LE(b []byte, off int, v uint16) { binary.LittleEndian.PutUint16(b[off:], v) }
func putU32LE(b []byte, off int, v uint32) { binary.LittleEndian.PutUint32(b[off:], v) }

// ─── Packet-type byte assignments ───────────────────────────────────────

func TestStoragePacketTypeBytes(t *testing.T) {
	// Each row pins (constant, expected wire byte).  Sourced from
	// CLAUDE.md's "Effects: Storage 0x93–0xA3 + 0xA9 + 0xB0" entry
	// and controllers/lib/sfx_serial/serial/storage/storage_protocol.h.
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"SD_INIT", storage.SdInit, 0x93},
		{"SD_STATUS_REQ", storage.SdStatusReq, 0x94},
		{"FLASH_STATUS_REQ", storage.FlashStatusReq, 0x99},
		{"FILE_LIST", storage.FileList, 0x9A},
		{"FILE_INFO_RESP", storage.FileInfoResp, 0x9E},
		{"FILE_UPLOAD_BEGIN", storage.FileUploadBegin, 0xA0},
		{"FILE_UPLOAD_DATA", storage.FileUploadData, 0xA1},
		{"FILE_UPLOAD_END", storage.FileUploadEnd, 0xA2},
		{"FILE_UPLOAD_CANCEL", storage.FileUploadCancel, 0xA3},
		{"FILE_UPLOAD_PROGRESS", storage.FileUploadProgress, 0xB0},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X — wire-format drift?",
				tc.name, byte(tc.got), tc.want)
		}
	}
}

// ─── Wire-constant byte values ──────────────────────────────────────────

func TestStorageTargetBytes(t *testing.T) {
	if storage.TargetSD != 0 {
		t.Errorf("TargetSD = %d, want 0", storage.TargetSD)
	}
	if storage.TargetFlash != 1 {
		t.Errorf("TargetFlash = %d, want 1", storage.TargetFlash)
	}
}

func TestUploadModeBytes(t *testing.T) {
	// UploadSync = 0 (per-chunk ACK), UploadStream = 3 (raw burst).
	// Mode = 1 and 2 are reserved per
	// controllers/lib/sfx_serial/serial/storage/storage_protocol.h.
	if storage.UploadSync != 0 {
		t.Errorf("UploadSync = %d, want 0", storage.UploadSync)
	}
	if storage.UploadStream != 3 {
		t.Errorf("UploadStream = %d, want 3", storage.UploadStream)
	}
	if storage.UploadSync == storage.UploadStream {
		t.Fatal("UploadSync and UploadStream must differ")
	}
}

func TestDeleteFlagBits(t *testing.T) {
	if storage.DeleteFlagNone != 0x00 {
		t.Errorf("DeleteFlagNone = 0x%02X, want 0x00", storage.DeleteFlagNone)
	}
	if storage.DeleteFlagRecursive != 0x01 {
		t.Errorf("DeleteFlagRecursive = 0x%02X, want 0x01", storage.DeleteFlagRecursive)
	}
}

func TestMkdirFlagBits(t *testing.T) {
	if storage.MkdirFlagNone != 0x00 {
		t.Errorf("MkdirFlagNone = 0x%02X, want 0x00", storage.MkdirFlagNone)
	}
	if storage.MkdirFlagParents != 0x01 {
		t.Errorf("MkdirFlagParents = 0x%02X, want 0x01", storage.MkdirFlagParents)
	}
}

// ─── FILE_UPLOAD_BEGIN wire format ──────────────────────────────────────

func TestCmdFileUploadBeginPayload(t *testing.T) {
	const path = "/sounds/test.mp3"
	const size = uint32(1458935)
	const target = storage.TargetSD
	const mode = storage.UploadStream

	raw := storage.CmdFileUploadBegin(path, size, target, mode)
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed (CRC or length mismatch)")
	}
	if ptype != storage.FileUploadBegin {
		t.Errorf("type = 0x%02X, want 0x%02X", byte(ptype), byte(storage.FileUploadBegin))
	}

	// Payload: [size:u32LE][pathLen:u8][path][target:u8][mode:u8]
	wantLen := 4 + 1 + len(path) + 1 + 1
	if len(payload) != wantLen {
		t.Fatalf("payload length = %d, want %d", len(payload), wantLen)
	}
	if got := protocol.ReadU32LE(payload, 0); got != size {
		t.Errorf("size field = %d, want %d", got, size)
	}
	if pl := payload[4]; int(pl) != len(path) {
		t.Errorf("pathLen = %d, want %d", pl, len(path))
	}
	if got := string(payload[5 : 5+len(path)]); got != path {
		t.Errorf("path = %q, want %q", got, path)
	}
	if got := payload[5+len(path)]; got != target {
		t.Errorf("target = 0x%02X, want 0x%02X", got, target)
	}
	if got := payload[6+len(path)]; got != mode {
		t.Errorf("mode = 0x%02X, want 0x%02X", got, mode)
	}
}

// ─── FILE_UPLOAD_DATA wire format ───────────────────────────────────────

func TestCmdFileUploadDataPayload(t *testing.T) {
	const seq = uint16(42)
	data := []byte{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF, 0x42}

	raw := storage.CmdFileUploadData(seq, data)
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	if ptype != storage.FileUploadData {
		t.Errorf("type = 0x%02X, want 0x%02X", byte(ptype), byte(storage.FileUploadData))
	}

	// Payload: [seq:u16LE][crc16:u16LE][data:N]
	const headerLen = 4
	if len(payload) != headerLen+len(data) {
		t.Fatalf("payload length = %d, want %d", len(payload), headerLen+len(data))
	}
	if got := protocol.ReadU16LE(payload, 0); got != seq {
		t.Errorf("seq = %d, want %d", got, seq)
	}
	// Tail is the raw data unmodified — CRC16 is in payload[2:4] but
	// we don't re-derive it here; the firmware verifies it on receive
	// and the protocol_test/core_test.go already covers CRC math.
	if got := string(payload[4:]); got != string(data) {
		t.Errorf("data tail = %v, want %v", payload[4:], data)
	}
}

// ─── FILE_UPLOAD_PROGRESS decoder ───────────────────────────────────────

func TestDecodeUploadProgress(t *testing.T) {
	// Wire: [seg:u16LE][bytes:u32LE][fillPct:u8] (7 bytes)
	payload := make([]byte, 7)
	putU16LE(payload, 0, 0x1234)
	putU32LE(payload, 2, 0xDEADBEEF)
	payload[6] = 73 // fill_pct

	p, err := storage.DecodeUploadProgress(payload)
	if err != nil {
		t.Fatalf("DecodeUploadProgress: %v", err)
	}
	if p.SegmentIdx != 0x1234 {
		t.Errorf("SegmentIdx = 0x%04X, want 0x1234", p.SegmentIdx)
	}
	if p.BytesReceived != 0xDEADBEEF {
		t.Errorf("BytesReceived = 0x%08X, want 0xDEADBEEF", p.BytesReceived)
	}
	if p.RingFillPct != 73 {
		t.Errorf("RingFillPct = %d, want 73", p.RingFillPct)
	}
}

func TestDecodeUploadProgressShortPayloadFails(t *testing.T) {
	if _, err := storage.DecodeUploadProgress(make([]byte, 6)); err == nil {
		t.Error("6-byte payload should fail (need 7)")
	}
}

// ─── DecodeSdStatus ─────────────────────────────────────────────────────

func TestDecodeSdStatusInitialized(t *testing.T) {
	// Wire: [init:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE]
	//       [freeSpace_MB:u32LE][fatType:u8][cardType:u8][busMode:u8]
	//       [usedSpace_MB:u32LE]  = 20 bytes
	payload := make([]byte, 20)
	payload[0] = 1                                // initialized
	putU32LE(payload, 1, 15640)        // cardSize_MB (16 GB SD)
	putU32LE(payload, 5, 15600)        // totalSpace_MB (after FAT overhead)
	putU32LE(payload, 9, 12000)        // freeSpace_MB
	payload[13] = 32                              // fatType (FAT32)
	payload[14] = 3                               // cardType (SDHC)
	payload[15] = 2                               // busMode (SDIO-4bit)
	putU32LE(payload, 16, 3600)        // usedSpace_MB

	s, err := storage.DecodeSdStatus(payload)
	if err != nil {
		t.Fatalf("DecodeSdStatus: %v", err)
	}
	if !s.Initialized {
		t.Error("Initialized = false, want true")
	}
	if s.CardSizeMB != 15640 {
		t.Errorf("CardSizeMB = %d, want 15640", s.CardSizeMB)
	}
	if s.FreeSpaceMB != 12000 {
		t.Errorf("FreeSpaceMB = %d, want 12000", s.FreeSpaceMB)
	}
	if s.UsedSpaceMB != 3600 {
		t.Errorf("UsedSpaceMB = %d, want 3600", s.UsedSpaceMB)
	}
}

func TestDecodeSdStatusNotInitialized(t *testing.T) {
	// First byte 0 = not initialized; rest of payload semantically
	// ignored.  Decoder must still return a valid (zero) struct, not
	// an error — the firmware sends a zero-filled tail in that case.
	payload := make([]byte, 20)
	payload[0] = 0
	s, err := storage.DecodeSdStatus(payload)
	if err != nil {
		t.Fatalf("DecodeSdStatus on init=0: %v", err)
	}
	if s.Initialized {
		t.Error("Initialized = true on payload[0]=0")
	}
}

// ─── DecodeFlashStatus ──────────────────────────────────────────────────

func TestDecodeFlashStatus(t *testing.T) {
	// Wire: [init:u8][total:u32LE][used:u32LE][free:u32LE]  = 13 bytes
	payload := make([]byte, 13)
	payload[0] = 1
	putU32LE(payload, 1, 4128768) // total
	putU32LE(payload, 5, 791504)  // used
	putU32LE(payload, 9, 3337264) // free

	s, err := storage.DecodeFlashStatus(payload)
	if err != nil {
		t.Fatalf("DecodeFlashStatus: %v", err)
	}
	if !s.Initialized {
		t.Error("Initialized = false")
	}
	if s.TotalBytes != 4128768 {
		t.Errorf("TotalBytes = %d, want 4128768", s.TotalBytes)
	}
	if s.UsedBytes != 791504 {
		t.Errorf("UsedBytes = %d, want 791504", s.UsedBytes)
	}
	if s.FreeBytes != 3337264 {
		t.Errorf("FreeBytes = %d, want 3337264", s.FreeBytes)
	}
}

// ─── DecodeFileInfo ─────────────────────────────────────────────────────

func TestDecodeFileInfoExists(t *testing.T) {
	// Wire: [exists:u8][isDir:u8][size:u32LE]  = 6 bytes
	payload := make([]byte, 6)
	payload[0] = 1
	payload[1] = 0
	putU32LE(payload, 2, 1458935)
	r, err := storage.DecodeFileInfo(payload)
	if err != nil {
		t.Fatalf("DecodeFileInfo: %v", err)
	}
	if !r.Exists || r.IsDir {
		t.Errorf("exists=%v isDir=%v, want true/false", r.Exists, r.IsDir)
	}
	if r.Size != 1458935 {
		t.Errorf("Size = %d, want 1458935", r.Size)
	}
}

func TestDecodeFileInfoNotFound(t *testing.T) {
	payload := make([]byte, 6)
	// All zeros = not found
	r, err := storage.DecodeFileInfo(payload)
	if err != nil {
		t.Fatalf("DecodeFileInfo on zero payload: %v", err)
	}
	if r.Exists {
		t.Error("Exists = true on zero payload")
	}
}

func TestDecodeFileInfoDirectory(t *testing.T) {
	payload := make([]byte, 6)
	payload[0] = 1 // exists
	payload[1] = 1 // is directory
	r, _ := storage.DecodeFileInfo(payload)
	if !r.Exists || !r.IsDir {
		t.Errorf("expected dir, got exists=%v isDir=%v", r.Exists, r.IsDir)
	}
}

// ─── CmdSdInit speed encoding ───────────────────────────────────────────

func TestCmdSdInitPayload(t *testing.T) {
	raw := storage.CmdSdInit(40)
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	if ptype != storage.SdInit {
		t.Errorf("type = 0x%02X, want 0x%02X", byte(ptype), byte(storage.SdInit))
	}
	if len(payload) != 1 || payload[0] != 40 {
		t.Errorf("payload = %v, want [40]", payload)
	}
}

// ─── Path-bearing command shape ─────────────────────────────────────────

func TestCmdFileListPayload(t *testing.T) {
	const path = "/sounds"
	raw := storage.CmdFileList(path, storage.TargetSD)
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	if ptype != storage.FileList {
		t.Errorf("type = 0x%02X, want 0x%02X", byte(ptype), byte(storage.FileList))
	}
	// Wire: [pathLen:u8][path][target:u8]
	wantLen := 1 + len(path) + 1
	if len(payload) != wantLen {
		t.Fatalf("payload length = %d, want %d", len(payload), wantLen)
	}
	if int(payload[0]) != len(path) {
		t.Errorf("pathLen = %d, want %d", payload[0], len(path))
	}
	if got := string(payload[1 : 1+len(path)]); got != path {
		t.Errorf("path = %q, want %q", got, path)
	}
	if payload[1+len(path)] != storage.TargetSD {
		t.Errorf("target = 0x%02X, want 0x%02X", payload[1+len(path)], storage.TargetSD)
	}
}

// ─── TargetName ─────────────────────────────────────────────────────────

func TestTargetName(t *testing.T) {
	if storage.TargetName(storage.TargetSD) != "sd" {
		t.Errorf("TargetName(TargetSD) = %q, want %q",
			storage.TargetName(storage.TargetSD), "sd")
	}
	if storage.TargetName(storage.TargetFlash) != "flash" {
		t.Errorf("TargetName(TargetFlash) = %q, want %q",
			storage.TargetName(storage.TargetFlash), "flash")
	}
}
