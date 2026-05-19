package hubfx

// Storage backend status (SD/Flash) and USB host bus controls.
// File operations live in files.go; these are the lighter-weight queries
// that report mount state, capacity, attached USB devices, etc.

func (h *Handler) cmdSdInit(_ []string) { h.E.Ack(h.E.API.HubFx.SdInit(), "SD card remounted") }

func (h *Handler) cmdSdStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.SdStatus(), h.parseSdStatus)
}

func (h *Handler) cmdFlashStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.FlashStatus(), h.parseFlashStatus)
}

func (h *Handler) cmdUsbDevices(_ []string) {
	h.E.Query(h.E.API.HubFx.UsbDevices(), h.parseUsbDevices)
}

func (h *Handler) cmdUsbReset(_ []string) { h.E.Ack(h.E.API.HubFx.UsbReset(), "USB bus reset") }
