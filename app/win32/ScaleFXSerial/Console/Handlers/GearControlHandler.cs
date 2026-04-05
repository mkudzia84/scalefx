using ScaleFX.Serial.Api;
using ScaleFX.Serial.Commands;

namespace ScaleFX.Serial.Console.Handlers;

/// <summary>
/// GearControl commands: deploy/retract, servo, calibration, battery.
/// </summary>
public class GearControlHandler : ICommandHandler
{
    public string GroupName => "GearControl";

    private static readonly List<CommandInfo> _commands =
    [
        new("deploy",        "deploy <id>|all",                                   "Deploy landing gear"),
        new("retract",       "retract <id>|all",                                  "Retract landing gear"),
        new("stop",          "stop <id>|all",                                     "Emergency stop motor"),
        new("servo",         "servo set <id> <pulse_us>",                         "Move servo"),
        new("servo.config",  "servo.config <id> <min> <max> [spd] [acc] [dec]",   "Configure servo"),
        new("gear.config",   "gear.config <id> <flags> <stall_mA> <timeout_ms>",  "Configure gear"),
        new("door.config",   "door.config <id> <op0> <cl0> <op1> <cl1>",          "Configure door servos"),
        new("door.mode",     "door.mode <id> <pre> <post> <delay_ms>",            "Set door timing mode"),
        new("yaw.config",    "yaw.config <id> <neutral> <min> <max>",             "Configure yaw compensation"),
        new("yaw",           "yaw <position_us>",                                 "Feed yaw position"),
        new("calibrate",     "calibrate <id> [timeout_s]",                         "Calibrate stall current"),
        new("calibrate.cancel","calibrate.cancel <id>",                            "Cancel calibration"),
        new("battery",       "battery <enable> <auto_deploy>",                     "Configure battery monitor"),
        new("reset",         "reset <id>",                                         "Clear error state"),
        new("enable",        "enable <id>",                                        "Enable gear channel"),
        new("disable",       "disable <id>",                                       "Disable gear channel"),
    ];

    public IReadOnlyList<CommandInfo> Commands => _commands;

    public bool IsRelevant(string? detectedController) =>
        detectedController is null or "gearcontrol";

    private static bool Mine(CommandContext ctx) =>
        ctx.DetectedController is null or "gearcontrol";

    public async Task<bool> TryExecuteAsync(string cmd, string[] args, CommandContext ctx)
    {
        switch (cmd)
        {
            case "deploy":       return await DoAction(args, ctx, "deploy", api => api.AllDeployAsync(), api => api.DeployAsync, "Deploying");
            case "retract":      return await DoAction(args, ctx, "retract", api => api.AllRetractAsync(), api => api.RetractAsync, "Retracting");
            case "stop":         return await DoAction(args, ctx, "stop", api => api.AllStopAsync(), api => api.StopAsync, "Stopped");
            case "servo" when Mine(ctx):       return await DoServo(args, ctx);
            case "servo.config" when Mine(ctx): return await DoServoConfig(args, ctx);
            case "gear.config":  return await DoGearConfig(args, ctx);
            case "door.config":  return await DoDoorConfig(args, ctx);
            case "door.mode":    return await DoDoorMode(args, ctx);
            case "yaw.config":   return await DoYawConfig(args, ctx);
            case "yaw":          return await DoYawInput(args, ctx);
            case "calibrate":    return await DoCalibrate(args, ctx);
            case "calibrate.cancel": return await DoIdCmd(args, ctx, "calibrate.cancel",
                                    (api, id) => api.CalibrateCancelAsync(id), "Calibration cancelled");
            case "battery":      return await DoBatteryConfig(args, ctx);
            case "reset" when Mine(ctx):   return await DoIdCmd(args, ctx, "reset",
                                    (api, id) => api.ResetAsync(id), "Reset");
            case "enable" when Mine(ctx):  return await DoIdCmd(args, ctx, "enable",
                                    (api, id) => api.EnableAsync(id, true), "Enabled");
            case "disable" when Mine(ctx): return await DoIdCmd(args, ctx, "disable",
                                    (api, id) => api.EnableAsync(id, false), "Disabled");
            default: return false;
        }
    }

    // ─── Helpers ───

    private static bool Ack(CommandContext ctx, ApiResult r, string msg)
    {
        if (r.Success) ctx.Output.WriteSuccess(msg);
        else ctx.Output.WriteError(r.Error ?? "Command failed");
        return true;
    }

    // ─── Deploy / Retract / Stop ───

    private async Task<bool> DoAction(string[] args, CommandContext ctx, string name,
        Func<GearControlApi, Task<ApiResult>> allAction,
        Func<GearControlApi, Func<byte, CancellationToken, Task<ApiResult>>> singleAction,
        string msg)
    {
        if (args.Length < 1) { ctx.Output.WriteError($"Usage: {name} <id>|all"); return true; }
        if (!ctx.RequireConnection()) return true;
        var api = new GearControlApi(ctx.Connection!);

        if (args[0].Equals("all", StringComparison.OrdinalIgnoreCase))
        {
            var result = await allAction(api);
            if (result.Success)
            {
                // "all" commands may return GEAR_SEQ_STATUS as intermediate response
                if (result.Response?.PacketType == PacketTypes.GearControl.GEAR_SEQ_STATUS)
                {
                    ResponseFormatters.FormatGearSeqStatus(result.Response, ctx.Output);
                    ctx.Output.WriteInfo("Waiting for remaining status updates...");
                }
                else
                    ctx.Output.WriteSuccess($"{msg} ALL gears");
            }
            else
                ctx.Output.WriteError(result.Error ?? $"{name} all failed");
        }
        else
        {
            if (!byte.TryParse(args[0], out var id))
            { ctx.Output.WriteError($"Invalid gear id: {args[0]}"); return true; }
            return Ack(ctx, await singleAction(api)(id, default), $"Gear {id}: {msg}");
        }
        return true;
    }

    // ─── Servo (GearControl) ───

    private async Task<bool> DoServo(string[] args, CommandContext ctx)
    {
        if (args.Length < 3 || args[0] != "set")
        { ctx.Output.WriteError("Usage: servo set <id> <pulse_us>"); return true; }
        if (!byte.TryParse(args[1], out var id) || !ushort.TryParse(args[2], out var pulse))
        { ctx.Output.WriteError("Invalid parameters"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GearControlApi(ctx.Connection!).ServoSetAsync(id, pulse), $"Servo {id} → {pulse}µs");
    }

    private async Task<bool> DoServoConfig(string[] args, CommandContext ctx)
    {
        if (args.Length < 3) { ctx.Output.WriteError("Usage: servo.config <id> <min> <max> [spd] [acc] [dec]"); return true; }
        if (!byte.TryParse(args[0], out var id) || !ushort.TryParse(args[1], out var min)
            || !ushort.TryParse(args[2], out var max))
        { ctx.Output.WriteError("Invalid parameters"); return true; }
        ushort spd = args.Length > 3 && ushort.TryParse(args[3], out var s) ? s : (ushort)4000;
        ushort acc = args.Length > 4 && ushort.TryParse(args[4], out var a) ? a : (ushort)8000;
        ushort dec = args.Length > 5 && ushort.TryParse(args[5], out var d) ? d : (ushort)8000;
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GearControlApi(ctx.Connection!).ServoConfigAsync(id, min, max, spd, acc, dec),
            $"Servo {id} config: {min}-{max}µs");
    }

    // ─── Config commands ───

    private async Task<bool> DoGearConfig(string[] args, CommandContext ctx)
    {
        if (args.Length < 2) { ctx.Output.WriteError("Usage: gear.config <id> <flags> [stall_mA] [timeout_ms]"); return true; }
        if (!byte.TryParse(args[0], out var id) || !byte.TryParse(args[1], out var flags))
        { ctx.Output.WriteError("Invalid parameters"); return true; }
        ushort stall = args.Length > 2 && ushort.TryParse(args[2], out var s) ? s : (ushort)0;
        ushort timeout = args.Length > 3 && ushort.TryParse(args[3], out var t) ? t : (ushort)0;
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GearControlApi(ctx.Connection!).GearConfigAsync(id, flags, stall, timeout), "Gear config updated");
    }

    private async Task<bool> DoDoorConfig(string[] args, CommandContext ctx)
    {
        if (args.Length < 5 || !byte.TryParse(args[0], out var id)
            || !ushort.TryParse(args[1], out var op0) || !ushort.TryParse(args[2], out var cl0)
            || !ushort.TryParse(args[3], out var op1) || !ushort.TryParse(args[4], out var cl1))
        { ctx.Output.WriteError("Usage: door.config <id> <open0> <close0> <open1> <close1>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GearControlApi(ctx.Connection!).DoorConfigAsync(id, op0, cl0, op1, cl1), "Door config updated");
    }

    private async Task<bool> DoDoorMode(string[] args, CommandContext ctx)
    {
        if (args.Length < 4 || !byte.TryParse(args[0], out var id)
            || !byte.TryParse(args[1], out var pre) || !byte.TryParse(args[2], out var post)
            || !ushort.TryParse(args[3], out var delay))
        { ctx.Output.WriteError("Usage: door.mode <id> <pre> <post> <delay_ms>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GearControlApi(ctx.Connection!).DoorModeAsync(id, pre, post, delay), "Door mode updated");
    }

    private async Task<bool> DoYawConfig(string[] args, CommandContext ctx)
    {
        if (args.Length < 4 || !byte.TryParse(args[0], out var id)
            || !ushort.TryParse(args[1], out var neutral) || !ushort.TryParse(args[2], out var min)
            || !ushort.TryParse(args[3], out var max))
        { ctx.Output.WriteError("Usage: yaw.config <id> <neutral> <min> <max>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GearControlApi(ctx.Connection!).YawConfigAsync(id, neutral, min, max), "Yaw config updated");
    }

    private async Task<bool> DoYawInput(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !ushort.TryParse(args[0], out var pos))
        { ctx.Output.WriteError("Usage: yaw <position_us>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GearControlApi(ctx.Connection!).YawInputAsync(pos), $"Yaw input → {pos}µs");
    }

    // ─── Calibration ───

    private async Task<bool> DoCalibrate(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var id))
        { ctx.Output.WriteError("Usage: calibrate <id> [timeout_s]"); return true; }
        byte timeout = args.Length > 1 && byte.TryParse(args[1], out var t) ? t : (byte)0;
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GearControlApi(ctx.Connection!).CalibrateAsync(id, timeout),
            $"Gear {id}: calibration started");
    }

    // ─── Battery ───

    private async Task<bool> DoBatteryConfig(string[] args, CommandContext ctx)
    {
        if (args.Length < 2) { ctx.Output.WriteError("Usage: battery <enable 0|1> <auto_deploy 0|1>"); return true; }
        bool en = args[0] is "1" or "true" or "on";
        bool ad = args[1] is "1" or "true" or "on";
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GearControlApi(ctx.Connection!).BatteryConfigAsync(en, ad),
            $"Battery monitor: {(en ? "enabled" : "disabled")}, auto-deploy: {(ad ? "on" : "off")}");
    }

    // ─── Generic ID command helper ───

    private async Task<bool> DoIdCmd(string[] args, CommandContext ctx, string usage,
        Func<GearControlApi, byte, Task<ApiResult>> action, string msg)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var id))
        { ctx.Output.WriteError($"Usage: {usage} <id>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await action(new GearControlApi(ctx.Connection!), id), $"Gear {id}: {msg}");
    }
}
