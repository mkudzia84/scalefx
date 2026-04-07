package hubfx

// HubFX Protocol — mirrors serial/hubfx/hubfx.h
// Master hub: slave management, audio, engine, config, storage, USB, codec.

import (
	"fmt"

	"scalefx/protocol"
)

// ─── Packet Types (0x80-0xB0) ───

const (
	// Slave management
	SlaveList         protocol.PacketType = 0x80
	SlaveListResp     protocol.PacketType = 0x81
	SlaveInit         protocol.PacketType = 0x82
	SlaveStatus       protocol.PacketType = 0x83

	// Audio control
	AudioPlay         protocol.PacketType = 0x84
	AudioStop         protocol.PacketType = 0x85
	AudioVolume       protocol.PacketType = 0x86
	AudioFade         protocol.PacketType = 0x87
	AudioQueue        protocol.PacketType = 0x88
	AudioQueueClear   protocol.PacketType = 0x89
	AudioStatusReq    protocol.PacketType = 0x8A
	AudioStatusResp   protocol.PacketType = 0x8B

	// Engine FX
	EngineStart       protocol.PacketType = 0x8C
	EngineStop        protocol.PacketType = 0x8D
	EngineStatusReq   protocol.PacketType = 0x8E
	EngineStatusResp  protocol.PacketType = 0x8F

	// Config
	ConfigReload      protocol.PacketType = 0x90
	ConfigStatus      protocol.PacketType = 0x91
	ConfigStatusResp  protocol.PacketType = 0x92
	ConfigSave        protocol.PacketType = 0xAC

	// SD Card
	SdInit            protocol.PacketType = 0x93
	SdStatusReq       protocol.PacketType = 0x94
	SdStatusResp      protocol.PacketType = 0x95

	// Slave routing
	SlaveRouteGunfx       protocol.PacketType = 0x96
	SlaveRouteLightfx     protocol.PacketType = 0x97
	SlaveRouteGearcontrol protocol.PacketType = 0x98

	// Flash
	FlashStatusReq    protocol.PacketType = 0x99

	// File operations
	FileList          protocol.PacketType = 0x9A
	FileDelete        protocol.PacketType = 0x9B
	FileMkdir         protocol.PacketType = 0x9C
	FileInfo          protocol.PacketType = 0x9D
	FileInfoResp      protocol.PacketType = 0x9E
	FileDownload      protocol.PacketType = 0x9F
	FileUploadBegin   protocol.PacketType = 0xA0
	FileUploadData    protocol.PacketType = 0xA1
	FileUploadEnd     protocol.PacketType = 0xA2
	FileUploadCancel  protocol.PacketType = 0xA3
	FileTree          protocol.PacketType = 0xA9

	// USB
	UsbDevicesReq     protocol.PacketType = 0xA7
	UsbDevicesResp    protocol.PacketType = 0xA8
	UsbResetBus       protocol.PacketType = 0xAD

	// Codec
	CodecStatusReq    protocol.PacketType = 0xAA
	CodecStatusResp   protocol.PacketType = 0xAB

	// Slave info
	SlaveInfo         protocol.PacketType = 0xAE
	SlaveInfoResp     protocol.PacketType = 0xAF

	// Upload progress
	FileUploadProgress protocol.PacketType = 0xB0
)

// ─── Error Codes (0x80-0x8F) ───

const (
	ErrSlaveNotFound     protocol.ErrorCode = 0x80
	ErrSlaveNotConnected protocol.ErrorCode = 0x81
	ErrSlaveInitFailed   protocol.ErrorCode = 0x82
	ErrNoSlaves          protocol.ErrorCode = 0x83
	ErrSlaveCommError    protocol.ErrorCode = 0x84
	ErrAudioError        protocol.ErrorCode = 0x85
	ErrSdNotInitialized  protocol.ErrorCode = 0x86
	ErrEngineNotAvailable protocol.ErrorCode = 0x87
	ErrConfigError       protocol.ErrorCode = 0x88
	ErrInvalidChannel    protocol.ErrorCode = 0x89
	ErrFileNotFound      protocol.ErrorCode = 0x8A
	ErrFileAlreadyExists protocol.ErrorCode = 0x8B
	ErrFileIoError       protocol.ErrorCode = 0x8C
	ErrFileTooLarge      protocol.ErrorCode = 0x8D
	ErrUploadInProgress  protocol.ErrorCode = 0x8E
	ErrNoUploadActive    protocol.ErrorCode = 0x8F
)

// ─── Audio Constants ───

const (
	AudioOutputCh1 byte = 0x01
	AudioOutputCh2 byte = 0x02
	AudioOutputAll byte = 0x03

	AudioLoopNone     byte = 0
	AudioLoopFinite   byte = 1
	AudioLoopInfinite byte = 2

	AudioQueueFinishLoop byte = 0
	AudioQueueStopNow    byte = 1

	AudioChAll    byte = 0xFF
	AudioMaxChans      = 8
)

// ─── Storage Constants ───

const (
	StorageTargetSd    byte = 0
	StorageTargetFlash byte = 1
)

// ─── Slave Types ───

const (
	SlaveTypeUnknown     byte = 0
	SlaveTypeGunFx       byte = 1
	SlaveTypeLightFx     byte = 2
	SlaveTypeGearControl byte = 3
)

// ─── Commands ───

func CmdSlaveList() []byte {
	return protocol.BuildPacket(SlaveList, nil, 0)
}

func CmdSlaveInit(slaveType byte) []byte {
	return protocol.BuildPacket(SlaveInit, []byte{slaveType}, 0)
}

func CmdSlaveInfo(slaveType byte) []byte {
	return protocol.BuildPacket(SlaveInfo, []byte{slaveType}, 0)
}

func CmdAudioPlay(ch, vol, output, loopMode byte, loopCount uint16, path string) []byte {
	payload := []byte{ch, vol, output, loopMode}
	payload = append(payload, protocol.U16LE(loopCount)...)
	payload = append(payload, byte(len(path)))
	payload = append(payload, []byte(path)...)
	return protocol.BuildPacket(AudioPlay, payload, 0)
}

func CmdAudioStop(ch byte) []byte {
	return protocol.BuildPacket(AudioStop, []byte{ch}, 0)
}

func CmdAudioVolume(ch, vol byte) []byte {
	return protocol.BuildPacket(AudioVolume, []byte{ch, vol}, 0)
}

func CmdAudioFade(ch byte) []byte {
	return protocol.BuildPacket(AudioFade, []byte{ch}, 0)
}

func CmdAudioStatusReq() []byte {
	return protocol.BuildPacket(AudioStatusReq, nil, 0)
}

func CmdAudioQueue(ch, vol byte, loopCount uint16, behavior byte, path string) []byte {
	payload := []byte{ch, vol}
	payload = append(payload, protocol.U16LE(loopCount)...)
	payload = append(payload, behavior, byte(len(path)))
	payload = append(payload, []byte(path)...)
	return protocol.BuildPacket(AudioQueue, payload, 0)
}

func CmdAudioQueueClear(ch byte) []byte {
	return protocol.BuildPacket(AudioQueueClear, []byte{ch}, 0)
}

func CmdEngineStart() []byte {
	return protocol.BuildPacket(EngineStart, nil, 0)
}

func CmdEngineStop() []byte {
	return protocol.BuildPacket(EngineStop, nil, 0)
}

func CmdEngineStatusReq() []byte {
	return protocol.BuildPacket(EngineStatusReq, nil, 0)
}

func CmdConfigReload(path string) []byte {
	if path == "" {
		return protocol.BuildPacket(ConfigReload, nil, 0)
	}
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	return protocol.BuildPacket(ConfigReload, payload, 0)
}

func CmdConfigStatus() []byte {
	return protocol.BuildPacket(ConfigStatus, nil, 0)
}

func CmdConfigSave(path string) []byte {
	if path == "" {
		return protocol.BuildPacket(ConfigSave, nil, 0)
	}
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	return protocol.BuildPacket(ConfigSave, payload, 0)
}

func CmdSdInit(speed_mhz byte) []byte {
	return protocol.BuildPacket(SdInit, []byte{speed_mhz}, 0)
}

func CmdSdStatusReq() []byte {
	return protocol.BuildPacket(SdStatusReq, nil, 0)
}

func CmdFlashStatusReq() []byte {
	return protocol.BuildPacket(FlashStatusReq, nil, 0)
}

func CmdFileList(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return protocol.BuildPacket(FileList, payload, 0)
}

func CmdFileDelete(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return protocol.BuildPacket(FileDelete, payload, 0)
}

func CmdFileMkdir(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return protocol.BuildPacket(FileMkdir, payload, 0)
}

func CmdFileInfo(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return protocol.BuildPacket(FileInfo, payload, 0)
}

func CmdFileTree(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return protocol.BuildPacket(FileTree, payload, 0)
}

func CmdFileDownload(path string, target byte) []byte {
	payload := []byte{byte(len(path))}
	payload = append(payload, []byte(path)...)
	payload = append(payload, target)
	return protocol.BuildPacket(FileDownload, payload, 0)
}

func CmdFileUploadBegin(path string, size uint32, target byte, mode byte) []byte {
	payload := make([]byte, 4+1+len(path)+1+1)
	copy(payload[0:4], protocol.U32LE(size))
	payload[4] = byte(len(path))
	copy(payload[5:5+len(path)], []byte(path))
	payload[5+len(path)] = target
	payload[6+len(path)] = mode
	return protocol.BuildPacket(FileUploadBegin, payload, 0)
}

func CmdFileUploadData(seq uint16, data []byte) []byte {
	crc := protocol.CRC16CCITT(data)
	payload := make([]byte, 4+len(data))
	copy(payload[0:2], protocol.U16LE(seq))
	copy(payload[2:4], protocol.U16LE(crc))
	copy(payload[4:], data)
	return protocol.BuildPacket(FileUploadData, payload, 0)
}

func CmdFileUploadEnd() []byte {
	return protocol.BuildPacket(FileUploadEnd, nil, 0)
}

func CmdFileUploadCancel() []byte {
	return protocol.BuildPacket(FileUploadCancel, nil, 0)
}

func CmdUsbDevicesReq() []byte {
	return protocol.BuildPacket(UsbDevicesReq, nil, 0)
}

func CmdUsbResetBus() []byte {
	return protocol.BuildPacket(UsbResetBus, nil, 0)
}

func CmdCodecStatusReq() []byte {
	return protocol.BuildPacket(CodecStatusReq, nil, 0)
}

// CmdSlaveRoute wraps an inner packet for routing through the hub to a slave.
func CmdSlaveRoute(routeType protocol.PacketType, innerPacket []byte) []byte {
	ptype, _, payload, ok := protocol.ParsePacket(innerPacket)
	if !ok {
		return innerPacket
	}
	routePayload := []byte{byte(ptype)}
	routePayload = append(routePayload, payload...)
	return protocol.BuildPacket(routeType, routePayload, 0)
}

// ─── Name Lookups ───

// EngineStateName returns engine state name.
func EngineStateName(state byte) string {
	names := map[byte]string{0: "Stopped", 1: "Starting", 2: "Running", 3: "Stopping"}
	if n, ok := names[state]; ok {
		return n
	}
	return fmt.Sprintf("Unknown(%d)", state)
}

// SlaveTypeName returns slave type name.
func SlaveTypeName(stype byte) string {
	names := map[byte]string{0: "Unknown", 1: "GunFX", 2: "LightFX", 3: "GearControl"}
	if n, ok := names[stype]; ok {
		return n
	}
	return fmt.Sprintf("Unknown(%d)", stype)
}

// ─── Name Registration ───

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		SlaveList:             "HUB.SLAVE_LIST",
		SlaveListResp:        "HUB.SLAVE_LIST_RESP",
		SlaveInit:            "HUB.SLAVE_INIT",
		SlaveStatus:          "HUB.SLAVE_STATUS",
		AudioPlay:            "HUB.AUDIO_PLAY",
		AudioStop:            "HUB.AUDIO_STOP",
		AudioVolume:          "HUB.AUDIO_VOLUME",
		AudioFade:            "HUB.AUDIO_FADE",
		AudioQueue:           "HUB.AUDIO_QUEUE",
		AudioQueueClear:      "HUB.AUDIO_QUEUE_CLEAR",
		AudioStatusReq:       "HUB.AUDIO_STATUS_REQ",
		AudioStatusResp:      "HUB.AUDIO_STATUS_RESP",
		EngineStart:          "HUB.ENGINE_START",
		EngineStop:           "HUB.ENGINE_STOP",
		EngineStatusReq:      "HUB.ENGINE_STATUS_REQ",
		EngineStatusResp:     "HUB.ENGINE_STATUS_RESP",
		ConfigReload:         "HUB.CONFIG_RELOAD",
		ConfigStatus:         "HUB.CONFIG_STATUS",
		ConfigStatusResp:     "HUB.CONFIG_STATUS_RESP",
		ConfigSave:           "HUB.CONFIG_SAVE",
		SdInit:               "HUB.SD_INIT",
		SdStatusReq:          "HUB.SD_STATUS_REQ",
		SdStatusResp:         "HUB.SD_STATUS_RESP",
		SlaveRouteGunfx:      "HUB.SLAVE_ROUTE_GUNFX",
		SlaveRouteLightfx:    "HUB.SLAVE_ROUTE_LIGHTFX",
		SlaveRouteGearcontrol: "HUB.SLAVE_ROUTE_GEARCONTROL",
		FlashStatusReq:       "HUB.FLASH_STATUS_REQ",
		FileList:             "HUB.FILE_LIST",
		FileDelete:           "HUB.FILE_DELETE",
		FileMkdir:            "HUB.FILE_MKDIR",
		FileInfo:             "HUB.FILE_INFO",
		FileInfoResp:         "HUB.FILE_INFO_RESP",
		FileDownload:         "HUB.FILE_DOWNLOAD",
		FileUploadBegin:      "HUB.FILE_UPLOAD_BEGIN",
		FileUploadData:       "HUB.FILE_UPLOAD_DATA",
		FileUploadEnd:        "HUB.FILE_UPLOAD_END",
		FileUploadCancel:     "HUB.FILE_UPLOAD_CANCEL",
		FileTree:             "HUB.FILE_TREE",
		UsbDevicesReq:        "HUB.USB_DEVICES_REQ",
		UsbDevicesResp:       "HUB.USB_DEVICES_RESP",
		UsbResetBus:          "HUB.USB_RESET_BUS",
		CodecStatusReq:       "HUB.CODEC_STATUS_REQ",
		CodecStatusResp:      "HUB.CODEC_STATUS_RESP",
		SlaveInfo:            "HUB.SLAVE_INFO",
		SlaveInfoResp:        "HUB.SLAVE_INFO_RESP",
		FileUploadProgress:   "HUB.FILE_UPLOAD_PROGRESS",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrSlaveNotFound:     "SLAVE_NOT_FOUND",
		ErrSlaveNotConnected: "SLAVE_NOT_CONNECTED",
		ErrSlaveInitFailed:   "SLAVE_INIT_FAILED",
		ErrNoSlaves:          "NO_SLAVES",
		ErrSlaveCommError:    "SLAVE_COMM_ERROR",
		ErrAudioError:        "AUDIO_ERROR",
		ErrSdNotInitialized:  "SD_NOT_INITIALIZED",
		ErrEngineNotAvailable: "ENGINE_NOT_AVAILABLE",
		ErrConfigError:       "CONFIG_ERROR",
		ErrInvalidChannel:    "HUB_INVALID_CHANNEL",
		ErrFileNotFound:      "FILE_NOT_FOUND",
		ErrFileAlreadyExists: "FILE_ALREADY_EXISTS",
		ErrFileIoError:       "FILE_IO_ERROR",
		ErrFileTooLarge:      "FILE_TOO_LARGE",
		ErrUploadInProgress:  "UPLOAD_IN_PROGRESS",
		ErrNoUploadActive:    "NO_UPLOAD_ACTIVE",
	})
}
