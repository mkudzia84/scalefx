package api

import (
	"crypto/md5"
	"encoding/hex"
	"fmt"
	"scalefx/protocol"
	"scalefx/protocol/core"
	"scalefx/protocol/hubfx"
	"time"
)

// Upload chunk-size constants.
//
// FILE_UPLOAD_DATA payload layout: [seq:u16LE][crc16:u16LE][data:N] — the
// 4-byte envelope counts against MAX_PAYLOAD_SIZE. Anything larger than the
// peer's capacity is silently dropped by the COBS framer (framing error),
// manifesting as a segment-0 ACK timeout.
//
//	Pico  (RP2040/RP2350): MAX_PAYLOAD_SIZE = 512  → 508 bytes of data
//	ESP32-S3:              MAX_PAYLOAD_SIZE = 2048 → 2044 bytes of data
//
// Use UploadChunkSize (safe Pico default) unless the peer capacity is known —
// Client.SetPeerMaxPayload() upgrades to the ESP32 size once IDENTIFY/INIT
// reports the controller type.
const (
	PicoMaxPayload   = 512  // RP2040/RP2350 COBS RX buffer
	Esp32MaxPayload  = 2048 // ESP32-S3 COBS RX buffer
	UploadHeaderSize = 4    // seq:u16LE + crc16:u16LE
	UploadChunkSize  = PicoMaxPayload - UploadHeaderSize
)

const uploadMaxRetries = 3

// FileApi provides storage file operations (SD card, flash).
//
// peerMaxPayload defaults to PicoMaxPayload (safe for any board). Call
// SetPeerMaxPayload after detecting the controller type to unlock the ESP32
// chunk size for sync uploads.
type FileApi struct {
	apiClient
	peerMaxPayload int
}

func NewFileApi(conn *protocol.Connection) *FileApi {
	return &FileApi{apiClient: apiClient{conn}, peerMaxPayload: PicoMaxPayload}
}

// SetPeerMaxPayload updates the assumed COBS RX capacity of the connected
// peer, which controls sync-upload chunking. Safe to call at any time;
// clamps to [PicoMaxPayload, Esp32MaxPayload].
func (a *FileApi) SetPeerMaxPayload(size int) {
	if size < PicoMaxPayload {
		size = PicoMaxPayload
	}
	if size > Esp32MaxPayload {
		size = Esp32MaxPayload
	}
	a.peerMaxPayload = size
}

// uploadChunkSize returns the usable data bytes per FILE_UPLOAD_DATA packet
// for the current peer capacity.
func (a *FileApi) uploadChunkSize() int {
	if a.peerMaxPayload <= 0 {
		return UploadChunkSize
	}
	return a.peerMaxPayload - UploadHeaderSize
}

// List retrieves a directory listing as text.
func (a *FileApi) List(target byte, path string) (string, error) {
	return a.sendStream(hubfx.CmdFileList(path, target), 10*time.Second)
}

// Tree retrieves a directory tree as text.
func (a *FileApi) Tree(target byte, path string) (string, error) {
	return a.sendStream(hubfx.CmdFileTree(path, target), 30*time.Second)
}

// Delete removes a file, an empty directory, or a whole tree.
// Set recursive=true to delete a non-empty directory (mirrors `rm -rf`).
func (a *FileApi) Delete(target byte, path string, recursive bool) ApiResult {
	var flags byte
	if recursive {
		flags |= hubfx.DeleteFlagRecursive
	}
	return a.sendACK(hubfx.CmdFileDelete(path, target, flags))
}

// Mkdir creates a directory.
// Set createParents=true for `mkdir -p` semantics (creates missing ancestors,
// idempotent when the final directory already exists).
func (a *FileApi) Mkdir(target byte, path string, createParents bool) ApiResult {
	var flags byte
	if createParents {
		flags |= hubfx.MkdirFlagParents
	}
	return a.sendACK(hubfx.CmdFileMkdir(path, target, flags))
}

// Info queries file/directory metadata.
func (a *FileApi) Info(target byte, path string) ApiResult {
	return a.sendQuery(hubfx.CmdFileInfo(path, target), hubfx.FileInfoResp)
}

// Cat retrieves file contents as text (via stream download).
func (a *FileApi) Cat(target byte, path string) (string, error) {
	result, err := a.sendStreamBinary(hubfx.CmdFileDownload(path, target), 30*time.Second)
	if err != nil {
		return "", err
	}
	return string(result.Data), nil
}

// Download retrieves file contents as binary (via stream download).
func (a *FileApi) Download(target byte, path string, timeout time.Duration) (*protocol.StreamResult, error) {
	return a.sendStreamBinary(hubfx.CmdFileDownload(path, target), timeout)
}

// CancelUpload sends an upload cancel command.
func (a *FileApi) CancelUpload() ApiResult {
	return a.sendACK(hubfx.CmdFileUploadCancel())
}

// Upload sends file data to the device using the specified transfer mode.
// The progress callback receives (bytesSent, totalBytes).
func (a *FileApi) Upload(target byte, remotePath string, data []byte, mode UploadMode,
	progress func(sent, total int)) UploadResult {

	switch mode {
	case UploadSync:
		return a.uploadSync(target, remotePath, data, progress)
	case UploadStream:
		return a.uploadStream(target, remotePath, data, progress)
	default:
		return UploadResult{ApiResult: apiFail(fmt.Sprintf("unsupported upload mode: %d", mode))}
	}
}

// uploadSync implements per-chunk ACK upload with CRC retry.
func (a *FileApi) uploadSync(target byte, remotePath string, data []byte,
	progress func(sent, total int)) UploadResult {

	fileSize := uint32(len(data))

	// Begin upload (mode 0 = sync)
	resp, err := a.conn.SendExpectACK(hubfx.CmdFileUploadBegin(remotePath, fileSize, target, 0))
	if err != nil {
		return UploadResult{ApiResult: apiFail(fmt.Sprintf("upload begin: %v", err))}
	}
	if !resp.IsACK() {
		return UploadResult{ApiResult: apiFailResp(resp)}
	}

	start := time.Now()
	offset := 0
	seq := uint16(0)
	localHash := md5.New()
	chunkSize := a.uploadChunkSize()

	for offset < len(data) {
		end := offset + chunkSize
		if end > len(data) {
			end = len(data)
		}
		chunk := data[offset:end]
		localHash.Write(chunk)

		sent := false
		for retry := 0; retry < uploadMaxRetries; retry++ {
			resp, err := a.conn.SendExpectACK(hubfx.CmdFileUploadData(seq, chunk))
			if err != nil {
				a.conn.SendExpectACK(hubfx.CmdFileUploadCancel())
				return UploadResult{ApiResult: apiFail(fmt.Sprintf("upload error at segment %d: %v", seq, err))}
			}
			if resp.IsACK() {
				sent = true
				break
			}
			// CRC error → retry
			if resp.IsNACK() && len(resp.Payload) > 0 && protocol.ErrorCode(resp.Payload[0]) == core.ErrCrcError {
				continue
			}
			// Other error → abort
			a.conn.SendExpectACK(hubfx.CmdFileUploadCancel())
			return UploadResult{ApiResult: apiFailResp(resp)}
		}
		if !sent {
			a.conn.SendExpectACK(hubfx.CmdFileUploadCancel())
			return UploadResult{ApiResult: apiFail(fmt.Sprintf("max retries on segment %d", seq))}
		}

		offset = end
		seq++
		if progress != nil {
			progress(offset, len(data))
		}
	}

	// End upload
	resp, err = a.conn.SendExpectACK(hubfx.CmdFileUploadEnd())
	elapsed := time.Since(start)
	speed := float64(fileSize) / elapsed.Seconds() / 1024

	if err != nil {
		return UploadResult{ApiResult: apiFail(fmt.Sprintf("upload end: %v", err))}
	}
	if !resp.IsACK() {
		a.conn.SendExpectACK(hubfx.CmdFileUploadCancel())
		return UploadResult{ApiResult: apiFailResp(resp)}
	}

	return a.buildUploadResult(resp, fileSize, elapsed, speed, localHash.Sum(nil))
}

// uploadStream implements raw binary streaming upload with segment-based ACKs.
func (a *FileApi) uploadStream(target byte, remotePath string, data []byte,
	progress func(sent, total int)) UploadResult {

	fileSize := uint32(len(data))
	const writeChunk = 32768 // 32 KB per serial write call

	// Begin upload (mode 3 = stream)
	resp, err := a.conn.SendExpectACK(hubfx.CmdFileUploadBegin(remotePath, fileSize, target, byte(UploadStream)))
	if err != nil {
		return UploadResult{ApiResult: apiFail(fmt.Sprintf("upload begin: %v", err))}
	}
	if !resp.IsACK() {
		return UploadResult{ApiResult: apiFailResp(resp)}
	}

	// Parse stream params from ACK payload [segment_size:u32LE][segment_count:u16LE]
	segmentSize := uint32(524288) // default 512 KB
	if len(resp.Payload) >= 6 {
		segmentSize = protocol.ReadU32LE(resp.Payload, 0)
	}

	// Register async filter to intercept UPLOAD_PROGRESS packets
	progressCh := make(chan *protocol.Response, 16)
	a.conn.RegisterAsyncFilter(hubfx.FileUploadProgress, progressCh)
	defer a.conn.UnregisterAsyncFilter(hubfx.FileUploadProgress)

	start := time.Now()
	offset := 0
	localHash := md5.New()
	localHash.Write(data) // compute MD5 over entire file up front

	for offset < len(data) {
		// Determine this segment's size
		remaining := len(data) - offset
		thisSeg := int(segmentSize)
		if thisSeg > remaining {
			thisSeg = remaining
		}

		// Send raw binary in write-sized chunks
		segSent := 0
		for segSent < thisSeg {
			chunkLen := writeChunk
			if chunkLen > thisSeg-segSent {
				chunkLen = thisSeg - segSent
			}
			chunk := data[offset : offset+chunkLen]
			if err := a.conn.SendRaw(chunk); err != nil {
				return UploadResult{ApiResult: apiFail(fmt.Sprintf("write error at offset %d: %v", offset, err))}
			}
			offset += chunkLen
			segSent += chunkLen

			if progress != nil {
				progress(offset, len(data))
			}
		}

		// Flush serial TX buffer before waiting for segment ACK
		a.conn.FlushOutput()

		// Wait for segment ACK from server
		select {
		case ack := <-progressCh:
			// Flow control: throttle based on ring buffer fill level
			if len(ack.Payload) >= 7 {
				ringPct := int(ack.Payload[6])
				if ringPct > 50 {
					delayMs := (ringPct - 50) * 60
					time.Sleep(time.Duration(delayMs) * time.Millisecond)
				}
			}
		case <-time.After(15 * time.Second):
			return UploadResult{ApiResult: apiFail("timeout waiting for segment ACK")}
		}
	}

	// End upload — allow up to 60s for server to drain ring buffer (dual-core
	// policies can have 2+ MB in flight at the moment the last data packet is
	// sent; draining that to SD takes several seconds on top of the close +
	// MD5 finalize).
	resp, err = a.conn.SendExpectACKTimeout(hubfx.CmdFileUploadEnd(), 60*time.Second)
	elapsed := time.Since(start)
	speed := float64(fileSize) / elapsed.Seconds() / 1024

	if err != nil {
		return UploadResult{ApiResult: apiFail(fmt.Sprintf("upload end: %v", err))}
	}
	if !resp.IsACK() {
		a.conn.SendExpectACK(hubfx.CmdFileUploadCancel())
		return UploadResult{ApiResult: apiFailResp(resp)}
	}

	return a.buildUploadResult(resp, fileSize, elapsed, speed, localHash.Sum(nil))
}

// buildUploadResult parses MD5 and CRC errors from the UPLOAD_END ACK payload.
func (a *FileApi) buildUploadResult(resp *protocol.Response, fileSize uint32, elapsed time.Duration, speed float64, localMD5 []byte) UploadResult {
	result := UploadResult{
		ApiResult:        apiOK(resp),
		BytesTransferred: fileSize,
		Elapsed:          elapsed,
		SpeedKBs:         speed,
	}

	if len(resp.Payload) >= 16 {
		result.RemoteMD5 = hex.EncodeToString(resp.Payload[:16])
		result.LocalMD5 = hex.EncodeToString(localMD5)
		result.MD5Match = result.RemoteMD5 == result.LocalMD5
	}

	return result
}
