using System.ComponentModel;
using YamlDotNet.Serialization;

namespace ScaleFXStudio.Models;

/// <summary>
/// Root configuration for ScaleFX Hub system
/// </summary>
public class ScaleFXConfiguration
{
    [Category("Engine FX")]
    [Description("Engine sound effects configuration")]
    [YamlMember(Alias = "engine_fx")]
    public EngineFxConfig? EngineFx { get; set; }

    [Category("Gun FX")]
    [Description("Gun effects configuration")]
    [YamlMember(Alias = "gun_fx")]
    public GunFxConfig? GunFx { get; set; }
}

public class EngineFxConfig
{
    [Category("General")]
    [Description("Enable or disable engine effects (UI only - not serialized)")]
    [YamlIgnore]
    public bool Enabled { get; set; } = true;

    [Category("General")]
    [Description("Engine type: turbine, radial, diesel")]
    [YamlMember(Alias = "type")]
    public string Type { get; set; } = "turbine";

    [Category("Engine Toggle")]
    [Description("Engine toggle PWM input configuration")]
    [YamlMember(Alias = "engine_toggle")]
    public PwmInputConfig? EngineToggle { get; set; }

    [Category("Sounds")]
    [Description("Engine sound files configuration")]
    [YamlMember(Alias = "sounds")]
    public EngineSoundsConfig? Sounds { get; set; }
}

public class PwmInputConfig
{
    [Category("PWM")]
    [Description("Input channel number (1-12)")]
    [YamlMember(Alias = "input_channel")]
    public int InputChannel { get; set; }

    [Category("PWM")]
    [Description("PWM threshold in microseconds")]
    [YamlMember(Alias = "threshold_us")]
    public int ThresholdUs { get; set; } = 1500;
}

public class EngineSoundsConfig
{
    [Category("Sounds")]
    [Description("Engine starting sound file path")]
    [YamlMember(Alias = "starting")]
    public string? Starting { get; set; }

    [Category("Sounds")]
    [Description("Engine running loop sound file path")]
    [YamlMember(Alias = "running")]
    public string? Running { get; set; }

    [Category("Sounds")]
    [Description("Engine stopping sound file path")]
    [YamlMember(Alias = "stopping")]
    public string? Stopping { get; set; }

    [Category("Transitions")]
    [Description("Sound transition timing")]
    [YamlMember(Alias = "transitions")]
    public TransitionsConfig? Transitions { get; set; }
}

public class SoundFileConfig
{
    [Category("Sound")]
    [Description("Path to sound file")]
    [Editor(typeof(System.Windows.Forms.Design.FileNameEditor), typeof(System.Drawing.Design.UITypeEditor))]
    [YamlMember(Alias = "file")]
    public string? File { get; set; }
}

public class TransitionsConfig
{
    [Category("Timing")]
    [Description("Offset in milliseconds when starting")]
    [YamlMember(Alias = "starting_offset_ms")]
    public int StartingOffsetMs { get; set; }

    [Category("Timing")]
    [Description("Offset in milliseconds when stopping")]
    [YamlMember(Alias = "stopping_offset_ms")]
    public int StoppingOffsetMs { get; set; }
}

public class GunFxConfig
{
    [Category("General")]
    [Description("Enable or disable gun effects (UI only - not serialized)")]
    [YamlIgnore]
    public bool Enabled { get; set; } = true;

    [Category("Trigger")]
    [Description("Gun trigger PWM input configuration")]
    [YamlMember(Alias = "trigger")]
    public TriggerConfig? Trigger { get; set; }

    [Category("Smoke")]
    [Description("Smoke generator configuration")]
    [YamlMember(Alias = "smoke")]
    public SmokeConfig? Smoke { get; set; }

    [Category("Turret Control")]
    [Description("Turret servo control configuration")]
    [YamlMember(Alias = "turret_control")]
    public TurretControlConfig? TurretControl { get; set; }

    [Category("Rate of Fire")]
    [Description("Rate of fire configurations")]
    [YamlMember(Alias = "rates_of_fire")]
    public List<RateOfFireConfig>? RateOfFire { get; set; }
}

public class TriggerConfig
{
    [Category("Trigger")]
    [Description("Input channel for trigger PWM (1-12)")]
    [YamlMember(Alias = "input_channel")]
    public int InputChannel { get; set; }
}

public class SmokeConfig
{
    [Category("General")]
    [Description("Enable smoke generator (UI only - not serialized)")]
    [YamlIgnore]
    public bool Enabled { get; set; } = true;

    [Category("Heater")]
    [Description("Input channel for heater toggle PWM (1-12)")]
    [YamlMember(Alias = "heater_toggle_channel")]
    public int HeaterToggleChannel { get; set; }

    [Category("Heater")]
    [Description("PWM threshold for heater activation (µs)")]
    [YamlMember(Alias = "heater_pwm_threshold_us")]
    public int HeaterPwmThresholdUs { get; set; } = 1500;

    [Category("Fan")]
    [Description("Delay before turning smoke fan off after firing stops (ms)")]
    [YamlMember(Alias = "fan_off_delay_ms")]
    public int FanOffDelayMs { get; set; } = 2000;
}

public class TurretControlConfig
{
    [Category("Pitch")]
    [Description("Pitch axis servo configuration")]
    [YamlMember(Alias = "pitch")]
    public ServoConfig? Pitch { get; set; }

    [Category("Yaw")]
    [Description("Yaw axis servo configuration")]
    [YamlMember(Alias = "yaw")]
    public ServoConfig? Yaw { get; set; }
}

public class ServoConfig
{
    [Category("General")]
    [Description("Enable this servo axis (UI only - not serialized)")]
    [YamlIgnore]
    public bool Enabled { get; set; } = true;

    [Category("General")]
    [Description("Servo ID (1-3)")]
    [YamlMember(Alias = "servo_id")]
    public int ServoId { get; set; } = 1;

    [Category("PWM Input")]
    [Description("Input channel (1-12)")]
    [YamlMember(Alias = "input_channel")]
    public int InputChannel { get; set; }

    [Category("PWM Input")]
    [Description("Minimum input PWM (µs)")]
    [YamlMember(Alias = "input_min_us")]
    public int InputMinUs { get; set; } = 1000;

    [Category("PWM Input")]
    [Description("Maximum input PWM (µs)")]
    [YamlMember(Alias = "input_max_us")]
    public int InputMaxUs { get; set; } = 2000;

    [Category("PWM Output")]
    [Description("Minimum output PWM (µs)")]
    [YamlMember(Alias = "output_min_us")]
    public int OutputMinUs { get; set; } = 1000;

    [Category("PWM Output")]
    [Description("Maximum output PWM (µs)")]
    [YamlMember(Alias = "output_max_us")]
    public int OutputMaxUs { get; set; } = 2000;

    [Category("Motion")]
    [Description("Maximum speed (µs/sec)")]
    [YamlMember(Alias = "max_speed_us_per_sec")]
    public double MaxSpeedUsPerSec { get; set; } = 500.0;

    [Category("Motion")]
    [Description("Maximum acceleration (µs/sec²)")]
    [YamlMember(Alias = "max_accel_us_per_sec2")]
    public double MaxAccelUsPerSec2 { get; set; } = 2000.0;

    [Category("Motion")]
    [Description("Maximum deceleration (µs/sec²)")]
    [YamlMember(Alias = "max_decel_us_per_sec2")]
    public double MaxDecelUsPerSec2 { get; set; } = 2000.0;

    [Category("Recoil")]
    [Description("Recoil jerk offset per shot (µs, 0=disabled)")]
    [YamlMember(Alias = "recoil_jerk_us")]
    public int RecoilJerkUs { get; set; } = 0;

    [Category("Recoil")]
    [Description("Random variance range for recoil jerk (µs)")]
    [YamlMember(Alias = "recoil_jerk_variance_us")]
    public int RecoilJerkVarianceUs { get; set; } = 0;
}

public class RateOfFireConfig
{
    [Category("Rate")]
    [Description("Display name for this rate")]
    [YamlMember(Alias = "name")]
    public string? Name { get; set; }

    [Category("Rate")]
    [Description("Rounds per minute")]
    [YamlMember(Alias = "rpm")]
    public int Rpm { get; set; }

    [Category("Rate")]
    [Description("PWM threshold to activate this rate (µs)")]
    [YamlMember(Alias = "pwm_threshold_us")]
    public int PwmThresholdUs { get; set; }

    [Category("Sound")]
    [Description("Sound file for this firing rate")]
    [Editor(typeof(System.Windows.Forms.Design.FileNameEditor), typeof(System.Drawing.Design.UITypeEditor))]
    [YamlMember(Alias = "sound_file")]
    public string? SoundFile { get; set; }

    public override string ToString() => Name ?? $"{Rpm} RPM";
}
