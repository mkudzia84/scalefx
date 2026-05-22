package client

import (
	"encoding/binary"
	"fmt"

	"scalefx/protocol/core"
)

// Config is the master-side handle for the YAML config store on the
// connected board.  Backed by `sfx_config::ConfigServicePolicy` —
// path-routed across every registered ConfigStore.  See
// `instructions/19-HUBFX-CONFIG-SCHEMA.md` for the on-device layout.
type Config struct{ c *Client }

// ConfigStatus is the decoded CONFIG_STATUS_RESP payload.  Aggregated
// across every registered ConfigStore by the firmware: `Loaded` is
// "all stores loaded", `FileSize` is the sum, `ValidOk` is the
// per-store validate() AND.
type ConfigStatus struct {
	Loaded   bool   `json:"loaded"`
	FileSize uint16 `json:"fileSize"`
	ValidOk  bool   `json:"validOk"`
}

// Reload re-reads every registered config store, fires each store's
// onLoaded callback (which fans the parsed struct out to the effect
// services), then ACKs.  Cheap — kilobytes of work, no UART traffic
// for hub-local apply.
func (g *Config) Reload() error {
	return g.c.sendExpectACK(core.CmdConfigReload())
}

// ReloadPath reloads only the matching store (e.g. "/hubfx.yaml").
func (g *Config) ReloadPath(path string) error {
	return g.c.sendExpectACK(core.CmdConfigReloadPath(path))
}

// Status returns the aggregate config-store state.
func (g *Config) Status() (ConfigStatus, error) {
	resp, err := g.c.sendForResp(core.CmdConfigStatus(), core.ConfigStatusResp)
	if err != nil {
		return ConfigStatus{}, err
	}
	if len(resp.Payload) < 4 {
		return ConfigStatus{}, fmt.Errorf("config status: short payload (%d < 4)", len(resp.Payload))
	}
	return ConfigStatus{
		Loaded:   resp.Payload[0] != 0,
		FileSize: binary.LittleEndian.Uint16(resp.Payload[1:3]),
		ValidOk:  resp.Payload[3] != 0,
	}, nil
}

// SavePath flushes the named store's in-memory state back to its file
// on flash.  Atomic per-file (temp + rename) on the firmware side.
func (g *Config) SavePath(path string) error {
	return g.c.sendExpectACK(core.CmdConfigSavePath(path))
}
