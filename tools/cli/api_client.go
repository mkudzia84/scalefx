package main

import "time"

// apiClient is the base for all API types. Wraps a Connection for typed methods.
type apiClient struct {
	conn *Connection
}

// sendACK sends a packet and expects ACK/NACK.
func (a *apiClient) sendACK(pkt []byte) ApiResult {
	resp, err := a.conn.SendExpectACK(pkt)
	if err != nil {
		return apiFail(err.Error())
	}
	if resp.IsACK() {
		return apiOK(resp)
	}
	return apiFailResp(resp)
}

// sendQuery sends a packet and expects one of the given response types.
func (a *apiClient) sendQuery(pkt []byte, expectedTypes ...byte) ApiResult {
	resp, err := a.conn.SendAndWait(pkt)
	if err != nil {
		return apiFail(err.Error())
	}
	for _, t := range expectedTypes {
		if resp.PacketType == t {
			return apiOK(resp)
		}
	}
	if resp.IsNACK() {
		return apiFailResp(resp)
	}
	// Unexpected type — return with response for caller to inspect
	return apiOK(resp)
}

// sendStream sends a packet, collects a streamed response, and returns text.
func (a *apiClient) sendStream(pkt []byte, timeout time.Duration) (string, error) {
	result, err := a.conn.SendAndReceiveStream(pkt, timeout)
	if err != nil {
		return "", err
	}
	return string(result.Data), nil
}

// sendStreamBinary sends a packet, collects a streamed response as binary.
func (a *apiClient) sendStreamBinary(pkt []byte, timeout time.Duration) (*StreamResult, error) {
	return a.conn.SendAndReceiveStream(pkt, timeout)
}
