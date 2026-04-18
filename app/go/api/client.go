package api

import (
	"scalefx/protocol"
	"time"
)

// Client is the top-level API facade. It aggregates every controller-specific
// API into a single entry point so callers create one Client on connect and
// access sub-APIs via fields (Facade pattern).
type Client struct {
	Core        *CoreApi
	GunFx       *GunFxApi
	LightFx     *LightFxApi
	GearControl *GearControlApi
	HubFx       *HubFxApi
	Files       *FileApi
	conn        *protocol.Connection
}

// NewClient creates all sub-APIs from a single Connection.
func NewClient(conn *protocol.Connection) *Client {
	base := apiClient{conn}
	return &Client{
		Core:        &CoreApi{base},
		GunFx:       &GunFxApi{base},
		LightFx:     &LightFxApi{base},
		GearControl: &GearControlApi{base},
		HubFx:       &HubFxApi{base},
		Files:       &FileApi{apiClient: base, peerMaxPayload: PicoMaxPayload},
		conn:        conn,
	}
}

// Connection returns the underlying protocol connection.
func (c *Client) Connection() *protocol.Connection { return c.conn }

// apiClient is the base for all API types. Wraps a Connection for typed methods.
type apiClient struct {
	conn *protocol.Connection
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
func (a *apiClient) sendQuery(pkt []byte, expectedTypes ...protocol.PacketType) ApiResult {
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
func (a *apiClient) sendStreamBinary(pkt []byte, timeout time.Duration) (*protocol.StreamResult, error) {
	return a.conn.SendAndReceiveStream(pkt, timeout)
}
