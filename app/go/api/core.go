package api

import (
	"scalefx/protocol"
	"scalefx/protocol/core"
)

// CoreApi provides core protocol operations (init, status, reboot, etc.).
type CoreApi struct{ apiClient }

func NewCoreApi(conn *protocol.Connection) *CoreApi {
	return &CoreApi{apiClient{conn}}
}

func (a *CoreApi) Init() ApiResult                  { return a.sendQuery(core.CmdInit(), core.InitReady) }
func (a *CoreApi) InitMode(mode, flags byte) ApiResult { return a.sendQuery(core.CmdInitMode(mode, flags), core.InitReady) }
func (a *CoreApi) Identify() ApiResult              { return a.sendQuery(core.CmdIdentify(), core.Identify, core.InitReady) }
func (a *CoreApi) Shutdown() ApiResult              { return a.sendACK(core.CmdShutdown()) }
func (a *CoreApi) Status() ApiResult                { return a.sendQuery(core.CmdStatusReq(), core.Status) }
func (a *CoreApi) I2CScan() ApiResult               { return a.sendQuery(core.CmdI2cScan(), core.I2cScanRes) }
func (a *CoreApi) Keepalive() ApiResult             { return a.sendACK(core.CmdKeepalive()) }
func (a *CoreApi) DiagHistory(count byte) ApiResult { return a.sendACK(core.CmdDiagHistory(count)) }

// Reboot sends reboot command (fire-and-forget, no response expected).
func (a *CoreApi) Reboot() error { return a.conn.Send(core.CmdReboot()) }

// Bootsel sends BOOTSEL/DFU command (fire-and-forget).
func (a *CoreApi) Bootsel() error { return a.conn.Send(core.CmdBootsel()) }
