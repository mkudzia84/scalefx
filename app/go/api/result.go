package api

import (
	"scalefx/protocol"
	"time"
)

// ApiResult is the standard return type for all API methods.
type ApiResult struct {
	OK       bool               // Success (ACK or expected response type)
	Error    string             // Error description (timeout, NACK message, etc.)
	Response *protocol.Response // Raw response (nil if transport error)
}

func apiOK(resp *protocol.Response) ApiResult {
	return ApiResult{OK: true, Response: resp}
}

func apiFail(msg string) ApiResult {
	return ApiResult{Error: msg}
}

func apiFailResp(resp *protocol.Response) ApiResult {
	return ApiResult{Error: resp.ErrorMessage(), Response: resp}
}

// UploadResult extends ApiResult with file transfer metrics.
type UploadResult struct {
	ApiResult
	BytesTransferred uint32
	Elapsed          time.Duration
	SpeedKBs         float64
	RemoteMD5        string
	LocalMD5         string
	MD5Match         bool
}

// UploadMode selects the file upload transfer mode.
type UploadMode byte

const (
	UploadSync   UploadMode = 0 // Per-chunk ACK with CRC retry
	UploadStream UploadMode = 3 // Raw binary streaming with segment-based ACKs
)

// LargeFileBatchThreshold is the size at which SD uploads switch from
// UploadSync to UploadStream in auto-mode. At ~44 KB/s sync vs ~465 KB/s
// batch on SD, anything above this finishes meaningfully faster in batch.
// Flash never uses batch — onboard LittleFS write speed is the bottleneck
// and batch's segment-ack pacing has no headroom there.
const LargeFileBatchThreshold = 64 * 1024

// PickUploadMode returns the best transfer mode for (target, size).
// Flash always uses UploadSync (batch is unreliable on LittleFS partitions).
// SD uses UploadStream above LargeFileBatchThreshold, UploadSync otherwise.
// `target` is the storage target byte (hubfx.StorageTargetFlash / Sd).
func PickUploadMode(target byte, size int) UploadMode {
	const flash byte = 1 // hubfx.StorageTargetFlash — avoid import cycle
	if target == flash {
		return UploadSync
	}
	if size >= LargeFileBatchThreshold {
		return UploadStream
	}
	return UploadSync
}
