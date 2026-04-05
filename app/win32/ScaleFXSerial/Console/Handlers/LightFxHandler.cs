using ScaleFX.Serial.Api;
using ScaleFX.Serial.Commands;

namespace ScaleFX.Serial.Console.Handlers;

/// <summary>
/// LightFX commands: LED control, sequences, servo, landing lights.
/// </summary>
public class LightFxHandler : ICommandHandler
{
    public string GroupName => "LightFX";

    private static readonly List<CommandInfo> _commands =
    [
        new("led",             "led <ch> <brightness>",                               "Set LED brightness"),
        new("led.off",         "led.off <ch>",                                        "Turn off LED channel"),
        new("led.status",      "led.status",                                          "Query all LED states"),
        new("seq.add",         "seq.add <ch> <type> [params...]",                     "Add sequence event"),
        new("seq.clear",       "seq.clear <ch>",                                      "Clear sequence"),
        new("seq.start",       "seq.start <ch> [loops]",                              "Start sequence"),
        new("seq.stop",        "seq.stop <ch>",                                       "Stop sequence"),
        new("seq.restart",     "seq.restart <ch>",                                    "Restart sequence"),
        new("seq.status",      "seq.status <ch>",                                     "Query sequence state"),
        new("seq.queue",       "seq.queue <ch>",                                      "Query sequence queue"),
        new("brightness",      "brightness <0-100>",                                  "Set master brightness"),
        new("servo",           "servo set <id> <pulse_us>",                           "Move servo"),
        new("servo.config",    "servo.config <id> <min> <max> [spd] [acc] [dec]",     "Configure servo"),
        new("landing.bind",    "landing.bind <slot> <servo> <led> <dep> <ret> <bri>", "Bind landing light"),
        new("landing.unbind",  "landing.unbind <slot>",                               "Unbind landing light"),
        new("landing.deploy",  "landing.deploy <slot>",                               "Deploy landing light"),
        new("landing.retract", "landing.retract <slot>",                              "Retract landing light"),
        new("reset",           "reset <ch>",                                          "Reset LED channel"),
        new("enable",          "enable <ch>",                                         "Enable LED channel"),
        new("disable",         "disable <ch>",                                        "Disable LED channel"),
    ];

    public IReadOnlyList<CommandInfo> Commands => _commands;

    public bool IsRelevant(string? detectedController) =>
        detectedController is null or "lightfx";

    private static bool Mine(CommandContext ctx) =>
        ctx.DetectedController is null or "lightfx";

    public async Task<bool> TryExecuteAsync(string cmd, string[] args, CommandContext ctx)
    {
        switch (cmd)
        {
            case "led":             return await DoLed(args, ctx);
            case "led.off":         return await DoLedOff(args, ctx);
            case "led.status":      return await DoLedStatus(ctx);
            case "seq.add":         return await DoSeqAdd(args, ctx);
            case "seq.clear":       return await DoSeqClear(args, ctx);
            case "seq.start":       return await DoSeqStart(args, ctx);
            case "seq.stop":        return await DoSeqStop(args, ctx);
            case "seq.restart":     return await DoSeqRestart(args, ctx);
            case "seq.status":      return await DoSeqStatus(args, ctx);
            case "seq.queue":       return await DoSeqQueue(args, ctx);
            case "brightness":      return await DoBrightness(args, ctx);
            case "servo" when Mine(ctx):       return await DoServo(args, ctx);
            case "servo.config" when Mine(ctx): return await DoServoConfig(args, ctx);
            case "landing.bind":    return await DoLandingBind(args, ctx);
            case "landing.unbind":  return await DoSlotCmd(args, ctx, "landing.unbind", (api, s) => api.LandingUnbindAsync(s), "Unbound");
            case "landing.deploy":  return await DoSlotCmd(args, ctx, "landing.deploy", (api, s) => api.LandingDeployAsync(s), "Deploying");
            case "landing.retract": return await DoSlotCmd(args, ctx, "landing.retract", (api, s) => api.LandingRetractAsync(s), "Retracting");
            case "reset" when Mine(ctx):   return await DoChCmd(args, ctx, "reset", (api, ch) => api.ResetAsync(ch), "Reset");
            case "enable" when Mine(ctx):  return await DoChCmd(args, ctx, "enable", (api, ch) => api.EnableAsync(ch), "Enabled");
            case "disable" when Mine(ctx): return await DoChCmd(args, ctx, "disable", (api, ch) => api.DisableAsync(ch), "Disabled");
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

    private static bool Query(CommandContext ctx, ApiResult r, Action<Response, IConsoleOutput> fmt)
    {
        if (r.Success && r.Response != null) fmt(r.Response, ctx.Output);
        else ctx.Output.WriteError(r.Error ?? "Query failed");
        return true;
    }

    // ─── LED ───

    private async Task<bool> DoLed(string[] args, CommandContext ctx)
    {
        if (args.Length < 2 || !byte.TryParse(args[0], out var ch) || !byte.TryParse(args[1], out var bri))
        { ctx.Output.WriteError("Usage: led <ch> <brightness>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new LightFxApi(ctx.Connection!).LedSetAsync(ch, bri), $"LED {ch} → {bri}");
    }

    private async Task<bool> DoLedOff(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var ch))
        { ctx.Output.WriteError("Usage: led.off <ch>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new LightFxApi(ctx.Connection!).LedOffAsync(ch), $"LED {ch} off");
    }

    private async Task<bool> DoLedStatus(CommandContext ctx)
    {
        if (!ctx.RequireConnection()) return true;
        return Query(ctx, await new LightFxApi(ctx.Connection!).LedStatusAsync(), ResponseFormatters.FormatLedStatus);
    }

    // ─── Sequences ───

    private async Task<bool> DoSeqAdd(string[] args, CommandContext ctx)
    {
        if (args.Length < 2)
        {
            ctx.Output.WriteError("Usage: seq.add <ch> on <duration_ms> <brightness>");
            ctx.Output.WriteLine("       seq.add <ch> off <duration_ms>");
            ctx.Output.WriteLine("       seq.add <ch> flash <period_ms> <duty%> <brightness>");
            ctx.Output.WriteLine("       seq.add <ch> fadein <duration_ms> <target>");
            ctx.Output.WriteLine("       seq.add <ch> fadeout <duration_ms>");
            ctx.Output.WriteLine("       seq.add <ch> fading <period_ms> <min> <max>");
            ctx.Output.WriteLine("       seq.add <ch> beacon <cycle_ms> <peak>");
            return true;
        }

        if (!byte.TryParse(args[0], out var ch)) { ctx.Output.WriteError("Invalid channel"); return true; }
        var type = args[1].ToLowerInvariant();

        byte evType; ushort p1 = 0, p2 = 0; byte p3 = 0, p4 = 0;
        switch (type)
        {
            case "on":
                if (args.Length < 4) { ctx.Output.WriteError("Usage: seq.add <ch> on <duration_ms> <brightness>"); return true; }
                evType = PacketTypes.LedEvent.ON;
                p1 = ushort.Parse(args[2]); p3 = byte.Parse(args[3]);
                break;
            case "off":
                if (args.Length < 3) { ctx.Output.WriteError("Usage: seq.add <ch> off <duration_ms>"); return true; }
                evType = PacketTypes.LedEvent.OFF;
                p1 = ushort.Parse(args[2]);
                break;
            case "flash":
                if (args.Length < 5) { ctx.Output.WriteError("Usage: seq.add <ch> flash <period_ms> <duty%> <brightness>"); return true; }
                evType = PacketTypes.LedEvent.FLASH;
                p1 = ushort.Parse(args[2]);
                p2 = (ushort)(byte.Parse(args[3]) * 255 / 100);
                p3 = byte.Parse(args[4]);
                break;
            case "fadein":
                if (args.Length < 4) { ctx.Output.WriteError("Usage: seq.add <ch> fadein <duration_ms> <target>"); return true; }
                evType = PacketTypes.LedEvent.FADE_IN;
                p1 = ushort.Parse(args[2]); p3 = byte.Parse(args[3]);
                break;
            case "fadeout":
                if (args.Length < 3) { ctx.Output.WriteError("Usage: seq.add <ch> fadeout <duration_ms>"); return true; }
                evType = PacketTypes.LedEvent.FADE_OUT;
                p1 = ushort.Parse(args[2]);
                break;
            case "fading":
                if (args.Length < 5) { ctx.Output.WriteError("Usage: seq.add <ch> fading <period_ms> <min> <max>"); return true; }
                evType = PacketTypes.LedEvent.FADING;
                p1 = ushort.Parse(args[2]); p3 = byte.Parse(args[3]); p4 = byte.Parse(args[4]);
                break;
            case "beacon":
                if (args.Length < 4) { ctx.Output.WriteError("Usage: seq.add <ch> beacon <cycle_ms> <peak>"); return true; }
                evType = PacketTypes.LedEvent.BEACON;
                p1 = ushort.Parse(args[2]); p3 = byte.Parse(args[3]);
                break;
            default:
                ctx.Output.WriteError($"Unknown event type: {type}. Valid: on, off, flash, fadein, fadeout, fading, beacon");
                return true;
        }

        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new LightFxApi(ctx.Connection!).SeqAddAsync(ch, evType, p1, p2, p3, p4),
            $"Seq event {type} added to ch {ch}");
    }

    private async Task<bool> DoSeqClear(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var ch))
        { ctx.Output.WriteError("Usage: seq.clear <ch>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new LightFxApi(ctx.Connection!).SeqClearAsync(ch), $"Ch {ch}: Sequence cleared");
    }

    private async Task<bool> DoSeqStart(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var ch))
        { ctx.Output.WriteError("Usage: seq.start <ch> [loops]"); return true; }
        ushort loops = args.Length > 1 && ushort.TryParse(args[1], out var l) ? l : (ushort)0;
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new LightFxApi(ctx.Connection!).SeqStartAsync(ch, loops),
            loops > 0 ? $"Ch {ch}: sequence started ({loops} loops)" : $"Ch {ch}: sequence started (infinite)");
    }

    private async Task<bool> DoSeqStop(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var ch))
        { ctx.Output.WriteError("Usage: seq.stop <ch>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new LightFxApi(ctx.Connection!).SeqStopAsync(ch), $"Ch {ch}: Sequence stopped");
    }

    private async Task<bool> DoSeqRestart(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var ch))
        { ctx.Output.WriteError("Usage: seq.restart <ch>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new LightFxApi(ctx.Connection!).SeqRestartAsync(ch), $"Ch {ch}: Sequence restarted");
    }

    private async Task<bool> DoSeqStatus(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var ch))
        { ctx.Output.WriteError("Usage: seq.status <ch>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Query(ctx, await new LightFxApi(ctx.Connection!).SeqStatusAsync(ch), ResponseFormatters.FormatLedSeqStatus);
    }

    private async Task<bool> DoSeqQueue(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var ch))
        { ctx.Output.WriteError("Usage: seq.queue <ch>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Query(ctx, await new LightFxApi(ctx.Connection!).SeqQueueAsync(ch), ResponseFormatters.FormatLedSeqQueue);
    }

    // ─── Master Brightness ───

    private async Task<bool> DoBrightness(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var val))
        { ctx.Output.WriteError("Usage: brightness <0-100>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new LightFxApi(ctx.Connection!).MasterBrightnessAsync(val), $"Master brightness → {val}");
    }

    // ─── Servo (LightFX) ───

    private async Task<bool> DoServo(string[] args, CommandContext ctx)
    {
        if (args.Length < 3 || args[0] != "set")
        { ctx.Output.WriteError("Usage: servo set <id> <pulse_us>"); return true; }
        if (!byte.TryParse(args[1], out var id) || !ushort.TryParse(args[2], out var pulse))
        { ctx.Output.WriteError("Invalid parameters"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new LightFxApi(ctx.Connection!).ServoSetAsync(id, pulse), $"Servo {id} → {pulse}µs");
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
        return Ack(ctx, await new LightFxApi(ctx.Connection!).ServoConfigAsync(id, min, max, spd, acc, dec),
            $"Servo {id} config: {min}-{max}µs");
    }

    // ─── Landing Lights ───

    private async Task<bool> DoLandingBind(string[] args, CommandContext ctx)
    {
        if (args.Length < 6)
        { ctx.Output.WriteError("Usage: landing.bind <slot> <servo> <led> <deploy_us> <retract_us> <brightness>"); return true; }
        if (!byte.TryParse(args[0], out var slot) || !byte.TryParse(args[1], out var servo)
            || !byte.TryParse(args[2], out var led) || !ushort.TryParse(args[3], out var dep)
            || !ushort.TryParse(args[4], out var ret) || !byte.TryParse(args[5], out var bri))
        { ctx.Output.WriteError("Invalid parameters"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await new LightFxApi(ctx.Connection!).LandingBindAsync(slot, servo, led, dep, ret, bri),
            $"Landing light {slot} bound");
    }

    private async Task<bool> DoSlotCmd(string[] args, CommandContext ctx, string usage,
        Func<LightFxApi, byte, Task<ApiResult>> action, string msg)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var slot))
        { ctx.Output.WriteError($"Usage: {usage} <slot>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await action(new LightFxApi(ctx.Connection!), slot), $"Landing light {slot}: {msg}");
    }

    // ─── Reset / Enable / Disable ───

    private async Task<bool> DoChCmd(string[] args, CommandContext ctx, string name,
        Func<LightFxApi, byte, Task<ApiResult>> action, string msg)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var ch))
        { ctx.Output.WriteError($"Usage: {name} <ch>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await action(new LightFxApi(ctx.Connection!), ch), $"Ch {ch}: {msg}");
    }
}
