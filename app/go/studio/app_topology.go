package main

// Capability + topology bindings for the Firmware tab's board view.
// DeviceSystemInfo() is the one-shot the frontend calls after connect to
// render what the board can do (decoded capability flags) and — on a hub —
// the full port roster + every connected expander (incl. battery).

import (
	"fmt"

	"scalefx/client"
	"scalefx/protocol/core"
)

// DeviceInfo is the composite board view returned to the Firmware tab.
type DeviceInfo struct {
	// Compiled-in capability bitmask + decoded flag names (always set).
	Capabilities    uint32   `json:"capabilities"`
	CapabilityNames []string `json:"capabilityNames"`

	// HasTopology is true when the board exposes the GUID-addressed
	// topology surface (the hub).  Drives whether Ports/System are populated.
	HasTopology bool `json:"hasTopology"`

	// System is the hub + every connected expander (identity, capabilities,
	// build, and per-expander battery telemetry).  nil on non-hub boards.
	System *client.SystemInfo `json:"system,omitempty"`

	// Ports is the full port roster across every board (hub + expanders),
	// each entry GUID-tagged.  Empty on non-hub boards.
	Ports []client.BoardPorts `json:"ports,omitempty"`
}

// DeviceSystemInfo returns the connected board's capability + topology view.
// Capability flags are always populated from the cached IDENTIFY; the port
// roster + expander/battery list are queried live when the board exposes the
// topology surface (the hub).
func (a *App) DeviceSystemInfo() (DeviceInfo, error) {
	defer a.diag.Around("DeviceSystemInfo", nil)()
	a.mu.Lock()
	defer a.mu.Unlock()

	out := DeviceInfo{}
	if a.c == nil {
		return out, fmt.Errorf("not connected")
	}
	out.Capabilities = a.id.Capabilities
	out.CapabilityNames = a.id.CapabilityNames()
	out.HasTopology = a.id.Capabilities&core.CapTopology != 0

	if out.HasTopology {
		if si, err := a.c.Expanders.SystemInfo(); err == nil {
			out.System = &si
		} else {
			a.diag.Warn("TOPO", "system-info query failed: %v", err)
		}
		if ports, err := a.c.Topology.PortList(""); err == nil {
			out.Ports = ports
		} else {
			a.diag.Warn("TOPO", "port-list query failed: %v", err)
		}
	}
	return out, nil
}
