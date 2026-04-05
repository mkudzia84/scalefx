using ScaleFX.Serial.Api;
using ScaleFX.Serial.Commands;

namespace ScaleFX.Serial.Console.Handlers;

/// <summary>
/// GunFX commands: trigger, servo, smoke.
/// </summary>
public class GunFxHandler : ICommandHandler
{
    public string GroupName => "GunFX";

    private static readonly List<CommandInfo> _commands =
    [
        new("trigger",       "trigger on <rpm> | off [delay_ms]",                         "Fire control"),
        new("servo",         "servo set <id> <pulse_us>",                                 "Move servo"),
        new("servo.config",  "servo.config <id> <min> <max> [spd] [acc] [dec]",           "Configure servo"),
        new("servo.recoil",  "servo.recoil <id> <jerk_us> <variance>",                    "Set recoil jerk"),
        new("smoke",         "smoke on|off",                                               "Toggle smoke heater"),
        new("smoke.config",  "smoke.config <pulsing> <speed> <high> <low> <pulse> <spin>","Configure smoke fan"),
        new("smoke.reset",   "smoke.reset",                                                "Reset smoke system"),
        new("smoke.limit",   "smoke.limit <target_mA> <limit_mA>",                        "Set current limit"),
    ];

    public IReadOnlyList<CommandInfo> Commands => _commands;

    public bool IsRelevant(string? detectedController) =>
        detectedController is null or "gunfx";

    private static bool Mine(CommandContext ctx) =>
        ctx.DetectedController is null or "gunfx";

    public async Task<bool> TryExecuteAsync(string cmd, string[] args, CommandContext ctx)
    {
        switch (cmd)
        {
            case "trigger": return await DoTrigger(args, ctx);
            case "servo" when Mine(ctx): return await DoServo(args, ctx);
            case "servo.config" when Mine(ctx): return await DoServoConfig(args, ctx);
            case "servo.recoil": return await DoServoRecoil(args, ctx);
            case "smoke": return await DoSmoke(args, ctx);
            case "smoke.config": return await DoSmokeConfig(args, ctx);
            case "smoke.reset":
                if (!ctx.RequireConnection()) return true;
                return Ack(ctx, await new GunFxApi(ctx.Connection!).SmokeResetAsync(), "Smoke system reset");
            case "smoke.limit": return await DoSmokeLimit(args, ctx);
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

    // ─── Trigger ───

    private async Task<bool> DoTrigger(string[] args, CommandContext ctx)
    {
        if (args.Length < 1) { ctx.Output.WriteError("Usage: trigger on <rpm> | off [delay_ms]"); return true; }
        if (!ctx.RequireConnection()) return true;
        var api = new GunFxApi(ctx.Connection!);

        if (args[0] == "on")
        {
            if (args.Length < 2 || !ushort.TryParse(args[1], out var rpm))
            { ctx.Output.WriteError("Usage: trigger on <rpm>"); return true; }
            return Ack(ctx, await api.TriggerOnAsync(rpm), $"Firing at {rpm} RPM");
        }
        if (args[0] == "off")
        {
            ushort delay = args.Length > 1 && ushort.TryParse(args[1], out var d) ? d : (ushort)0;
            return Ack(ctx, await api.TriggerOffAsync(delay), "Trigger off");
        }
        ctx.Output.WriteError("Usage: trigger on <rpm> | off [delay_ms]");
        return true;
    }

    private async Task<bool> DoServo(string[] args, CommandContext ctx)
    {
        if (args.Length < 3 || args[0] != "set")
        { ctx.Output.WriteError("Usage: servo set <id> <pulse_us>"); return true; }
        if (!byte.TryParse(args[1], out var id) || !ushort.TryParse(args[2], out var pulse))
        { ctx.Output.WriteError("Invalid servo id or pulse value"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GunFxApi(ctx.Connection!).ServoSetAsync(id, pulse), $"Servo {id} → {pulse}µs");
    }

    private async Task<bool> DoServoConfig(string[] args, CommandContext ctx)
    {
        if (args.Length < 3) { ctx.Output.WriteError("Usage: servo.config <id> <min> <max> [spd] [acc] [dec]"); return true; }
        if (!byte.TryParse(args[0], out var id) || !ushort.TryParse(args[1], out var min)
            || !ushort.TryParse(args[2], out var max))
        { ctx.Output.WriteError("Invalid parameters"); return true; }
        ushort spd = args.Length > 3 && ushort.TryParse(args[3], out var s) ? s : (ushort)0;
        ushort acc = args.Length > 4 && ushort.TryParse(args[4], out var a) ? a : (ushort)0;
        ushort dec = args.Length > 5 && ushort.TryParse(args[5], out var d) ? d : (ushort)0;
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GunFxApi(ctx.Connection!).ServoConfigAsync(id, min, max, spd, acc, dec),
            $"Servo {id} config: {min}-{max}µs");
    }

    private async Task<bool> DoServoRecoil(string[] args, CommandContext ctx)
    {
        if (args.Length < 3 || !byte.TryParse(args[0], out var id)
            || !ushort.TryParse(args[1], out var jerk) || !byte.TryParse(args[2], out var var_))
        { ctx.Output.WriteError("Usage: servo.recoil <id> <jerk_us> <variance>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GunFxApi(ctx.Connection!).ServoRecoilAsync(id, jerk, var_),
            $"Servo {id} recoil: jerk={jerk}µs var={var_}");
    }

    private async Task<bool> DoSmoke(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || args[0] is not ("on" or "off"))
        { ctx.Output.WriteError("Usage: smoke on|off"); return true; }
        if (!ctx.RequireConnection()) return true;
        bool on = args[0] == "on";
        return Ack(ctx, await new GunFxApi(ctx.Connection!).SmokeHeatAsync(on),
            on ? "Smoke heater ON" : "Smoke heater OFF");
    }

    private async Task<bool> DoSmokeConfig(string[] args, CommandContext ctx)
    {
        if (args.Length < 6) { ctx.Output.WriteError("Usage: smoke.config <pulsing> <speed> <high> <low> <pulse_ms> <spindown_ms>"); return true; }
        if (!byte.TryParse(args[0], out var pulsingByte) || !byte.TryParse(args[1], out var speed)
            || !byte.TryParse(args[2], out var high) || !byte.TryParse(args[3], out var low)
            || !ushort.TryParse(args[4], out var pulse) || !ushort.TryParse(args[5], out var spin))
        { ctx.Output.WriteError("Invalid parameters"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GunFxApi(ctx.Connection!).SmokeConfigAsync(pulsingByte != 0, speed, high, low, pulse, spin),
            "Smoke config updated");
    }

    private async Task<bool> DoSmokeLimit(string[] args, CommandContext ctx)
    {
        if (args.Length < 2 || !byte.TryParse(args[0], out var target) || !ushort.TryParse(args[1], out var limit))
        { ctx.Output.WriteError("Usage: smoke.limit <target_mA> <limit_mA>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new GunFxApi(ctx.Connection!).SmokeLimitAsync(target, limit),
            $"Smoke limit: target={target}mA limit={limit}mA");
    }
}
