package devicemodel

// Input-side model: the RC receiver protocol on an input port, how many
// channels it carries, and what function each channel is mapped to.  This
// is the data behind the Studio "Input & Ports" tab's left column.
//
// Channel→function mapping is a UI/config concept (like domain claims):
// the firmware streams raw channel values; the function label is how the
// operator reasons about them ("channel 5 = Landing Gear") and, later, how
// the hub routes a channel to a domain trigger.

// ─── Input protocol ───────────────────────────────────────────────────

// InputProtocol is the wire format an input port decodes.  Only PPM is
// implemented in firmware today; SBUS and Jeti EX are catalogued but
// disabled in the UI until their decoders land.
type InputProtocol string

const (
	InputNone   InputProtocol = "none"
	InputPpm    InputProtocol = "ppm"
	InputSbus   InputProtocol = "sbus"
	InputJetiEx InputProtocol = "jeti-ex"
)

// InputProtocolDef describes a selectable protocol for the UI.
type InputProtocolDef struct {
	ID          InputProtocol `json:"id"`
	Label       string        `json:"label"`
	RoleKind    byte          `json:"roleKind"`    // role attached to realize it
	Implemented bool          `json:"implemented"` // false → shown disabled
	MaxChannels int           `json:"maxChannels"`
}

// inputProtocols is the catalog the input panel renders.
var inputProtocols = []InputProtocolDef{
	{ID: InputPpm, Label: "PPM (pulse)", RoleKind: 0x02 /*RcPwmInput*/, Implemented: true, MaxChannels: 8},
	{ID: InputSbus, Label: "SBUS", RoleKind: 0x03 /*SbusInput*/, Implemented: false, MaxChannels: 16},
	{ID: InputJetiEx, Label: "Jeti EX", RoleKind: 0x04 /*JetiExInput*/, Implemented: false, MaxChannels: 16},
}

// InputProtocols returns the protocol catalog.
func InputProtocols() []InputProtocolDef { return inputProtocols }

// ProtocolByID looks up a protocol definition.
func ProtocolByID(id InputProtocol) (InputProtocolDef, bool) {
	for _, p := range inputProtocols {
		if p.ID == id {
			return p, true
		}
	}
	return InputProtocolDef{}, false
}

// ─── Channel function catalog ─────────────────────────────────────────

// ChannelFunctionDef is one selectable channel role, grouped for the
// dropdown.  IDs are stable; labels are display strings.
type ChannelFunctionDef struct {
	ID    string `json:"id"`
	Label string `json:"label"`
	Group string `json:"group"`
}

// channelFunctions is the catalog offered per channel — the HubFX control
// surface, grouped by subsystem.  Each function is something an RC channel
// can drive on the hub (engine, lights, gear, gun, audio).  Flight-control
// axes (aileron/elevator/rudder) are intentionally absent: the flight
// controller owns those, the hub doesn't act on them.
//
// Firmware backing: engine on/off, light program/brightness, landing
// lights, landing gear, gun fire/mode/smoke, and master volume are wired.
// Gun yaw/pitch/retract describe a pan-tilt turret + retract that the
// current GunFX (muzzle flash + recoil servo + smoke heater) does not yet
// implement — catalogued so layouts can be planned ahead.
var channelFunctions = []ChannelFunctionDef{
	{ID: "unassigned", Label: "— unassigned —", Group: "General"},

	// Primary flight axes — the hub doesn't drive these, but operators wire
	// a full receiver and label them for reference / engine-throttle follow.
	{ID: "throttle", Label: "Throttle", Group: "Flight"},
	{ID: "aileron", Label: "Aileron / Roll", Group: "Flight"},
	{ID: "elevator", Label: "Elevator / Pitch", Group: "Flight"},
	{ID: "rudder", Label: "Rudder / Yaw", Group: "Flight"},

	// Helicopter.
	{ID: "collective", Label: "Collective Pitch", Group: "Helicopter"},
	{ID: "gyro_gain", Label: "Gyro / Tail Gain", Group: "Helicopter"},
	{ID: "governor", Label: "Governor / Rotor RPM", Group: "Helicopter"},
	{ID: "flight_mode", Label: "Flight Mode / Idle-Up", Group: "Helicopter"},

	// Fixed-wing.
	{ID: "flaps", Label: "Flaps", Group: "Fixed-wing"},
	{ID: "spoilers", Label: "Spoilers / Airbrake", Group: "Fixed-wing"},

	{ID: "engine_onoff", Label: "Engine On / Off", Group: "Engine"},

	{ID: "light_program", Label: "Light Program", Group: "Lights"},
	{ID: "light_brightness", Label: "Light Brightness", Group: "Lights"},
	{ID: "landing_lights", Label: "Landing Lights On / Off", Group: "Lights"},

	{ID: "landing_gear", Label: "Landing Gear Deploy / Retract", Group: "Gear"},

	{ID: "gun_fire", Label: "Gun Fire", Group: "Gun"},
	{ID: "gun_fire_mode", Label: "Gun Fire Mode", Group: "Gun"},
	{ID: "gun_smoke", Label: "Gun Smoke On / Off", Group: "Gun"},
	{ID: "gun_yaw", Label: "Gun Yaw (turret)", Group: "Gun"},
	{ID: "gun_pitch", Label: "Gun Pitch (turret)", Group: "Gun"},
	{ID: "gun_retract", Label: "Gun Retract", Group: "Gun"},

	{ID: "master_volume", Label: "Master Volume", Group: "Audio"},
}

// ChannelFunctions returns the channel-function catalog.
func ChannelFunctions() []ChannelFunctionDef { return channelFunctions }

// ─── Per-port input configuration ─────────────────────────────────────

// ChannelMap assigns a logical function to a channel index.
type ChannelMap struct {
	Channel  int    `json:"channel"`
	Function string `json:"function"`
}

// InputPortConfig is the operator's configuration for one input port.
type InputPortConfig struct {
	Port         PortRef       `json:"port"`
	Protocol     InputProtocol `json:"protocol"`
	ChannelCount int           `json:"channelCount"`
	Channels     []ChannelMap  `json:"channels"`
}

// NewInputPortConfig builds a default config for a port: PPM, 8 channels,
// all unassigned.
func NewInputPortConfig(p PortRef) InputPortConfig {
	cfg := InputPortConfig{Port: p, Protocol: InputPpm, ChannelCount: 8}
	for i := 0; i < cfg.ChannelCount; i++ {
		cfg.Channels = append(cfg.Channels, ChannelMap{Channel: i, Function: "unassigned"})
	}
	return cfg
}

// SetChannelCount resizes the channel map, preserving existing assignments.
func (c *InputPortConfig) SetChannelCount(n int) {
	if n < 1 {
		n = 1
	}
	if n > 18 {
		n = 18
	}
	out := make([]ChannelMap, n)
	for i := 0; i < n; i++ {
		fn := "unassigned"
		if i < len(c.Channels) {
			fn = c.Channels[i].Function
		}
		out[i] = ChannelMap{Channel: i, Function: fn}
	}
	c.Channels = out
	c.ChannelCount = n
}

// SetFunction assigns a function to a channel (no-op if out of range).
func (c *InputPortConfig) SetFunction(channel int, fn string) {
	for i := range c.Channels {
		if c.Channels[i].Channel == channel {
			c.Channels[i].Function = fn
			return
		}
	}
}
