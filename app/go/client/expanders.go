package client

import (
	expp "scalefx/protocol/expanders"
)

// Expanders exposes the HubFX-side expander enumeration + system-info
// surface (0x80..0x87).
type Expanders struct{ c *Client }

// ExpanderEntry is a decoded EXPANDER_LIST_RESP row — one tracked USB
// expander (kind + USB info, plus IDENTIFY-derived spec when present).
type ExpanderEntry = expp.ExpanderEntry

// HubInfo mirrors the hub block of EXPANDER_SYSTEM_INFO_RESP.
type HubInfo = expp.HubInfo

// SystemInfo bundles the hub identity + every expander's identity in a
// single round-trip — the canonical "show me everything" response.
type SystemInfo = expp.SystemInfo

// List sends EXPANDER_LIST_REQ and decodes the response.
func (e *Expanders) List() ([]ExpanderEntry, error) {
	resp, err := e.c.sendForResp(expp.CmdExpanderListReq(), expp.ExpanderListResp)
	if err != nil {
		return nil, err
	}
	return expp.DecodeExpanderList(resp.Payload)
}

// SystemInfo sends EXPANDER_SYSTEM_INFO_REQ and decodes the response.
// One round-trip that yields the hub identity + every connected
// expander's identity.
func (e *Expanders) SystemInfo() (SystemInfo, error) {
	resp, err := e.c.sendForResp(expp.CmdExpanderSystemInfoReq(), expp.ExpanderSystemInfoResp)
	if err != nil {
		return SystemInfo{}, err
	}
	return expp.DecodeSystemInfo(resp.Payload)
}

// FindByGUID returns the live expander entry matching `guid`, or nil
// when there's no live slot with that GUID.  Convenience wrapper
// around List().
func (e *Expanders) FindByGUID(guid string) (*ExpanderEntry, error) {
	list, err := e.List()
	if err != nil {
		return nil, err
	}
	for i := range list {
		if list[i].GUID == guid {
			return &list[i], nil
		}
	}
	return nil, nil
}
