package main

import "time"

// ApiResult is the standard return type for all API methods.
type ApiResult struct {
	OK       bool      // Success (ACK or expected response type)
	Error    string    // Error description (timeout, NACK message, etc.)
	Response *Response // Raw response (nil if transport error)
}

func apiOK(resp *Response) ApiResult {
	return ApiResult{OK: true, Response: resp}
}

func apiFail(msg string) ApiResult {
	return ApiResult{Error: msg}
}

func apiFailResp(resp *Response) ApiResult {
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
