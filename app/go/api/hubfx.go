package api

import (
	"scalefx/protocol/hubfx"
)

// HubFxApi provides HubFX master hub operations.
type HubFxApi struct{ apiClient }

// ─── Slave Commands ───

func (a *HubFxApi) SlaveList() ApiResult              { return a.sendQuery(hubfx.CmdSlaveList(), hubfx.SlaveListResp) }
func (a *HubFxApi) SlaveInit(slaveType byte) ApiResult { return a.sendACK(hubfx.CmdSlaveInit(slaveType)) }
func (a *HubFxApi) SlaveInfo(slaveType byte) ApiResult { return a.sendQuery(hubfx.CmdSlaveInfo(slaveType), hubfx.SlaveInfoResp) }

// SlaveEnum returns the slot-keyed registry table from the hub. Studio/CLI
// use this once on connect to populate per-slave routing slots.
func (a *HubFxApi) SlaveEnum() ApiResult {
	return a.sendQuery(hubfx.CmdSlaveEnumReq(), hubfx.SlaveEnumResp)
}

// ─── Audio Commands ───

func (a *HubFxApi) AudioPlay(ch, vol, output, loopMode byte, loopCount uint16, path string) ApiResult {
	return a.sendACK(hubfx.CmdAudioPlay(ch, vol, output, loopMode, loopCount, path))
}

func (a *HubFxApi) AudioStop(ch byte) ApiResult       { return a.sendACK(hubfx.CmdAudioStop(ch)) }
func (a *HubFxApi) AudioVolume(ch, vol byte) ApiResult { return a.sendACK(hubfx.CmdAudioVolume(ch, vol)) }
func (a *HubFxApi) AudioFade(ch byte) ApiResult        { return a.sendACK(hubfx.CmdAudioFade(ch)) }

func (a *HubFxApi) AudioQueue(ch, vol byte, loopCount uint16, behavior byte, path string) ApiResult {
	return a.sendACK(hubfx.CmdAudioQueue(ch, vol, loopCount, behavior, path))
}

func (a *HubFxApi) AudioQueueClear(ch byte) ApiResult { return a.sendACK(hubfx.CmdAudioQueueClear(ch)) }
func (a *HubFxApi) AudioStatus() ApiResult            { return a.sendQuery(hubfx.CmdAudioStatusReq(), hubfx.AudioStatusResp) }
func (a *HubFxApi) CodecStatus() ApiResult            { return a.sendQuery(hubfx.CmdCodecStatusReq(), hubfx.CodecStatusResp) }

// ─── Engine Commands ───

func (a *HubFxApi) EngineStart() ApiResult  { return a.sendACK(hubfx.CmdEngineStart()) }
func (a *HubFxApi) EngineStop() ApiResult   { return a.sendACK(hubfx.CmdEngineStop()) }
func (a *HubFxApi) EngineStatus() ApiResult { return a.sendQuery(hubfx.CmdEngineStatusReq(), hubfx.EngineStatusResp) }

// ─── Config Commands ───

func (a *HubFxApi) ConfigReload(path string) ApiResult { return a.sendACK(hubfx.CmdConfigReload(path)) }
func (a *HubFxApi) ConfigStatus() ApiResult            { return a.sendQuery(hubfx.CmdConfigStatus(), hubfx.ConfigStatusResp) }
func (a *HubFxApi) ConfigSave(path string) ApiResult   { return a.sendACK(hubfx.CmdConfigSave(path)) }

// ─── Storage Commands ───

func (a *HubFxApi) SdInit() ApiResult      { return a.sendACK(hubfx.CmdSdInit(0)) }
func (a *HubFxApi) SdStatus() ApiResult    { return a.sendQuery(hubfx.CmdSdStatusReq(), hubfx.SdStatusResp) }
func (a *HubFxApi) FlashStatus() ApiResult { return a.sendQuery(hubfx.CmdFlashStatusReq(), hubfx.FlashStatusReq) }

// ─── USB Commands ───

func (a *HubFxApi) UsbDevices() ApiResult { return a.sendQuery(hubfx.CmdUsbDevicesReq(), hubfx.UsbDevicesResp) }
func (a *HubFxApi) UsbReset() ApiResult   { return a.sendACK(hubfx.CmdUsbResetBus()) }
