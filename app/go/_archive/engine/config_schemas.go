package engine

// ScaleFX Engine - Config Schema Definitions
// Controller-specific YAML config schemas mirroring the C++ declarative schemas.
// Used for client-side validation before upload.

import "scalefx/protocol/core"

// SchemaForController returns the validation schema for the given controller type.
// Returns nil if no schema is defined (e.g., GunFX, NoOp).
func SchemaForController(ctrl string) *Schema {
	switch ctrl {
	case core.CtrlGearControl:
		return GearControlSchema()
	case core.CtrlHubFX:
		return HubFXSchema()
	case core.CtrlLightFX:
		return LightFXSchema()
	default:
		return nil
	}
}

// ─── GearControl Schema ───
// Mirrors: controllers/gearcontrol/pico/src/config/gearcontrol_config.h

func GearControlSchema() *Schema {
	retractItem := NewSchema("retract").
		PropRange("channel", FieldInt, 0, 2).
		Prop("enabled", FieldBool).
		PropRange("stall_current_mA", FieldInt, 50, 5000).
		PropRange("timeout_ms", FieldInt, 500, 65000)

	pinItem := NewSchema("pin").
		Prop("slot", FieldString).
		PropEnum("role", "door", "gear_input", "yaw_input", "yaw_output", "unused").
		PropRange("channel", FieldInt, 0, 2).
		PropRange("min_us", FieldInt, 300, 2700).
		PropRange("max_us", FieldInt, 300, 2700).
		Prop("speed", FieldInt).
		Prop("reversed", FieldBool).
		PropRange("threshold_us", FieldInt, 800, 2200).
		PropRange("gear_id", FieldInt, 0, 2).
		PropRange("neutral_us", FieldInt, 500, 2500)

	doorModeItem := NewSchema("door_mode").
		PropRange("channel", FieldInt, 0, 2).
		PropEnum("pre_deploy", "none", "single", "dual_sync", "dual_delay", "dual_seq").
		PropEnum("post_deploy", "none", "single", "dual_sync", "dual_delay", "dual_seq").
		PropRange("delay_ms", FieldInt, 0, 5000)

	batterySchema := NewSchema("battery").
		Prop("auto_deploy", FieldBool).
		PropEnum("chemistry", "lipo", "liion", "nimh").
		PropRange("cell_count", FieldInt, 0, 6) // 0 = auto-detect, 1-6 = fixed cell count

	return NewSchema("GearControl").
		Seq("retracts", 3, retractItem).
		Seq("pins", 7, pinItem).
		Seq("door_modes", 3, doorModeItem).
		SubGroup("battery", batterySchema)
}

// ─── HubFX Schema ───
// Mirrors: controllers/hubfx/esp32s3/src/config/hubfx_config.h
// Composes: audio, engine_fx, gun_fx sections

func HubFXSchema() *Schema {
	audioSchema := NewSchema("audio").
		PropEnum("codec_supply_voltage", "12v", "15v", "20v", "24v")

	transitionsSchema := NewSchema("transitions").
		Prop("starting_offset_ms", FieldInt).
		Prop("stopping_offset_ms", FieldInt)

	soundsSchema := NewSchema("sounds").
		Prop("starting", FieldString).
		Prop("running", FieldString).
		Prop("stopping", FieldString).
		SubGroup("transitions", transitionsSchema)

	toggleSchema := NewSchema("engine_toggle").
		PropRange("input_channel", FieldInt, 1, 10).
		PropRange("threshold_us", FieldInt, 800, 2200)

	engineSchema := NewSchema("engine_fx").
		Prop("enabled", FieldBool).
		PropEnum("type", "turbine", "radial", "diesel").
		PropEnum("output_channels", "all", "ch1", "ch2", "ch1+ch2", "none").
		SubGroup("engine_toggle", toggleSchema).
		SubGroup("sounds", soundsSchema)

	gunFxSchema := NewSchema("gun_fx").
		PropEnum("output_channels", "all", "ch1", "ch2", "ch1+ch2", "none")

	return NewSchema("HubFX").
		SubGroup("audio", audioSchema).
		SubGroup("engine_fx", engineSchema).
		SubGroup("gun_fx", gunFxSchema)
}

// ─── LightFX Schema ───
// Mirrors: controllers/lightfx/pico/src/config/lightfx_config.h (placeholder)
// + controllers/lib/sfx_config/config/light_program_config.h (shared)

func LightFXSchema() *Schema {
	servoItem := NewSchema("servo").
		PropRange("id", FieldInt, 0, 3).
		PropRange("min_us", FieldInt, 300, 2700).
		PropRange("max_us", FieldInt, 300, 2700).
		PropRange("default_us", FieldInt, 300, 2700).
		Prop("speed", FieldInt).
		Prop("accel", FieldInt).
		Prop("decel", FieldInt).
		Prop("reversed", FieldBool)

	landingItem := NewSchema("landing_group").
		Prop("name", FieldString).
		PropRange("servo_id", FieldInt, 0, 3).
		Prop("channel_mask", FieldInt).
		PropRange("brightness", FieldInt, 0, 100)

	bandItem := NewSchema("band").
		PropRange("min_us", FieldInt, 800, 2200).
		PropRange("max_us", FieldInt, 800, 2200).
		PropRange("program", FieldInt, 0, 3)

	inputSchema := NewSchema("input").
		PropRange("channel", FieldInt, 1, 10).
		Seq("bands", 8, bandItem)

	eventItem := NewSchema("event").
		PropEnum("type", "on", "off", "flash", "fade_in", "fade_out", "fading", "beacon").
		Prop("duration_ms", FieldInt).
		Prop("cycle_ms", FieldInt).
		PropRange("brightness", FieldInt, 0, 100).
		PropRange("pwm_duty", FieldInt, 0, 100).
		PropRange("duty", FieldInt, 0, 100).
		PropRange("flash_pct", FieldInt, 0, 100).
		PropRange("min_brightness", FieldInt, 0, 100).
		PropRange("max_brightness", FieldInt, 0, 100)

	channelItem := NewSchema("channel").
		PropRange("channel", FieldInt, 0, 8).
		Prop("group", FieldInt).
		Seq("events", 8, eventItem)

	programItem := NewSchema("program").
		Prop("name", FieldString).
		Seq("channels", 8, channelItem)

	return NewSchema("LightFX").
		PropRange("master_brightness", FieldInt, 0, 100).
		Seq("servos", 3, servoItem).
		Seq("landing_groups", 3, landingItem).
		SubGroup("input", inputSchema).
		Seq("programs", 4, programItem)
}
