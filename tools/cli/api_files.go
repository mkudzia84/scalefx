package main

import (
	"crypto/md5"
	"encoding/hex"
	"fmt"
	"time"
)

const (
	uploadChunkSize  = 2044  // MAX_PAYLOAD_SIZE(2048) - 4 (seq+crc header)
	uploadMaxRetries = 3
)

// FileApi provides storage file operations (SD card, flash).
type FileApi struct{ apiClient }

// NewFileApi creates a FileApi bound to the given connection.
func NewFileApi(conn *Connection) *FileApi {
	return &FileApi{apiClient{conn}}
}

// List retrieves a directory listing as text.
func (a *FileApi) List(target byte, path string) (string, error) {
	return a.sendStream(CmdHubFileList(path, target), 10*time.Second)
}

// Tree retrieves a directory tree as text.
func (a *FileApi) Tree(target byte, path string) (string, error) {
	return a.sendStream(CmdHubFileTree(path, target), 30*time.Second)
}

// Delete removes a file or directory.
func (a *FileApi) Delete(target byte, path string) ApiResult {
	return a.sendACK(CmdHubFileDelete(path, target))
}

// Mkdir creates a directory.
func (a *FileApi) Mkdir(target byte, path string) ApiResult {
	return a.sendACK(CmdHubFileMkdir(path, target))
}

// Info queries file/directory metadata.
func (a *FileApi) Info(target byte, path string) ApiResult {
	return a.sendQuery(CmdHubFileInfo(path, target), HubFILE_INFO_RESP)
}

// Cat retrieves file contents as text (via stream download).
func (a *FileApi) Cat(target byte, path string) (string, error) {
	result, err := a.sendStreamBinary(CmdHubFileDownload(path, target), 30*time.Second)
	if err != nil {
		return "", err
	}
	return string(result.Data), nil
}

// Download retrieves file contents as binary (via stream download).
func (a *FileApi) Download(target byte, path string, timeout time.Duration) (*StreamResult, error) {
	return a.sendStreamBinary(CmdHubFileDownload(path, target), timeout)
}

// CancelUpload sends an upload cancel command.
func (a *FileApi) CancelUpload() ApiResult {
	return a.sendACK(CmdHubFileUploadCancel())
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
	resp, err := a.conn.SendExpectACK(CmdHubFileUploadBegin(remotePath, fileSize, target, 0))
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

	for offset < len(data) {
		end := offset + uploadChunkSize
		if end > len(data) {
			end = len(data)
		}
		chunk := data[offset:end]
		localHash.Write(chunk)

		sent := false
		for retry := 0; retry < uploadMaxRetries; retry++ {
			resp, err := a.conn.SendExpectACK(CmdHubFileUploadData(seq, chunk))
			if err != nil {
				a.conn.SendExpectACK(CmdHubFileUploadCancel())
				return UploadResult{ApiResult: apiFail(fmt.Sprintf("upload error at segment %d: %v", seq, err))}
			}
			if resp.IsACK() {
				sent = true
				break
			}
			// CRC error (0x02 = NOT_INITIALIZED repurposed) → retry
			if resp.IsNACK() && len(resp.Payload) > 0 && resp.Payload[0] == ErrCRC_ERROR {
				continue
			}
			// Other error → abort
			a.conn.SendExpectACK(CmdHubFileUploadCancel())
			return UploadResult{ApiResult: apiFailResp(resp)}
		}
		if !sent {
			a.conn.SendExpectACK(CmdHubFileUploadCancel())
			return UploadResult{ApiResult: apiFail(fmt.Sprintf("max retries on segment %d", seq))}
		}

		offset = end
		seq++
		if progress != nil {
			progress(offset, len(data))
		}
	}

	// End upload
	resp, err = a.conn.SendExpectACK(CmdHubFileUploadEnd())
	elapsed := time.Since(start)
	speed := float64(fileSize) / elapsed.Seconds() / 1024

	if err != nil {
		return UploadResult{ApiResult: apiFail(fmt.Sprintf("upload end: %v", err))}
	}
	if !resp.IsACK() {
		a.conn.SendExpectACK(CmdHubFileUploadCancel())
		return UploadResult{ApiResult: apiFailResp(resp)}
	}

	return a.buildUploadResult(resp, fileSize, elapsed, speed, localHash.Sum(nil))
}

// uploadStream implements raw binary streaming upload with segment-based ACKs.
// Data is sent as unframed bytes (no COBS) directly to the serial port.
// The server sends a COBS-framed FILE_UPLOAD_PROGRESS after each 512 KB segment.
func (a *FileApi) uploadStream(target byte, remotePath string, data []byte,
	progress func(sent, total int)) UploadResult {

	fileSize := uint32(len(data))
	const writeChunk = 32768 // 32 KB per serial write call

	// Begin upload (mode 3 = stream)
	resp, err := a.conn.SendExpectACK(CmdHubFileUploadBegin(remotePath, fileSize, target, byte(UploadStream)))
	if err != nil {
		return UploadResult{ApiResult: apiFail(fmt.Sprintf("upload begin: %v", err))}
	}
	if !resp.IsACK() {
		return UploadResult{ApiResult: apiFailResp(resp)}
	}

	// Parse stream params from ACK payload [segment_size:u32LE][segment_count:u16LE]
	segmentSize := uint32(524288) // default 512 KB
	if len(resp.Payload) >= 6 {
		segmentSize = ReadU32LE(resp.Payload, 0)
	}

	// Register async filter to intercept UPLOAD_PROGRESS packets (tag=0, type=0xB0)
	progressCh := make(chan *Response, 16)
	a.conn.RegisterAsyncFilter(HubFILE_UPLOAD_PROGRESS, progressCh)
	defer a.conn.UnregisterAsyncFilter(HubFILE_UPLOAD_PROGRESS)

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
					// Wait proportional to ring fill above 50% to let SD writer drain
					delayMs := (ringPct - 50) * 60 // ~60ms per percent
					time.Sleep(time.Duration(delayMs) * time.Millisecond)
				}
			}
		case <-time.After(15 * time.Second):
			return UploadResult{ApiResult: apiFail("timeout waiting for segment ACK")}
		}
	}

	// End upload
	resp, err = a.conn.SendExpectACK(CmdHubFileUploadEnd())
	elapsed := time.Since(start)
	speed := float64(fileSize) / elapsed.Seconds() / 1024

	if err != nil {
		return UploadResult{ApiResult: apiFail(fmt.Sprintf("upload end: %v", err))}
	}
	if !resp.IsACK() {
		a.conn.SendExpectACK(CmdHubFileUploadCancel())
		return UploadResult{ApiResult: apiFailResp(resp)}
	}

	return a.buildUploadResult(resp, fileSize, elapsed, speed, localHash.Sum(nil))
}

// buildUploadResult parses MD5 and CRC errors from the UPLOAD_END ACK payload.
func (a *FileApi) buildUploadResult(resp *Response, fileSize uint32, elapsed time.Duration, speed float64, localMD5 []byte) UploadResult {
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
