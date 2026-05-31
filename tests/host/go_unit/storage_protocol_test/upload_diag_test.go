package storage_protocol_test

// Round-trip + bounds regression for FILE_UPLOAD_DIAG_RESP — the upload
// post-mortem packet added 2026-05-31 to surface SD write latencies (the cause
// of large-file stream-upload stalls) over the wire.  Mirrors the firmware
// payload layout in storage_protocol.h exactly; if the two drift, this fails.

import (
	"encoding/binary"
	"testing"

	"scalefx/protocol/storage"
)

func TestDecodeUploadDiagRoundTrip(t *testing.T) {
	// Build a 35-byte payload exactly as the firmware emits it.
	p := make([]byte, 35)
	binary.LittleEndian.PutUint32(p[0:4], 14729216)   // bytesRecv
	binary.LittleEndian.PutUint32(p[4:8], 126826176)  // expectedSize
	binary.LittleEndian.PutUint16(p[8:10], 899)       // segIndex
	binary.LittleEndian.PutUint16(p[10:12], 7741)     // segCount
	p[12] = 87                                         // fillPct
	binary.LittleEndian.PutUint32(p[13:17], 900)      // sdWriteCount
	binary.LittleEndian.PutUint32(p[17:21], 14729216) // sdBytesWritten
	binary.LittleEndian.PutUint32(p[21:25], 31250)    // sdMaxLat_ms (the stall)
	binary.LittleEndian.PutUint32(p[25:29], 45000)    // sdTotalStall_ms
	binary.LittleEndian.PutUint32(p[29:33], 31300)    // maxLoopGap_ms
	p[33] = 0x02                                       // flags: streamActive
	p[34] = storage.ReasonClientCancel                // reason

	d, err := storage.DecodeUploadDiag(p)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if d.BytesRecv != 14729216 || d.ExpectedSize != 126826176 {
		t.Errorf("bytes: got %d/%d", d.BytesRecv, d.ExpectedSize)
	}
	if d.SegIndex != 899 || d.SegCount != 7741 {
		t.Errorf("seg: got %d/%d", d.SegIndex, d.SegCount)
	}
	if d.FillPct != 87 {
		t.Errorf("fillPct: got %d", d.FillPct)
	}
	if d.SdMaxLatMs != 31250 {
		t.Errorf("sdMaxLat: got %d", d.SdMaxLatMs)
	}
	if d.SdAvgLatMs() != 45000/900 {
		t.Errorf("avgLat: got %d want %d", d.SdAvgLatMs(), 45000/900)
	}
	if d.UploadActive {
		t.Error("uploadActive should be false (flags=0x02)")
	}
	if !d.StreamActive {
		t.Error("streamActive should be true (flags=0x02)")
	}
	if d.Reason != storage.ReasonClientCancel {
		t.Errorf("reason: got %d", d.Reason)
	}
}

func TestDecodeUploadDiagShortPayload(t *testing.T) {
	if _, err := storage.DecodeUploadDiag(make([]byte, 34)); err == nil {
		t.Fatal("expected error on 34-byte payload, got nil")
	}
}

func TestUploadDiagAvgLatZeroWrites(t *testing.T) {
	d := storage.UploadDiag{SdWriteCount: 0, SdTotalStallMs: 1234}
	if d.SdAvgLatMs() != 0 {
		t.Errorf("avg with 0 writes should be 0, got %d", d.SdAvgLatMs())
	}
}

func TestUploadReasonNames(t *testing.T) {
	// Every defined reason must render non-empty + the unknown path must format.
	for r := uint8(0); r <= 7; r++ {
		if storage.ReasonName(r) == "" {
			t.Errorf("reason %d rendered empty", r)
		}
	}
	if storage.ReasonName(99) == "" {
		t.Error("unknown reason rendered empty")
	}
}
