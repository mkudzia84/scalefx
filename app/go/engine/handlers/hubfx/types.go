package hubfx

// ScaleFX Engine - HubFX Decoded Types
// JSON-tagged decoded structs for STATUS_BROADCAST. See CLAUDE.md Rule 19:
// decoded types live here — never re-decode in studio/app.go or cli/*.

import (
	"scalefx/protocol"
)

// StatusBroadcast is the HubFX periodic STATUS_BROADCAST payload.
// Wire format mirrors onStatusData() in hubfx/esp32s3 firmware.
type StatusBroadcast struct {
	Core1Ready    bool   `json:"core1Ready"`
	AudioInit     bool   `json:"audioInit"`
	FlashReady    bool   `json:"flashReady"`
	UsbReady      bool   `json:"usbReady"`
	SdReady       bool   `json:"sdReady"`
	SlaveMask     uint8  `json:"slaveMask"`
	Loop1Count    uint32 `json:"loop1Count"`
	GunFxReady    bool   `json:"gunFxReady"`
	LightFxReady  bool   `json:"lightFxReady"`
	GearCtrlReady bool   `json:"gearControlReady"`

	// I2C device block (extended format, present when len(data) >= 19).
	I2C I2CStatus `json:"i2c"`
}

// I2CStatus describes the hub's I2C device roster (GPIO expander, INA226
// current monitors, audio codec). Present only when firmware emits the
// extended status payload (>=19 bytes).
type I2CStatus struct {
	Present      bool         `json:"present"`
	Mask         uint8        `json:"mask"`
	DetectedCount uint8       `json:"detectedCount"`
	PCALPresent  bool         `json:"pcalPresent"`  // PCAL6416A GPIO expander
	TASPresent   bool         `json:"tasPresent"`   // TAS5825M audio codec
	INA226       [6]INA226Bus `json:"ina226"`
}

// INA226Bus is a single INA226 current monitor slot.
// Address is 0x40 + index.
type INA226Bus struct {
	Present    bool   `json:"present"`
	Voltage_mV uint16 `json:"voltage_mV"`
}

// DecodeStatusBroadcast parses a HubFX STATUS_BROADCAST payload.
// Returns nil for payloads shorter than 2 bytes.
func DecodeStatusBroadcast(data []byte) *StatusBroadcast {
	if len(data) < 2 {
		return nil
	}
	flags := data[0]
	mask := data[1]
	s := &StatusBroadcast{
		Core1Ready:    flags&0x01 != 0,
		AudioInit:     flags&0x02 != 0,
		FlashReady:    flags&0x04 != 0,
		UsbReady:      flags&0x08 != 0,
		SdReady:       flags&0x10 != 0,
		SlaveMask:     mask,
		GunFxReady:    mask&0x01 != 0,
		LightFxReady:  mask&0x02 != 0,
		GearCtrlReady: mask&0x04 != 0,
	}
	if len(data) >= 6 {
		s.Loop1Count = protocol.ReadU32LE(data, 2)
	}
	// Extended I2C block: mask byte at offset 6, then 6×u16 INA226 voltages.
	if len(data) >= 19 {
		i2cMask := data[6]
		s.I2C.Present = true
		s.I2C.Mask = i2cMask
		s.I2C.PCALPresent = i2cMask&0x01 != 0
		s.I2C.TASPresent = i2cMask&0x80 != 0
		count := uint8(0)
		for b := uint8(0); b < 8; b++ {
			if i2cMask&(1<<b) != 0 {
				count++
			}
		}
		s.I2C.DetectedCount = count
		for i := 0; i < 6; i++ {
			s.I2C.INA226[i].Present = i2cMask&(1<<(i+1)) != 0
			s.I2C.INA226[i].Voltage_mV = protocol.ReadU16LE(data, 7+i*2)
		}
	}
	return s
}
