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

// FlashStatus is the decoded LittleFS status — re-exported so CLI
// callers don't pull the protocol package.
type FlashStatus = storage.FlashStatus

// FlashStatus requests the LittleFS status.  Firmware reuses
// FLASH_STATUS_REQ (0x99) as the response type with a separate
// byte-denominated payload shape.
func (s *Storage) FlashStatus() (FlashStatus, error) {
	resp, err := s.c.sendForResp(storage.CmdFlashStatusReq(), storage.FlashStatusReq)
	if err != nil {
		return FlashStatus{}, err
	}
	return storage.DecodeFlashStatus(resp.Payload)
}

// UploadDiag is the decoded post-mortem of the last/active upload.
type UploadDiag = storage.UploadDiag

// UploadReasonName renders an upload-end reason code (UploadDiag.Reason).
func UploadReasonName(r uint8) string { return storage.ReasonName(r) }

// UploadDiag pulls the firmware's diagnostics snapshot for the most recent (or
// active) upload — SD write latencies, segment progress, abort reason. Safe to
// call any time the wire is in COBS mode (i.e. NOT mid raw-stream); the stats
// survive until the next upload begins, so it works right after a failure.
func (s *Storage) UploadDiag() (UploadDiag, error) {
	resp, err := s.c.sendForResp(storage.CmdFileUploadDiagReq(), storage.FileUploadDiagResp)
	if err != nil {
		return UploadDiag{}, err
	}
	return storage.DecodeUploadDiag(resp.Payload)
}

// uploadDiagSummary fetches the firmware upload post-mortem and renders a short
// multi-line summary suitable for embedding in an error (so it surfaces in BOTH
// the CLI and the Studio console, which echo the error verbatim).  Best-effort:
// a brief settle wait lets the firmware finish an in-flight SD write + process
// the cancel before it can answer the query.  Returns "" if unavailable.
func (s *Storage) uploadDiagSummary() string {
	time.Sleep(500 * time.Millisecond)
	d, err := s.UploadDiag()
	if err != nil {
		return fmt.Sprintf("\n  (upload diagnostics unavailable: %v)", err)
	}
	out := "\n  ── upload diagnostics ──" +
		fmt.Sprintf("\n    ended:     %s", storage.ReasonName(d.Reason)) +
		fmt.Sprintf("\n    progress:  %d/%d bytes, seg %d/%d, ring fill %d%%",
			d.BytesRecv, d.ExpectedSize, d.SegIndex, d.SegCount, d.FillPct) +
		fmt.Sprintf("\n    SD writes: %d writes, %d KB, avg %d ms, MAX %d ms, total I/O %d ms",
			d.SdWriteCount, d.SdBytesWritten/1024, d.SdAvgLatMs(), d.SdMaxLatMs, d.SdTotalStallMs) +
		fmt.Sprintf("\n    max loop gap: %d ms", d.MaxLoopGapMs)
	if d.SdMaxLatMs >= 1000 {
		out += fmt.Sprintf("\n    ⚠ a single SD write took %.1fs — the card (or its wiring) "+
			"is stalling; try another card / reformat / check SDIO signal integrity",
			float64(d.SdMaxLatMs)/1000)
	}
	return out
}

// ─── File ops ────────────────────────────────────────────────────────

// FileInfo returns metadata for the path (exists / dir / size).  NOTE: the
// firmware defaults this query to SD — for a flash path (config files), use
// FileInfoOn(path, TargetFlash) or it reads as absent.
func (s *Storage) FileInfo(path string) (FileInfoResult, error) {
	resp, err := s.c.sendForResp(storage.CmdFileInfo(path), storage.FileInfoResp)
	if err != nil {
		return FileInfoResult{}, err
	}
	return storage.DecodeFileInfo(resp.Payload)
}

// FileInfoOn returns metadata for the path on an EXPLICIT backend
// (TargetSD / TargetFlash).  Use this for config files (they live on flash).
func (s *Storage) FileInfoOn(path string, target byte) (FileInfoResult, error) {
	resp, err := s.c.sendForResp(storage.CmdFileInfoTarget(path, target), storage.FileInfoResp)
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

// FileDownloadFrom streams a file body to memory from a specific storage
// target (TargetSD / TargetFlash).  Use this for config files — the
// firmware's FILE_DOWNLOAD payload defaults to SD when no target byte is
// sent, which is wrong for /hubfx.yaml, /enginefx.yaml, etc.
func (s *Storage) FileDownloadFrom(path string, target byte, timeout time.Duration) (DownloadResult, error) {
	if timeout <= 0 {
		timeout = 30 * time.Second
	}
	start := time.Now()
	stream, err := s.c.conn.SendAndReceiveStream(storage.CmdFileDownloadAt(path, target), timeout)
	if err != nil {
		return DownloadResult{}, err
	}
	r := DownloadResult{Data: stream.Data, Elapsed: time.Since(start)}
	r.MD5 = md5.Sum(stream.Data)
	return r, nil
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

// UploadMode picks the wire protocol used for the body of the upload.
// Sync is the simple per-chunk-ACK loop (works everywhere, modest
// throughput).  Stream is the high-throughput ESP32 path: raw binary
// segments + per-segment FILE_UPLOAD_PROGRESS ACKs with flow control
// based on the firmware's ring-buffer fill.
type UploadMode = byte

// UploadOptions configures FileUpload.
type UploadOptions struct {
	Path       string // destination on the hub (forward slashes)
	Target     byte   // TargetSD or TargetFlash
	Mode       UploadMode
	ChunkSize  int    // 0 → derived from peerMaxPayload (Sync only)
	OnProgress func(bytesSent, total int64)
}

// UploadResult bundles upload statistics.  RemoteMD5 / LocalMD5 are
// populated when the firmware echoes its computed digest in the
// FILE_UPLOAD_END ACK payload (16 bytes).  MD5Match is the verdict.
type UploadResult struct {
	BytesSent int64
	Elapsed   time.Duration
	SpeedKBs  float64 // KB/s over the entire transfer
	LocalMD5  [16]byte
	RemoteMD5 [16]byte
	MD5Match  bool
	Mode      UploadMode
}

// FileUpload reads `local` and uploads it to `opt.Path`.  Mode picks
// the wire path; Target picks the backend (SD / Flash).  On failure
// the routine sends FILE_UPLOAD_CANCEL so the device cleans up its
// pre-allocated file before returning.
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

	opt.Path = strings.ReplaceAll(opt.Path, "\\", "/")
	if !strings.HasPrefix(opt.Path, "/") {
		opt.Path = "/" + opt.Path
	}

	data, err := io.ReadAll(f)
	if err != nil {
		return UploadResult{}, fmt.Errorf("read %s: %w", local, err)
	}

	// Mark the upload phase for the whole transfer (both modes).  This stands the
	// keepalive goroutine down and signals background pollers (UploadActive()) to
	// hold off so nothing contends with the chunk/ACK loop on the shared wire.
	// uploadStream additionally toggles SetStreamPhase around its raw-byte bursts
	// for collision detection — this is the broader, mode-agnostic guard.
	s.c.conn.SetUploadPhase(true)
	defer s.c.conn.SetUploadPhase(false)

	switch opt.Mode {
	case UploadStream:
		return s.uploadStream(data, opt)
	default:
		return s.uploadSync(data, opt)
	}
}

// ─── Sync upload ─────────────────────────────────────────────────────

func (s *Storage) uploadSync(data []byte, opt UploadOptions) (UploadResult, error) {
	chunkSize := opt.ChunkSize
	if chunkSize <= 0 {
		chunkSize = s.uploadChunkSize()
	}
	total := int64(len(data))

	resp, err := s.c.conn.SendExpectACK(
		storage.CmdFileUploadBegin(opt.Path, uint32(total), opt.Target, UploadSync))
	if err != nil {
		return UploadResult{Mode: UploadSync}, fmt.Errorf("upload begin: %w", err)
	}
	if resp.IsNACK() {
		return UploadResult{Mode: UploadSync}, errFromNack(resp)
	}

	// Sync-mode chunk header is 4 bytes ([seq:u16][crc16:u16]).  The
	// peer's max payload window already accounted for that in
	// uploadChunkSize(); we just need enough headroom not to overrun.
	const syncHeader = 4
	if chunkSize <= syncHeader {
		return UploadResult{Mode: UploadSync}, fmt.Errorf("upload: chunk size too small")
	}
	chunkSize -= syncHeader

	start := time.Now()
	hash := md5.New()
	hash.Write(data)
	var sent int64
	seq := uint16(0)
	for off := 0; off < len(data); off += chunkSize {
		end := off + chunkSize
		if end > len(data) {
			end = len(data)
		}
		chunk := data[off:end]
		if err := s.c.sendExpectACK(storage.CmdFileUploadData(seq, chunk)); err != nil {
			_ = s.c.conn.Send(storage.CmdFileUploadCancel())
			return UploadResult{BytesSent: sent, Mode: UploadSync},
				fmt.Errorf("upload chunk @%d (seq=%d): %w", off, seq, err)
		}
		sent += int64(len(chunk))
		seq++
		if opt.OnProgress != nil {
			opt.OnProgress(sent, total)
		}
	}
	s.c.conn.FlushOutput()

	resp, err = s.c.conn.SendExpectACKTimeout(storage.CmdFileUploadEnd(), 60*time.Second)
	if err != nil {
		return UploadResult{BytesSent: sent, Mode: UploadSync}, fmt.Errorf("upload end: %w", err)
	}
	if resp.IsNACK() {
		_ = s.c.conn.Send(storage.CmdFileUploadCancel())
		return UploadResult{BytesSent: sent, Mode: UploadSync}, errFromNack(resp)
	}
	return s.finalizeUpload(resp, sent, time.Since(start), hash.Sum(nil), UploadSync)
}

// ─── Stream upload ───────────────────────────────────────────────────

// uploadStream is the high-throughput path used by the ESP32 HubFX
// firmware.  The protocol is:
//
//  1. FILE_UPLOAD_BEGIN with mode=UploadStream — firmware ACK carries
//     [segment_size:u32LE][segment_count:u16LE].
//  2. For each segment: blast raw bytes (no COBS framing) in 32 KB
//     write-sized chunks, wait for FILE_UPLOAD_PROGRESS TAG_ASYNC with
//     [seg_idx:u16][bytes:u32][fill_pct:u8].  Throttle if fill > 50 %.
//  3. FILE_UPLOAD_END — ACK carries the 16-byte remote MD5.
func (s *Storage) uploadStream(data []byte, opt UploadOptions) (UploadResult, error) {
	total := int64(len(data))

	resp, err := s.c.conn.SendExpectACK(
		storage.CmdFileUploadBegin(opt.Path, uint32(total), opt.Target, UploadStream))
	if err != nil {
		return UploadResult{Mode: UploadStream}, fmt.Errorf("upload begin: %w", err)
	}
	if resp.IsNACK() {
		return UploadResult{Mode: UploadStream}, errFromNack(resp)
	}
	segSize := uint32(524288)
	if len(resp.Payload) >= 6 {
		segSize = protocol.ReadU32LE(resp.Payload, 0)
	}

	progressCh := make(chan *protocol.Response, 16)
	s.c.conn.RegisterAsyncFilter(storage.FileUploadProgress, progressCh)
	defer s.c.conn.UnregisterAsyncFilter(storage.FileUploadProgress)

	// Raw-stream phase begins now (firmware is in stream mode after the BEGIN
	// ACK).  Any COBS packet from another goroutine (status poll, keepalive)
	// written from here until UPLOAD_END corrupts the file — SetStreamPhase
	// makes Send() log such collisions.  Cleared before UPLOAD_END (COBS) and
	// on every error path below.
	s.c.conn.SetStreamPhase(true)

	start := time.Now()
	hash := md5.New()
	hash.Write(data)
	const writeChunk = 32768

	var sent int64
	for sent < total {
		thisSeg := int64(segSize)
		if rem := total - sent; thisSeg > rem {
			thisSeg = rem
		}
		segStart := sent
		segEnd := segStart + thisSeg
		for segStart < segEnd {
			step := segEnd - segStart
			if step > writeChunk {
				step = writeChunk
			}
			if err := s.c.conn.SendRaw(data[segStart : segStart+step]); err != nil {
				s.c.conn.SetStreamPhase(false)
				_ = s.c.conn.Send(storage.CmdFileUploadCancel())
				return UploadResult{BytesSent: sent, Mode: UploadStream},
					fmt.Errorf("stream write @%d: %w", segStart, err)
			}
			segStart += step
			sent = segStart
			if opt.OnProgress != nil {
				opt.OnProgress(sent, total)
			}
		}
		s.c.conn.FlushOutput()

		// Segment-ACK deadline.  A single segment's firmware-side write can stall
		// for several seconds when the SD card does internal GC / wear-levelling
		// (more frequent tens of MB into a large file).  The firmware no longer
		// self-aborts on a slow-but-healthy write (it re-stamps its inactivity
		// timer after each flush), but the client must still wait long enough to
		// cover a worst-case card stall — 15 s was too tight for an 86 MB upload
		// (died at a random ~30 MB).  30 s gives margin; a slow segment logs a
		// warning so a degrading card is visible before it fails outright.
		segWait := time.Now()
		select {
		case ack := <-progressCh:
			if waited := time.Since(segWait); waited > 5*time.Second && s.c.conn != nil {
				fmt.Printf("  ⚠ slow upload segment: ACK took %.1fs at %d/%d bytes\n",
					waited.Seconds(), sent, total)
			}
			if len(ack.Payload) >= 7 {
				fill := int(ack.Payload[6])
				if fill > 50 {
					time.Sleep(time.Duration(fill-50) * 60 * time.Millisecond / 1)
				}
			}
		case <-time.After(30 * time.Second):
			s.c.conn.SetStreamPhase(false)
			_ = s.c.conn.Send(storage.CmdFileUploadCancel())
			// Embed the firmware's post-mortem in the error so the operator can
			// SEE the cause (SD write latencies are invisible during the stream —
			// raw mode can't emit log packets over the wire).  The error is echoed
			// verbatim by both the CLI and the Studio console.
			diag := s.uploadDiagSummary()
			return UploadResult{BytesSent: sent, Mode: UploadStream},
				fmt.Errorf("stream timeout waiting for segment ACK (@%d/%d bytes, "+
					"SD write stalled > 30s)%s", sent, total, diag)
		}
	}

	// Raw phase done — firmware is back in COBS mode; UPLOAD_END is a normal
	// packet again.
	s.c.conn.SetStreamPhase(false)

	resp, err = s.c.conn.SendExpectACKTimeout(storage.CmdFileUploadEnd(), 60*time.Second)
	if err != nil {
		return UploadResult{BytesSent: sent, Mode: UploadStream}, fmt.Errorf("upload end: %w", err)
	}
	if resp.IsNACK() {
		_ = s.c.conn.Send(storage.CmdFileUploadCancel())
		return UploadResult{BytesSent: sent, Mode: UploadStream}, errFromNack(resp)
	}
	return s.finalizeUpload(resp, sent, time.Since(start), hash.Sum(nil), UploadStream)
}

// finalizeUpload pulls the optional 16-byte remote MD5 out of the
// FILE_UPLOAD_END ACK payload and computes the verdict.
func (s *Storage) finalizeUpload(
	resp *protocol.Response, sent int64,
	elapsed time.Duration, localSum []byte, mode UploadMode,
) (UploadResult, error) {
	r := UploadResult{
		BytesSent: sent,
		Elapsed:   elapsed,
		SpeedKBs:  float64(sent) / elapsed.Seconds() / 1024.0,
		Mode:      mode,
	}
	copy(r.LocalMD5[:], localSum)
	if len(resp.Payload) >= 16 {
		copy(r.RemoteMD5[:], resp.Payload[:16])
		r.MD5Match = r.LocalMD5 == r.RemoteMD5
	}
	return r, nil
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
