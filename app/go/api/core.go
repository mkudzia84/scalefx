package api

import (
	"scalefx/protocol/core"
)

// CoreApi provides core protocol operations (init, status, reboot, etc.).
type CoreApi struct{ apiClient }

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

// BatteryConfig adjusts battery sensor settings on any board with battery
// monitoring (handled by BatteryServerT). chemistry is a wire-format byte
// (core.ChemistryLiPo / LiIon / NiMH); cellCount == 0 = re-arm auto-detect.
func (a *CoreApi) BatteryConfig(chemistry, cellCount byte) ApiResult {
	return a.sendACK(core.CmdBatteryConfig(chemistry, cellCount))
}
