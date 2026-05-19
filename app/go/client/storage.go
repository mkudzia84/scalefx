package client

import (
	"crypto/md5"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"scalefx/protocol"
	"scalefx/protocol/storage"
)

// Storage exposes the hub-side storage subsystem — SD card / LittleFS
// status, file enumeration / info / delete / mkdir, plus
// download / upload with progress.
type Storage struct {
	c *Client

	// peerMaxPayload caps FILE_UPLOAD_DATA chunk size.  Defaults to
	// the safe Pico value; SetPeerMaxPayload upgrades after IDENTIFY
	// reveals an ESP32-S3 peer.
	peerMaxPayload int
}

// Re-exported wire constants so CLI callers don't need the storage
// protocol package directly.
const (
	TargetSD    = storage.TargetSD
	TargetFlash = storage.TargetFlash

	UploadSync   = storage.UploadSync
	UploadStream = storage.UploadStream

	DeleteFlagNone      = storage.DeleteFlagNone
	DeleteFlagRecursive = storage.DeleteFlagRecursive

	MkdirFlagNone    = storage.MkdirFlagNone
	MkdirFlagParents = storage.MkdirFlagParents
)

// Wire envelope sizing.  FILE_UPLOAD_DATA is sent as
// `[data:N]` on the new protocol — no internal seq/crc envelope — so
// the per-frame budget is the entire payload window.
const (
	PicoMaxPayload  = 512
	Esp32MaxPayload = 2048
)

// TargetName returns "sd" or "flash" for the given target byte.
func TargetName(t byte) string { return storage.TargetName(t) }

// SdStatus is the decoded SD_STATUS_RESP.
type SdStatus = storage.SdStatus

// FileInfoResult is the decoded FILE_INFO_RESP.
type FileInfoResult = storage.FileInfoResult

// SetPeerMaxPayload tells the storage facet how big a FILE_UPLOAD_DATA
// payload the connected peer accepts.  Clamps to [Pico, ESP32] limits.
func (s *Storage) SetPeerMaxPayload(size int) {
	if size < PicoMaxPayload {
		size = PicoMaxPayload
	}
	if size > Esp32MaxPayload {
		size = Esp32MaxPayload
	}
	s.peerMaxPayload = size
}

// ─── Status ──────────────────────────────────────────────────────────

// SdInit (re-)initialises the SD card driver.  `speedMHz` of 0 uses
// firmware default; non-zero pins the SD clock.
func (s *Storage) SdInit(speedMHz byte) error {
	return s.c.sendExpectACK(storage.CmdSdInit(speedMHz))
}

// SdStatus requests + decodes the SD status.
func (s *Storage) SdStatus() (SdStatus, error) {
	resp, err := s.c.sendForResp(storage.CmdSdStatusReq(), storage.SdStatusResp)
	if err != nil {
		return SdStatus{}, err
	}
	return storage.DecodeSdStatus(resp.Payload)
}

// FlashStatus requests the LittleFS status.  Firmware replies with the
// SD_STATUS_RESP-shaped layout (just describing the flash backend), so
// the same decoder applies.
func (s *Storage) FlashStatus() (SdStatus, error) {
	resp, err := s.c.sendForResp(storage.CmdFlashStatusReq(), storage.SdStatusResp)
	if err != nil {
		return SdStatus{}, err
	}
	return storage.DecodeSdStatus(resp.Payload)
}

// ─── File ops ────────────────────────────────────────────────────────

// FileInfo returns metadata for the path (exists / dir / size).
func (s *Storage) FileInfo(path string) (FileInfoResult, error) {
	resp, err := s.c.sendForResp(storage.CmdFileInfo(path), storage.FileInfoResp)
	if err != nil {
		return FileInfoResult{}, err
	}
	return storage.DecodeFileInfo(resp.Payload)
}

// FileList returns the directory listing as the text blob the firmware
// formats (one entry per line, generally "<name>  <size>" with a `/`
// suffix on directories).
func (s *Storage) FileList(path string, target byte) (string, error) {
	return s.streamText(storage.CmdFileList(path, target), 10*time.Second)
}

// FileTree returns a recursive listing of `path`.
func (s *Storage) FileTree(path string, target byte) (string, error) {
	return s.streamText(storage.CmdFileTree(path, target), 30*time.Second)
}

// FileDelete removes `path`.  Use DeleteFlagRecursive for `rm -rf`.
func (s *Storage) FileDelete(path string, target, flags byte) error {
	return s.c.sendExpectACK(storage.CmdFileDelete(path, target, flags))
}

// FileMkdir creates a directory.  Use MkdirFlagParents for `mkdir -p`.
func (s *Storage) FileMkdir(path string, target, flags byte) error {
	return s.c.sendExpectACK(storage.CmdFileMkdir(path, target, flags))
}

// ─── Download ────────────────────────────────────────────────────────

// DownloadResult bundles the download data + verification hash.
type DownloadResult struct {
	Data    []byte
	MD5     [16]byte
	Elapsed time.Duration
}

// FileDownload streams a file body to memory.  For large files use
// FileDownloadTo to stream into a writer.
func (s *Storage) FileDownload(path string, timeout time.Duration) (DownloadResult, error) {
	if timeout <= 0 {
		timeout = 30 * time.Second
	}
	start := time.Now()
	stream, err := s.c.conn.SendAndReceiveStream(storage.CmdFileDownload(path), timeout)
	if err != nil {
		return DownloadResult{}, err
	}
	r := DownloadResult{Data: stream.Data, Elapsed: time.Since(start)}
	r.MD5 = md5.Sum(stream.Data)
	return r, nil
}

// FileDownloadTo is identical to FileDownload but writes directly into
// `dst`, returning byte count + MD5.
func (s *Storage) FileDownloadTo(path string, dst io.Writer, timeout time.Duration) (int64, [16]byte, error) {
	res, err := s.FileDownload(path, timeout)
	if err != nil {
		return 0, [16]byte{}, err
	}
	n, werr := dst.Write(res.Data)
	if werr != nil {
		return int64(n), res.MD5, werr
	}
	return int64(n), res.MD5, nil
}

// ─── Upload ──────────────────────────────────────────────────────────

// UploadOptions configures FileUpload.
type UploadOptions struct {
	Path       string  // destination on the hub (forward slashes)
	Target     byte    // TargetSD or TargetFlash
	ChunkSize  int     // 0 → derived from peerMaxPayload
	OnProgress func(bytesSent, total int64)
}

// UploadResult bundles upload statistics.
type UploadResult struct {
	BytesSent int64
	MD5       [16]byte
	Elapsed   time.Duration
}

// FileUpload reads `local` and uploads it to `opt.Path` on the hub.
// Uses the sync mode — one FILE_UPLOAD_DATA per chunk, ACK awaited
// before the next chunk goes out.  Stream / windowed modes are out
// of v1.
func (s *Storage) FileUpload(local string, opt UploadOptions) (UploadResult, error) {
	if opt.Path == "" {
		return UploadResult{}, fmt.Errorf("upload: destination path is empty")
	}
	f, err := os.Open(local)
	if err != nil {
		return UploadResult{}, err
	}
	defer f.Close()
	stat, err := f.Stat()
	if err != nil {
		return UploadResult{}, err
	}
	size := stat.Size()
	if size > int64(^uint32(0)) {
		return UploadResult{}, fmt.Errorf("upload: file too large (%d bytes)", size)
	}

	// Normalise destination slashes.
	opt.Path = strings.ReplaceAll(opt.Path, "\\", "/")
	if !strings.HasPrefix(opt.Path, "/") {
		opt.Path = "/" + opt.Path
	}

	chunkSize := opt.ChunkSize
	if chunkSize <= 0 {
		chunkSize = s.uploadChunkSize()
	}

	if err := s.c.sendExpectACK(
		storage.CmdFileUploadBegin(opt.Path, UploadSync, uint32(size))); err != nil {
		return UploadResult{}, fmt.Errorf("upload begin: %w", err)
	}

	start := time.Now()
	hash := md5.New()
	buf := make([]byte, chunkSize)
	var sent int64
	for {
		n, rerr := f.Read(buf)
		if n > 0 {
			chunk := buf[:n]
			if err := s.c.sendExpectACK(storage.CmdFileUploadData(chunk)); err != nil {
				_ = s.c.conn.Send(storage.CmdFileUploadCancel())
				return UploadResult{BytesSent: sent}, fmt.Errorf("upload chunk @%d: %w", sent, err)
			}
			hash.Write(chunk)
			sent += int64(n)
			if opt.OnProgress != nil {
				opt.OnProgress(sent, size)
			}
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			_ = s.c.conn.Send(storage.CmdFileUploadCancel())
			return UploadResult{BytesSent: sent}, rerr
		}
	}
	s.c.conn.FlushOutput()
	if err := s.c.sendExpectACK(storage.CmdFileUploadEnd()); err != nil {
		return UploadResult{BytesSent: sent}, fmt.Errorf("upload end: %w", err)
	}

	var sum [16]byte
	copy(sum[:], hash.Sum(nil))
	return UploadResult{BytesSent: sent, MD5: sum, Elapsed: time.Since(start)}, nil
}

// CancelUpload sends FILE_UPLOAD_CANCEL — useful when an out-of-band
// upload got stuck mid-flight.
func (s *Storage) CancelUpload() error {
	return s.c.sendExpectACK(storage.CmdFileUploadCancel())
}

// ─── Helpers ─────────────────────────────────────────────────────────

func (s *Storage) uploadChunkSize() int {
	cap := s.peerMaxPayload
	if cap <= 0 {
		cap = PicoMaxPayload
	}
	// Reserve a few bytes for COBS overhead — the firmware enforces a
	// hard max on the *encoded* frame size.  Leave 8 bytes of slack.
	return cap - 8
}

func (s *Storage) streamText(packet []byte, timeout time.Duration) (string, error) {
	stream, err := s.c.conn.SendAndReceiveStream(packet, timeout)
	if err != nil {
		return "", err
	}
	return string(stream.Data), nil
}

// Compile-time reference so vet doesn't drop the protocol import when
// this file is built in isolation.
var _ protocol.PacketType = storage.SdInit
