package hubfx

// Engine effect commands: start / stop / status of the synthesised engine
// audio loop driven by the HubFX firmware.

func (h *Handler) cmdEngineStart(_ []string) {
	h.E.Ack(h.E.API.HubFx.EngineStart(), "Engine start")
}

func (h *Handler) cmdEngineStop(_ []string) {
	h.E.Ack(h.E.API.HubFx.EngineStop(), "Engine stop")
}

func (h *Handler) cmdEngineStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.EngineStatus(), h.parseEngineStatus)
}
