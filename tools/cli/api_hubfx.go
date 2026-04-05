package main

// HubFxApi provides HubFX master hub operations.
type HubFxApi struct{ apiClient }

// NewHubFxApi creates a HubFxApi bound to the given connection.
func NewHubFxApi(conn *Connection) *HubFxApi {
	return &HubFxApi{apiClient{conn}}
}

// ─── Slave Commands ───

func (a *HubFxApi) SlaveList() ApiResult              { return a.sendQuery(CmdHubSlaveList(), HubSLAVE_LIST_RESP) }
func (a *HubFxApi) SlaveInit(slaveType byte) ApiResult { return a.sendACK(CmdHubSlaveInit(slaveType)) }
func (a *HubFxApi) SlaveInfo(slaveType byte) ApiResult { return a.sendQuery(CmdHubSlaveInfo(slaveType), HubSLAVE_INFO_RESP) }

// ─── Audio Commands ───

func (a *HubFxApi) AudioPlay(ch, vol, output, loopMode byte, loopCount uint16, path string) ApiResult {
	return a.sendACK(CmdHubAudioPlay(ch, vol, output, loopMode, loopCount, path))
}

func (a *HubFxApi) AudioStop(ch byte) ApiResult        { return a.sendACK(CmdHubAudioStop(ch)) }
func (a *HubFxApi) AudioVolume(ch, vol byte) ApiResult  { return a.sendACK(CmdHubAudioVolume(ch, vol)) }
func (a *HubFxApi) AudioFade(ch byte) ApiResult         { return a.sendACK(CmdHubAudioFade(ch)) }

func (a *HubFxApi) AudioQueue(ch, vol byte, loopCount uint16, behavior byte, path string) ApiResult {
	return a.sendACK(CmdHubAudioQueue(ch, vol, loopCount, behavior, path))
}

func (a *HubFxApi) AudioQueueClear(ch byte) ApiResult { return a.sendACK(CmdHubAudioQueueClear(ch)) }
func (a *HubFxApi) AudioStatus() ApiResult            { return a.sendQuery(CmdHubAudioStatusReq(), HubAUDIO_STATUS_RESP) }
func (a *HubFxApi) CodecStatus() ApiResult            { return a.sendQuery(CmdHubCodecStatusReq(), HubCODEC_STATUS_RESP) }

// ─── Engine Commands ───

func (a *HubFxApi) EngineStart() ApiResult  { return a.sendACK(CmdHubEngineStart()) }
func (a *HubFxApi) EngineStop() ApiResult   { return a.sendACK(CmdHubEngineStop()) }
func (a *HubFxApi) EngineStatus() ApiResult { return a.sendQuery(CmdHubEngineStatusReq(), HubENGINE_STATUS_RESP) }

// ─── Config Commands ───

func (a *HubFxApi) ConfigReload(path string) ApiResult { return a.sendACK(CmdHubConfigReload(path)) }
func (a *HubFxApi) ConfigStatus() ApiResult            { return a.sendQuery(CmdHubConfigStatus(), HubCONFIG_STATUS_RESP) }
func (a *HubFxApi) ConfigSave(path string) ApiResult   { return a.sendACK(CmdHubConfigSave(path)) }

// ─── Storage Commands ───

func (a *HubFxApi) SdInit() ApiResult     { return a.sendACK(CmdHubSDInit(0)) }
func (a *HubFxApi) SdStatus() ApiResult   { return a.sendQuery(CmdHubSDStatusReq(), HubSD_STATUS_RESP) }
func (a *HubFxApi) FlashStatus() ApiResult { return a.sendQuery(CmdHubFlashStatusReq(), HubFLASH_STATUS_REQ) }

// ─── USB Commands ───

func (a *HubFxApi) UsbDevices() ApiResult { return a.sendQuery(CmdHubUSBDevicesReq(), HubUSB_DEVICES_RESP) }
func (a *HubFxApi) UsbReset() ApiResult   { return a.sendACK(CmdHubUSBResetBus()) }
