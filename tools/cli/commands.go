package main

// ScaleFX CLI - Command Builders
// Mirrors tests/framework/commands.py — builds COBS-encoded packets.

// ─── Core Commands ───

func CmdInit() []byte        { return BuildPacket(CoreINIT, nil, 0) }
func CmdShutdown() []byte    { return BuildPacket(CoreSHUTDOWN, nil, 0) }
func CmdKeepalive() []byte   { return BuildPacket(CoreKEEPALIVE, nil, 0) }
func CmdReboot() []byte      { return BuildPacket(CoreREBOOT, nil, 0) }
func CmdBootsel() []byte     { return BuildPacket(CoreBOOTSEL, nil, 0) }
func CmdStatusReq() []byte   { return BuildPacket(CoreSTATUS_REQ, nil, 0) }
func CmdIdentify() []byte    { return BuildPacket(CoreIDENTIFY, nil, 0) }
func CmdI2CScan() []byte     { return BuildPacket(CoreI2C_SCAN, nil, 0) }
func CmdDiagHistory(count byte) []byte {
	if count == 0 {
		return BuildPacket(CoreDIAG_HISTORY, nil, 0)
	}
	return BuildPacket(CoreDIAG_HISTORY, []byte{count}, 0)
}

// ─── GunFX Commands ───

func CmdGfxTriggerOn(rpm uint16) []byte {
	return BuildPacket(GfxTRIGGER_ON, U16LE(rpm), 0)
}

func CmdGfxTriggerOff(delay_ms uint16) []byte {
	return BuildPacket(GfxTRIGGER_OFF, U16LE(delay_ms), 0)
}

func CmdGfxServoSet(id byte, pulse_us uint16) []byte {
	payload := []byte{id}
	payload = append(payload, U16LE(pulse_us)...)
	return BuildPacket(GfxSERVO_SET, payload, 0)
}

func CmdGfxServoSettings(id byte, minPulse, maxPulse, speed, accel, decel uint16) []byte {
	payload := []byte{id}
	payload = append(payload, U16LE(minPulse)...)
	payload = append(payload, U16LE(maxPulse)...)
	payload = append(payload, U16LE(speed)...)
	payload = append(payload, U16LE(accel)...)
	payload = append(payload, U16LE(decel)...)
	return BuildPacket(GfxSERVO_SETTINGS, payload, 0)
}

func CmdGfxServoRecoil(id byte, jerk_us, variance_us uint16) []byte {
	payload := []byte{id}
	payload = append(payload, U16LE(jerk_us)...)
	payload = append(payload, U16LE(variance_us)...)
	return BuildPacket(GfxSERVO_RECOIL, payload, 0)
}

func CmdGfxSmokeHeat(on bool) []byte {
	v := byte(0)
	if on {
		v = 1
	}
	return BuildPacket(GfxSMOKE_HEAT, []byte{v}, 0)
}

func CmdGfxSmokeSettings(pulsing bool, speed, high, low byte, pulse_ms, spindown_ms uint16) []byte {
	p := byte(0)
	if pulsing {
		p = 1
	}
	payload := []byte{p, speed, high, low}
	payload = append(payload, U16LE(pulse_ms)...)
	payload = append(payload, U16LE(spindown_ms)...)
	return BuildPacket(GfxSMOKE_SETTINGS, payload, 0)
}

func CmdGfxSmokeReset() []byte {
	return BuildPacket(GfxSMOKE_RESET, nil, 0)
}

func CmdGfxSmokeCurrentLimit(target byte, limit_mA uint16) []byte {
	payload := []byte{target}
	payload = append(payload, U16LE(limit_mA)...)
	return BuildPacket(GfxSMOKE_CURRENT_LIMIT, payload, 0)
}

// ─── GearControl Commands ───

func CmdGcDeploy(gearID byte) []byte {
	return BuildPacket(GcGEAR_DEPLOY, []byte{gearID}, 0)
}

func CmdGcRetract(gearID byte) []byte {
	return BuildPacket(GcGEAR_RETRACT, []byte{gearID}, 0)
}

func CmdGcStop(gearID byte) []byte {
	return BuildPacket(GcGEAR_STOP, []byte{gearID}, 0)
}

func CmdGcAll(action byte) []byte {
	return BuildPacket(GcGEAR_ALL, []byte{action}, 0)
}

func CmdGcServoSet(id byte, pulse_us uint16) []byte {
	payload := []byte{id}
	payload = append(payload, U16LE(pulse_us)...)
	return BuildPacket(GcSERVO_SET, payload, 0)
}

func CmdGcServoSettings(id byte, minPulse, maxPulse, speed, accel, decel uint16) []byte {
	payload := []byte{id}
	payload = append(payload, U16LE(minPulse)...)
	payload = append(payload, U16LE(maxPulse)...)
	payload = append(payload, U16LE(speed)...)
	payload = append(payload, U16LE(accel)...)
	payload = append(payload, U16LE(decel)...)
	return BuildPacket(GcSRV_SETTINGS, payload, 0)
}

func CmdGcGearConfig(gearID byte, flags byte, stall_mA uint16, timeout_ms uint16) []byte {
	payload := []byte{gearID, flags}
	payload = append(payload, U16LE(stall_mA)...)
	payload = append(payload, U16LE(timeout_ms)...)
	return BuildPacket(GcGEAR_CONFIG, payload, 0)
}

func CmdGcDoorConfig(gearID byte, open0, close0, open1, close1 uint16) []byte {
	payload := []byte{gearID}
	payload = append(payload, U16LE(open0)...)
	payload = append(payload, U16LE(close0)...)
	payload = append(payload, U16LE(open1)...)
	payload = append(payload, U16LE(close1)...)
	return BuildPacket(GcDOOR_CONFIG, payload, 0)
}

func CmdGcYawConfig(gearID byte, neutral, min, max uint16) []byte {
	payload := []byte{gearID}
	payload = append(payload, U16LE(neutral)...)
	payload = append(payload, U16LE(min)...)
	payload = append(payload, U16LE(max)...)
	return BuildPacket(GcYAW_CONFIG, payload, 0)
}

func CmdGcYawInput(position_us uint16) []byte {
	return BuildPacket(GcYAW_INPUT, U16LE(position_us), 0)
}

func CmdGcCalibrate(gearID byte, timeout_s byte) []byte {
	payload := []byte{gearID}
	if timeout_s > 0 {
		payload = append(payload, timeout_s)
	}
	return BuildPacket(GcGEAR_CALIBRATE, payload, 0)
}

func CmdGcCalibCancel(gearID byte) []byte {
	return BuildPacket(GcGEAR_CALIB_CANCEL, []byte{gearID}, 0)
}

func CmdGcBatteryConfig(enabled bool, autoDeploy bool) []byte {
	e := byte(0)
	if enabled {
		e = 1
	}
	a := byte(0)
	if autoDeploy {
		a = 1
	}
	return BuildPacket(GcBATTERY_CONFIG, []byte{e, a}, 0)
}

func CmdGcDoorMode(gearID, preDeploy, postDeploy byte, delay_ms uint16) []byte {
	payload := []byte{gearID, preDeploy, postDeploy}
	payload = append(payload, U16LE(delay_ms)...)
	return BuildPacket(GcDOOR_MODE, payload, 0)
}

func CmdGcReset(gearID byte) []byte {
	return BuildPacket(GcGEAR_RESET, []byte{gearID}, 0)
}

func CmdGcEnable(gearID byte, enabled bool) []byte {
	e := byte(0)
	if enabled {
		e = 1
	}
	return BuildPacket(GcGEAR_ENABLE, []byte{gearID, e}, 0)
}

// ─── LightFX Commands ───

func CmdLfxLedSet(ch, brightness byte) []byte {
	return BuildPacket(LfxLED_SET, []byte{ch, brightness}, 0)
}

func CmdLfxLedOff(ch byte) []byte {
	return BuildPacket(LfxLED_OFF, []byte{ch}, 0)
}

func CmdLfxLedSeqClear(ch byte) []byte {
	return BuildPacket(LfxLED_SEQ_CLEAR, []byte{ch}, 0)
}

func CmdLfxLedSeqAdd(ch, eventType byte, param1, param2 uint16, param3, param4 byte) []byte {
	payload := []byte{ch, eventType}
	payload = append(payload, U16LE(param1)...)
	payload = append(payload, U16LE(param2)...)
	payload = append(payload, param3, param4)
	return BuildPacket(LfxLED_SEQ_ADD, payload, 0)
}

func CmdLfxLedSeqStart(ch byte, loopCount uint16) []byte {
	payload := []byte{ch}
	payload = append(payload, U16LE(loopCount)...)
	return BuildPacket(LfxLED_SEQ_START, payload, 0)
}

func CmdLfxLedSeqStop(ch byte) []byte {
	return BuildPacket(LfxLED_SEQ_STOP, []byte{ch}, 0)
}

func CmdLfxLedSeqRestart(ch byte) []byte {
	return BuildPacket(LfxLED_SEQ_RESTART, []byte{ch}, 0)
}

func CmdLfxLedSeqStatus(ch byte) []byte {
	return BuildPacket(LfxLED_SEQ_STATUS, []byte{ch}, 0)
}

func CmdLfxLedSeqQueue(ch byte) []byte {
	return BuildPacket(LfxLED_SEQ_QUEUE, []byte{ch}, 0)
}

func CmdLfxLedStatus() []byte {
	return BuildPacket(LfxLED_STATUS, nil, 0)
}

func CmdLfxServoSet(id byte, pulse_us uint16) []byte {
	payload := []byte{id}
	payload = append(payload, U16LE(pulse_us)...)
	return BuildPacket(LfxSERVO_SET, payload, 0)
}

func CmdLfxServoSettings(id byte, minPulse, maxPulse, speed, accel, decel uint16) []byte {
	payload := []byte{id}
	payload = append(payload, U16LE(minPulse)...)
	payload = append(payload, U16LE(maxPulse)...)
	payload = append(payload, U16LE(speed)...)
	payload = append(payload, U16LE(accel)...)
	payload = append(payload, U16LE(decel)...)
	return BuildPacket(LfxSERVO_SETTINGS, payload, 0)
}

func CmdLfxMasterBrightness(brightness byte) []byte {
	return BuildPacket(LfxLED_MASTER_BRIGHTNESS, []byte{brightness}, 0)
}

func CmdLfxLedReset(ch byte) []byte {
	return BuildPacket(LfxLED_RESET, []byte{ch}, 0)
}

func CmdLfxLedEnable(ch byte, enabled bool) []byte {
	e := byte(0)
	if enabled {
		e = 1
	}
	return BuildPacket(LfxLED_ENABLE, []byte{ch, e}, 0)
}

func CmdLfxLandingLightBind(slot, servoID, ledCh byte, deploy_us, retract_us uint16, brightness byte) []byte {
	payload := []byte{slot, servoID, ledCh}
	payload = append(payload, U16LE(deploy_us)...)
	payload = append(payload, U16LE(retract_us)...)
	payload = append(payload, brightness)
	return BuildPacket(LfxLANDING_LIGHT_BIND, payload, 0)
}

func CmdLfxLandingLightUnbind(slot byte) []byte {
	return BuildPacket(LfxLANDING_LIGHT_UNBIND, []byte{slot}, 0)
}

func CmdLfxLandingLightDeploy(slot byte) []byte {
	return BuildPacket(LfxLANDING_LIGHT_DEPLOY, []byte{slot}, 0)
}

func CmdLfxLandingLightRetract(slot byte) []byte {
	return BuildPacket(LfxLANDING_LIGHT_RETRACT, []byte{slot}, 0)
}

// ─── HubFX Commands ───

func CmdHubSlaveList() []byte {
	return BuildPacket(HubSLAVE_LIST, nil, 0)
}

func CmdHubSlaveInit(slaveType byte) []byte {
	return BuildPacket(HubSLAVE_INIT, []byte{slaveType}, 0)
}

func CmdHubAudioPlay(ch, vol, output, loopMode byte, loopCount uint16, path string) []byte {
	payload := []byte{ch, vol, output, loopMode}
	payload = append(payload, U16LE(loopCount)...)
	payload = append(payload, byte(len(path)))
	payload = append(payload, []byte(path)...)
	return BuildPacket(HubAUDIO_PLAY, payload, 0)
}

func CmdHubAudioStop(ch byte) []byte {
	return BuildPacket(HubAUDIO_STOP, []byte{ch}, 0)
}

func CmdHubAudioVolume(ch, vol byte) []byte {
	return BuildPacket(HubAUDIO_VOLUME, []byte{ch, vol}, 0)
}

func CmdHubAudioFade(ch byte) []byte {
	return BuildPacket(HubAUDIO_FADE, []byte{ch}, 0)
}

func CmdHubAudioStatusReq() []byte {
	return BuildPacket(HubAUDIO_STATUS_REQ, nil, 0)
}

func CmdHubEngineStart() []byte {
	return BuildPacket(HubENGINE_START, nil, 0)
}

func CmdHubEngineStop() []byte {
	return BuildPacket(HubENGINE_STOP, nil, 0)
}

func CmdHubEngineStatusReq() []byte {
	return BuildPacket(HubENGINE_STATUS_REQ, nil, 0)
}

func CmdHubConfigReload(path string) []byte {
	if path == "" {
		return BuildPacket(HubCONFIG_RELOAD, nil, 0)
	}
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	return BuildPacket(HubCONFIG_RELOAD, payload, 0)
}

func CmdHubConfigStatus() []byte {
	return BuildPacket(HubCONFIG_STATUS, nil, 0)
}

func CmdHubConfigSave(path string) []byte {
	if path == "" {
		return BuildPacket(HubCONFIG_SAVE, nil, 0)
	}
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	return BuildPacket(HubCONFIG_SAVE, payload, 0)
}

func CmdHubSDInit(speed_mhz byte) []byte {
	return BuildPacket(HubSD_INIT, []byte{speed_mhz}, 0)
}

func CmdHubSDStatusReq() []byte {
	return BuildPacket(HubSD_STATUS_REQ, nil, 0)
}

func CmdHubFlashStatusReq() []byte {
	return BuildPacket(HubFLASH_STATUS_REQ, nil, 0)
}

func CmdHubFileList(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return BuildPacket(HubFILE_LIST, payload, 0)
}

func CmdHubFileDelete(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return BuildPacket(HubFILE_DELETE, payload, 0)
}

func CmdHubFileMkdir(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return BuildPacket(HubFILE_MKDIR, payload, 0)
}

func CmdHubFileInfo(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return BuildPacket(HubFILE_INFO, payload, 0)
}

func CmdHubFileTree(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return BuildPacket(HubFILE_TREE, payload, 0)
}

func CmdHubUSBDevicesReq() []byte {
	return BuildPacket(HubUSB_DEVICES_REQ, nil, 0)
}

func CmdHubUSBResetBus() []byte {
	return BuildPacket(HubUSB_RESET_BUS, nil, 0)
}

func CmdHubCodecStatusReq() []byte {
	return BuildPacket(HubCODEC_STATUS_REQ, nil, 0)
}

func CmdHubAudioQueue(ch, vol byte, loopCount uint16, behavior byte, path string) []byte {
	payload := []byte{ch, vol}
	payload = append(payload, U16LE(loopCount)...)
	payload = append(payload, behavior, byte(len(path)))
	payload = append(payload, []byte(path)...)
	return BuildPacket(HubAUDIO_QUEUE, payload, 0)
}

func CmdHubAudioQueueClear(ch byte) []byte {
	return BuildPacket(HubAUDIO_QUEUE_CLEAR, []byte{ch}, 0)
}

func CmdHubFileDownload(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return BuildPacket(HubFILE_DOWNLOAD, payload, 0)
}

func CmdHubFileUploadBegin(path string, size uint32, target byte, mode byte) []byte {
	payload := make([]byte, 4+1+len(path)+1+1)
	copy(payload[0:4], U32LE(size))
	payload[4] = byte(len(path))
	copy(payload[5:5+len(path)], []byte(path))
	payload[5+len(path)] = target
	payload[6+len(path)] = mode
	return BuildPacket(HubFILE_UPLOAD_BEGIN, payload, 0)
}

func CmdHubFileUploadData(seq uint16, data []byte) []byte {
	crc := CRC16CCITT(data)
	payload := make([]byte, 4+len(data))
	copy(payload[0:2], U16LE(seq))
	copy(payload[2:4], U16LE(crc))
	copy(payload[4:], data)
	return BuildPacket(HubFILE_UPLOAD_DATA, payload, 0)
}

func CmdHubFileUploadEnd() []byte {
	return BuildPacket(HubFILE_UPLOAD_END, nil, 0)
}

func CmdHubFileUploadCancel() []byte {
	return BuildPacket(HubFILE_UPLOAD_CANCEL, nil, 0)
}

func CmdHubSlaveInfo(slaveType byte) []byte {
	return BuildPacket(HubSLAVE_INFO, []byte{slaveType}, 0)
}

// ─── Slave Route Wrappers ───

func CmdHubSlaveRoute(routeType byte, innerPacket []byte) []byte {
	// Parse the inner packet to extract its type and payload
	ptype, _, payload, ok := ParsePacket(innerPacket)
	if !ok {
		return innerPacket
	}
	routePayload := []byte{ptype}
	routePayload = append(routePayload, payload...)
	return BuildPacket(routeType, routePayload, 0)
}
