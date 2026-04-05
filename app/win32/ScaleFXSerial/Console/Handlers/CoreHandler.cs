using ScaleFX.Serial.Api;
using ScaleFX.Serial.Commands;

namespace ScaleFX.Serial.Console.Handlers;

/// <summary>
/// Core protocol commands: connect, init, status, reboot, etc.
/// </summary>
public class CoreHandler : ICommandHandler
{
    public string GroupName => "Core";

    private static readonly List<CommandInfo> _commands =
    [
        new("connect",    "connect <port> [baud]",  "Connect to serial port"),
        new("disconnect", "disconnect",             "Disconnect from port"),
        new("reconnect",  "reconnect",              "Reconnect to same port"),
        new("ports",      "ports",                  "List available serial ports"),
        new("init",       "init",                   "Initialize controller"),
        new("identify",   "identify",               "Identify controller (no init)"),
        new("shutdown",   "shutdown",               "Shutdown controller"),
        new("status",     "status",                 "Request controller status"),
        new("reboot",     "reboot",                 "Reboot controller"),
        new("bootsel",    "bootsel",                "Enter BOOTSEL/DFU mode"),
        new("i2c.scan",   "i2c.scan",               "Scan I2C bus"),
        new("diag",       "diag [count]",           "Request diagnostic log"),
        new("keepalive",  "keepalive",              "Send keepalive ping"),
        new("verbose",    "verbose [on|off]",       "Toggle verbose output"),
        new("clear",      "clear",                  "Clear console output"),
    ];

    public IReadOnlyList<CommandInfo> Commands => _commands;

    public async Task<bool> TryExecuteAsync(string cmd, string[] args, CommandContext ctx)
    {
        // ─── Local commands (no connection needed) ───
        switch (cmd)
        {
            case "connect":    await DoConnect(args, ctx); return true;
            case "disconnect": DoDisconnect(ctx); return true;
            case "reconnect":  await DoReconnect(ctx); return true;
            case "ports":      DoPorts(ctx); return true;
            case "verbose":    DoVerbose(args, ctx); return true;
            case "clear":      return true;
        }

        // ─── Remote commands (connection required) ───
        if (!ctx.RequireConnection()) return true;
        var api = new CoreApi(ctx.Connection!);

        switch (cmd)
        {
            case "init":       await DoInit(api, ctx); return true;
            case "identify":   await DoIdentify(api, ctx); return true;
            case "shutdown":   return Ack(ctx, await api.ShutdownAsync(), "Controller shut down");
            case "status":     await DoStatus(api, ctx); return true;
            case "reboot":     return Ack(ctx, await api.RebootAsync(), "Rebooting...");
            case "bootsel":    return Ack(ctx, await api.BootselAsync(), "Entering BOOTSEL/DFU mode...");
            case "i2c.scan":   return Query(ctx, await api.I2CScanAsync(), ResponseFormatters.FormatI2CScan);
            case "diag":       await DoDiag(api, args, ctx); return true;
            case "keepalive":  return Ack(ctx, await api.KeepaliveAsync(), "Keepalive OK");
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

    // ─── Connect / Disconnect ───

    private async Task DoConnect(string[] args, CommandContext ctx)
    {
        if (ctx.IsConnected) { ctx.Output.WriteWarning("Already connected. Use 'disconnect' first."); return; }
        if (args.Length < 1) { ctx.Output.WriteError("Usage: connect <port> [baud]"); DoPorts(ctx); return; }

        string port = args[0];
        int baud = args.Length > 1 && int.TryParse(args[1], out var b) ? b : ScaleFxConnection.DefaultBaud;

        try
        {
            var conn = new ScaleFxConnection(port, baud, ctx.Verbose);
            conn.OnAsyncPacket += ctx.HandleAsyncPacket;
            conn.Connect();
            ctx.Connection = conn;
            ctx.Output.WriteSuccess($"Connected to {port} at {baud:N0} baud");

            // Auto-identify the controller
            await DoIdentify(new CoreApi(conn), ctx);
        }
        catch (Exception ex)
        {
            ctx.Output.WriteError($"Connection failed: {ex.Message}");
            ctx.Connection = null;
        }
    }

    private void DoDisconnect(CommandContext ctx)
    {
        if (!ctx.IsConnected) { ctx.Output.WriteWarning("Not connected"); return; }
        ctx.Connection!.Close();
        ctx.Connection = null;
        ctx.DetectedController = null;
        ctx.Output.WriteSuccess("Disconnected");
    }

    private async Task DoReconnect(CommandContext ctx)
    {
        if (ctx.Connection == null) { ctx.Output.WriteError("No previous connection"); return; }
        try
        {
            ctx.Connection.Reconnect();
            ctx.Output.WriteSuccess("Reconnected");
            await DoIdentify(new CoreApi(ctx.Connection), ctx);
        }
        catch (Exception ex)
        {
            ctx.Output.WriteError($"Reconnect failed: {ex.Message}");
        }
    }

    private void DoPorts(CommandContext ctx)
    {
        var ports = ScaleFxConnection.ListPorts();
        if (ports.Length == 0) { ctx.Output.WriteWarning("No serial ports found"); return; }
        ctx.Output.WriteInfo("Available ports:");
        foreach (var p in ports) ctx.Output.WriteLine($"  {p}");
    }

    // ─── Init / Identify / Status ───

    private async Task DoInit(CoreApi api, CommandContext ctx)
    {
        var result = await api.InitAsync();
        if (!result.Success) { ctx.Output.WriteError(result.Error ?? "INIT failed"); return; }

        var resp = result.Response!;
        if (resp.IsInitReady || resp.PacketType == PacketTypes.Core.INIT_READY)
        {
            ResponseFormatters.FormatInitReady(resp, ctx.Output);
            DetectController(resp, ctx);
        }
        else if (resp.IsAck)
            ctx.Output.WriteSuccess("INIT acknowledged");
        else
            ResponseFormatters.FormatHexDump(resp, ctx.Output);
    }

    private async Task DoIdentify(CoreApi api, CommandContext ctx)
    {
        var result = await api.IdentifyAsync();
        if (!result.Success)
        {
            // IDENTIFY not supported — fall back to INIT
            if (result.Response?.IsNack == true)
            {
                ctx.Output.WriteWarning("IDENTIFY not supported, trying INIT...");
                await DoInit(api, ctx);
                return;
            }
            ctx.Output.WriteError(result.Error ?? "IDENTIFY failed");
            return;
        }

        var resp = result.Response!;
        if (resp.PacketType == PacketTypes.Core.IDENTIFY || resp.IsInitReady)
        {
            ResponseFormatters.FormatInitReady(resp, ctx.Output);
            DetectController(resp, ctx);
        }
        else
            ResponseFormatters.FormatHexDump(resp, ctx.Output);
    }

    private async Task DoStatus(CoreApi api, CommandContext ctx)
    {
        var result = await api.StatusAsync();
        if (result.Success && result.Response != null)
            ResponseFormatters.FormatStatus(result.Response, ctx.Output, ctx.DetectedController);
        else
            ctx.Output.WriteError(result.Error ?? "Status failed");
    }

    // ─── Diag ───

    private async Task DoDiag(CoreApi api, string[] args, CommandContext ctx)
    {
        byte count = args.Length > 0 && byte.TryParse(args[0], out var c) ? c : (byte)0;
        var result = await api.DiagHistoryAsync(count);
        if (!result.Success) { ctx.Output.WriteError(result.Error ?? "Diag failed"); return; }

        var resp = result.Response!;
        if (resp.IsAck && resp.Payload.Length >= 2)
        {
            var sent = resp.GetU16LE(0);
            ctx.Output.WriteSuccess($"Received {sent} log entries");
        }
        else
        {
            ctx.Output.WriteSuccess("Diagnostic log retrieved");
        }
    }

    // ─── Verbose ───

    private void DoVerbose(string[] args, CommandContext ctx)
    {
        ctx.Verbose = args.Length > 0
            ? args[0].ToLowerInvariant() is "on" or "1" or "true"
            : !ctx.Verbose;

        if (ctx.Connection != null) ctx.Connection.Verbose = ctx.Verbose;
        ctx.Output.WriteInfo($"Verbose mode: {(ctx.Verbose ? "ON" : "OFF")}");
    }

    // ─── Controller detection ───

    private static void DetectController(Response resp, CommandContext ctx)
    {
        var info = InitReadyInfo.Parse(resp.Payload);
        if (info == null) return;
        ctx.DetectedController = BoardDetector.GetControllerName(info.BoardType) ?? info.DeviceName.ToLowerInvariant();
    }
}
