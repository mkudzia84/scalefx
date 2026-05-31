// Package storage mirrors serial/storage/storage_protocol.h — the
// host-side wire commands for SD card / LittleFS file management.
package storage

import (
	"encoding/binary"
	"fmt"

	"scalefx/protocol"
)

// ─── Packet types ─────────────────────────────────────────────────────

const (
	SdInit              protocol.PacketType = 0x93
	SdStatusReq         protocol.PacketType = 0x94
	SdStatusResp        protocol.PacketType = 0x95
	FlashStatusReq      protocol.PacketType = 0x99
	FileList            protocol.PacketType = 0x9A
	FileDelete          protocol.PacketType = 0x9B
	FileMkdir           protocol.PacketType = 0x9C
	FileInfo            protocol.PacketType = 0x9D
	FileInfoResp        protocol.PacketType = 0x9E
	FileDownload        protocol.PacketType = 0x9F
	FileUploadBegin     protocol.PacketType = 0xA0
	FileUploadData      protocol.PacketType = 0xA1
	FileUploadEnd       protocol.PacketType = 0xA2
	FileUploadCancel    protocol.PacketType = 0xA3
	FileUploadDiagReq   protocol.PacketType = 0xA4
	FileUploadDiagResp  protocol.PacketType = 0xA5
	FileTree            protocol.PacketType = 0xA9
	FileUploadProgress  protocol.PacketType = 0xB0
)

// UploadEndReason mirrors StorageWire::UploadEndReason — why the last upload
// ended (reported in FILE_UPLOAD_DIAG_RESP).
const (
	ReasonNone         = 0
	ReasonActive       = 1
	ReasonCompleted    = 2
	ReasonInactivity   = 3
	ReasonFlushFail    = 4
	ReasonHealth       = 5
	ReasonClientCancel = 6
	ReasonStaleReset   = 7
)

// ReasonName renders an UploadEndReason for humans.
func ReasonName(r uint8) string {
	switch r {
	case ReasonNone:
		return "none (no upload since boot)"
	case ReasonActive:
		return "active (in progress)"
	case ReasonCompleted:
		return "completed OK"
	case ReasonInactivity:
		return "inactivity timeout (client went silent)"
	case ReasonFlushFail:
		return "SD/flash write failed"
	case ReasonHealth:
		return "policy health abort"
	case ReasonClientCancel:
		return "client cancel"
	case ReasonStaleReset:
		return "superseded by new upload"
	default:
		return fmt.Sprintf("unknown(%d)", r)
	}
}

// ─── Wire enums ───────────────────────────────────────────────────────

const (
	TargetSD    byte = 0
	TargetFlash byte = 1
)

const (
	UploadSync   byte = 0
	UploadStream byte = 3
)

const (
	DeleteFlagNone      byte = 0x00
	DeleteFlagRecursive byte = 0x01
)

const (
	MkdirFlagNone    byte = 0x00
	MkdirFlagParents byte = 0x01
)

// TargetName returns "sd" or "flash" for the given target byte.
func TargetName(t byte) string {
	switch t {
	case TargetSD:
		return "sd"
	case TargetFlash:
		return "flash"
	default:
		return fmt.Sprintf("0x%02X", t)
	}
}

// ─── Error codes (0xA0..0xAF per CLAUDE.md) ──────────────────────────

const (
	ErrSdNotInitialized  protocol.ErrorCode = 0xA0
	ErrFileNotFound      protocol.ErrorCode = 0xA1
	ErrFileAlreadyExists protocol.ErrorCode = 0xA2
	ErrFileIoError       protocol.ErrorCode = 0xA3
	ErrFileTooLarge      protocol.ErrorCode = 0xA4
	ErrUploadInProgress  protocol.ErrorCode = 0xA5
	ErrNoUploadActive    protocol.ErrorCode = 0xA6
)

// ─── Decoded data types ───────────────────────────────────────────────

// SdStatus is the decoded SD_STATUS_RESP payload (20 bytes, sizes in MB).
//
//	[init:u8][cardSize_MB:u32][totalSpace_MB:u32][freeSpace_MB:u32]
//	[fatType:u8][cardType:u8][busMode:u8][usedSpace_MB:u32]
type SdStatus struct {
	Initialized  bool   `json:"initialized"`
	CardSizeMB   uint32 `json:"cardSizeMb"`
	TotalSpaceMB uint32 `json:"totalSpaceMb"`
	FreeSpaceMB  uint32 `json:"freeSpaceMb"`
	UsedSpaceMB  uint32 `json:"usedSpaceMb"`
	FatType      byte   `json:"fatType"`
	CardType     byte   `json:"cardType"`
	BusMode      byte   `json:"busMode"`
}

// DecodeSdStatus parses SD_STATUS_RESP.
func DecodeSdStatus(p []byte) (SdStatus, error) {
	if len(p) < 20 {
		return SdStatus{}, fmt.Errorf("sd status: expected 20 bytes, got %d", len(p))
	}
	return SdStatus{
		Initialized:  p[0] != 0,
		CardSizeMB:   binary.LittleEndian.Uint32(p[1:5]),
		TotalSpaceMB: binary.LittleEndian.Uint32(p[5:9]),
		FreeSpaceMB:  binary.LittleEndian.Uint32(p[9:13]),
		FatType:      p[13],
		CardType:     p[14],
		BusMode:      p[15],
		UsedSpaceMB:  binary.LittleEndian.Uint32(p[16:20]),
	}, nil
}

// FlashStatus is the decoded FLASH_STATUS payload (13 bytes, sizes in bytes).
// The firmware reuses FLASH_STATUS_REQ (0x99) as the response type.
//
//	[init:u8][totalBytes:u32][usedBytes:u32][freeBytes:u32]
type FlashStatus struct {
	Initialized bool   `json:"initialized"`
	TotalBytes  uint32 `json:"totalBytes"`
	UsedBytes   uint32 `json:"usedBytes"`
	FreeBytes   uint32 `json:"freeBytes"`
}

// DecodeFlashStatus parses the 13-byte FLASH_STATUS reply.
func DecodeFlashStatus(p []byte) (FlashStatus, error) {
	if len(p) < 13 {
		return FlashStatus{}, fmt.Errorf("flash status: expected 13 bytes, got %d", len(p))
	}
	return FlashStatus{
		Initialized: p[0] != 0,
		TotalBytes:  binary.LittleEndian.Uint32(p[1:5]),
		UsedBytes:   binary.LittleEndian.Uint32(p[5:9]),
		FreeBytes:   binary.LittleEndian.Uint32(p[9:13]),
	}, nil
}

// FileInfoResult decodes FILE_INFO_RESP.
type FileInfoResult struct {
	Exists bool   `json:"exists"`
	IsDir  bool   `json:"isDir"`
	Size   uint32 `json:"size"`
}

// DecodeFileInfo parses FILE_INFO_RESP.
func DecodeFileInfo(p []byte) (FileInfoResult, error) {
	if len(p) != 6 {
		return FileInfoResult{}, fmt.Errorf("file info: expected 6 bytes, got %d", len(p))
	}
	return FileInfoResult{
		Exists: p[0] != 0,
		IsDir:  p[1] != 0,
		Size:   binary.LittleEndian.Uint32(p[2:6]),
	}, nil
}

// UploadProgress is the decoded FILE_UPLOAD_PROGRESS async payload.
type UploadProgress struct {
	SegmentIdx   uint16 `json:"segmentIdx"`
	BytesReceived uint32 `json:"bytesReceived"`
	RingFillPct  byte   `json:"ringFillPct"`
}

// DecodeUploadProgress parses FILE_UPLOAD_PROGRESS.
func DecodeUploadProgress(p []byte) (UploadProgress, error) {
	if len(p) != 7 {
		return UploadProgress{}, fmt.Errorf("upload progress: expected 7 bytes, got %d", len(p))
	}
	return UploadProgress{
		SegmentIdx:    binary.LittleEndian.Uint16(p[0:2]),
		BytesReceived: binary.LittleEndian.Uint32(p[2:6]),
		RingFillPct:   p[6],
	}, nil
}

// UploadDiag is the decoded FILE_UPLOAD_DIAG_RESP payload — the post-mortem of
// the last (or active) upload. The SD write latencies (esp. SdMaxLatMs) are the
// smoking gun for a stalled large-file upload that the stream phase could never
// report over the wire.
type UploadDiag struct {
	BytesRecv      uint32 `json:"bytesRecv"`
	ExpectedSize   uint32 `json:"expectedSize"`
	SegIndex       uint16 `json:"segIndex"`
	SegCount       uint16 `json:"segCount"`
	FillPct        uint8  `json:"fillPct"`
	SdWriteCount   uint32 `json:"sdWriteCount"`
	SdBytesWritten uint32 `json:"sdBytesWritten"`
	SdMaxLatMs     uint32 `json:"sdMaxLatMs"`     // worst single SD write
	SdTotalStallMs uint32 `json:"sdTotalStallMs"` // cumulative SD write time
	MaxLoopGapMs   uint32 `json:"maxLoopGapMs"`
	UploadActive   bool   `json:"uploadActive"`
	StreamActive   bool   `json:"streamActive"`
	Reason         uint8  `json:"reason"`
}

// SdAvgLatMs is the mean SD write latency (0 if no writes recorded).
func (d UploadDiag) SdAvgLatMs() uint32 {
	if d.SdWriteCount == 0 {
		return 0
	}
	return d.SdTotalStallMs / d.SdWriteCount
}

// DecodeUploadDiag parses FILE_UPLOAD_DIAG_RESP (35 bytes).
func DecodeUploadDiag(p []byte) (UploadDiag, error) {
	if len(p) < 35 {
		return UploadDiag{}, fmt.Errorf("upload diag: expected 35 bytes, got %d", len(p))
	}
	return UploadDiag{
		BytesRecv:      binary.LittleEndian.Uint32(p[0:4]),
		ExpectedSize:   binary.LittleEndian.Uint32(p[4:8]),
		SegIndex:       binary.LittleEndian.Uint16(p[8:10]),
		SegCount:       binary.LittleEndian.Uint16(p[10:12]),
		FillPct:        p[12],
		SdWriteCount:   binary.LittleEndian.Uint32(p[13:17]),
		SdBytesWritten: binary.LittleEndian.Uint32(p[17:21]),
		SdMaxLatMs:     binary.LittleEndian.Uint32(p[21:25]),
		SdTotalStallMs: binary.LittleEndian.Uint32(p[25:29]),
		MaxLoopGapMs:   binary.LittleEndian.Uint32(p[29:33]),
		UploadActive:   p[33]&0x01 != 0,
		StreamActive:   p[33]&0x02 != 0,
		Reason:         p[34],
	}, nil
}

// ─── Command builders ────────────────────────────────────────────────

func CmdSdInit(speedMHz byte) []byte {
	return protocol.BuildPacket(SdInit, []byte{speedMHz}, 0)
}
func CmdSdStatusReq() []byte    { return protocol.BuildPacket(SdStatusReq, nil, 0) }
func CmdFlashStatusReq() []byte { return protocol.BuildPacket(FlashStatusReq, nil, 0) }

// pathPayload builds [pathLen:u8][path:str][target:u8] (always-present target).
func pathPayload(path string, target byte) []byte {
	pb := []byte(path)
	out := make([]byte, 0, 2+len(pb))
	out = append(out, byte(len(pb)))
	out = append(out, pb...)
	out = append(out, target)
	return out
}

func CmdFileList(path string, target byte) []byte {
	return protocol.BuildPacket(FileList, pathPayload(path, target), 0)
}
func CmdFileTree(path string, target byte) []byte {
	return protocol.BuildPacket(FileTree, pathPayload(path, target), 0)
}
func CmdFileDelete(path string, target, flags byte) []byte {
	payload := pathPayload(path, target)
	payload = append(payload, flags)
	return protocol.BuildPacket(FileDelete, payload, 0)
}
func CmdFileMkdir(path string, target, flags byte) []byte {
	payload := pathPayload(path, target)
	payload = append(payload, flags)
	return protocol.BuildPacket(FileMkdir, payload, 0)
}
func CmdFileInfo(path string) []byte {
	pb := []byte(path)
	payload := append([]byte{byte(len(pb))}, pb...)
	return protocol.BuildPacket(FileInfo, payload, 0)
}
func CmdFileDownload(path string) []byte {
	pb := []byte(path)
	payload := append([]byte{byte(len(pb))}, pb...)
	return protocol.BuildPacket(FileDownload, payload, 0)
}

// CmdFileDownloadAt is the target-aware variant.  The firmware's
// extractPathAndTarget treats the trailing byte as the requested backend
// (TargetSD / TargetFlash); CmdFileDownload omits it and the firmware
// defaults to SD, which is wrong for config files that live on flash.
func CmdFileDownloadAt(path string, target byte) []byte {
	pb := []byte(path)
	payload := make([]byte, 0, 2+len(pb))
	payload = append(payload, byte(len(pb)))
	payload = append(payload, pb...)
	payload = append(payload, target)
	return protocol.BuildPacket(FileDownload, payload, 0)
}

// CmdFileUploadBegin builds the wire payload the firmware expects:
//
//	[totalSize:u32LE][pathLen:u8][path:str][target:u8][mode:u8]
//
// `target` is TargetSD / TargetFlash; `mode` is UploadSync (one
// FILE_UPLOAD_DATA per chunk + per-chunk ACK) or UploadStream (raw
// binary segments with per-segment progress ACK — much higher
// throughput on the ESP32 side).
func CmdFileUploadBegin(path string, totalSize uint32, target, mode byte) []byte {
	pb := []byte(path)
	payload := make([]byte, 4+1+len(pb)+1+1)
	copy(payload[0:4], protocol.U32LE(totalSize))
	payload[4] = byte(len(pb))
	copy(payload[5:5+len(pb)], pb)
	payload[5+len(pb)] = target
	payload[6+len(pb)] = mode
	return protocol.BuildPacket(FileUploadBegin, payload, 0)
}

// CmdFileUploadData wraps a sync-mode chunk in a FILE_UPLOAD_DATA
// packet.  The wire payload is `[seqNum:u16LE][crc16:u16LE][data:N]` —
// the firmware verifies both the sequence and the CRC-16/CCITT before
// committing the bytes to the open file.  Stream-mode uploads skip
// this packet entirely; they send raw segment bytes through
// `Connection.SendRaw`.
func CmdFileUploadData(seq uint16, chunk []byte) []byte {
	crc := protocol.CRC16CCITT(chunk)
	payload := make([]byte, 4+len(chunk))
	payload[0] = byte(seq)
	payload[1] = byte(seq >> 8)
	payload[2] = byte(crc)
	payload[3] = byte(crc >> 8)
	copy(payload[4:], chunk)
	return protocol.BuildPacket(FileUploadData, payload, 0)
}
func CmdFileUploadEnd() []byte    { return protocol.BuildPacket(FileUploadEnd, nil, 0) }
func CmdFileUploadCancel() []byte { return protocol.BuildPacket(FileUploadCancel, nil, 0) }
func CmdFileUploadDiagReq() []byte {
	return protocol.BuildPacket(FileUploadDiagReq, nil, 0)
}

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		SdInit:             "SD_INIT",
		SdStatusReq:        "SD_STATUS_REQ",
		SdStatusResp:       "SD_STATUS_RESP",
		FlashStatusReq:     "FLASH_STATUS_REQ",
		FileList:           "FILE_LIST",
		FileDelete:         "FILE_DELETE",
		FileMkdir:          "FILE_MKDIR",
		FileInfo:           "FILE_INFO",
		FileInfoResp:       "FILE_INFO_RESP",
		FileDownload:       "FILE_DOWNLOAD",
		FileUploadBegin:    "FILE_UPLOAD_BEGIN",
		FileUploadData:     "FILE_UPLOAD_DATA",
		FileUploadEnd:      "FILE_UPLOAD_END",
		FileUploadCancel:   "FILE_UPLOAD_CANCEL",
		FileUploadDiagReq:  "FILE_UPLOAD_DIAG_REQ",
		FileUploadDiagResp: "FILE_UPLOAD_DIAG_RESP",
		FileTree:           "FILE_TREE",
		FileUploadProgress: "FILE_UPLOAD_PROGRESS",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrSdNotInitialized:  "SD_NOT_INITIALIZED",
		ErrFileNotFound:      "FILE_NOT_FOUND",
		ErrFileAlreadyExists: "FILE_ALREADY_EXISTS",
		ErrFileIoError:       "FILE_IO_ERROR",
		ErrFileTooLarge:      "FILE_TOO_LARGE",
		ErrUploadInProgress:  "UPLOAD_IN_PROGRESS",
		ErrNoUploadActive:    "NO_UPLOAD_ACTIVE",
	})
}
