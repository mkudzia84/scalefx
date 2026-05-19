package hubfx

// Slave roster commands: list / init / info per attached USB-host slave.

import (
	"fmt"

	hfxp "scalefx/protocol/hubfx"
)

func (h *Handler) cmdSlaves(_ []string) {
	h.E.Query(h.E.API.HubFx.SlaveList(), h.parseSlaveList)
}

func (h *Handler) cmdSlaveInit(args []string) {
	if !h.E.RequireArgs(args, 1, "slave.init <type> (gunfx|lightfx|gearcontrol or 1|2|3)") {
		return
	}
	slaveType, ok := ParseSlaveType(args[0])
	if !ok {
		h.E.Out.Error("Unknown slave type: %s", args[0])
		h.E.Out.Info("Valid types: gunfx (1), lightfx (2), gearcontrol (3)")
		return
	}
	h.E.Ack(h.E.API.HubFx.SlaveInit(slaveType), fmt.Sprintf("%s slave initialized", hfxp.SlaveTypeName(slaveType)))
}

func (h *Handler) cmdSlaveInfo(args []string) {
	if !h.E.RequireArgs(args, 1, "slave.info <type> (gunfx|lightfx|gearcontrol or 1|2|3)") || !h.E.RequireConn() {
		return
	}
	slaveType, ok := ParseSlaveType(args[0])
	if !ok {
		h.E.Out.Error("Unknown slave type: %s", args[0])
		return
	}
	h.E.Query(h.E.API.HubFx.SlaveInfo(slaveType), h.parseSlaveInfo)
}
