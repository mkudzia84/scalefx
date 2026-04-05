using ScaleFX.Serial.Api;
using ScaleFX.Serial.Commands;
using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial.Console.Handlers;

/// <summary>
/// HubFX commands: slaves, audio, engine, config, storage, USB.
/// </summary>
public class HubFxHandler : ICommandHandler
{
    public string GroupName => "HubFX";

    private static readonly List<CommandInfo> _commands =
    [
        // Slaves
        new("slaves",         "slaves",                                          "List connected slaves"),
        new("slave.init",     "slave.init <gunfx|lightfx|gearcontrol>",          "Initialize slave"),
        new("slave.info",     "slave.info <gunfx|lightfx|gearcontrol>",          "Query slave identity"),
        // Audio
        new("audio.play",     "audio.play <ch> <path> [vol] [ch1|ch2] [loop N]", "Play audio file"),
        new("audio.stop",     "audio.stop [ch|all]",                              "Stop playback"),
        new("audio.volume",   "audio.volume <ch|master> <vol>",                  "Set volume (0-100)"),
        new("audio.fade",     "audio.fade <ch>",                                 "Fade out channel"),
        new("audio.queue",    "audio.queue <ch> <path> [vol] [loop N]",          "Queue audio file"),
        new("audio.clear",    "audio.clear [ch|all]",                            "Clear play queue"),
        new("audio.status",   "audio.status",                                    "Audio mixer status"),
        new("codec.status",   "codec.status",                                    "Codec status"),
        // Engine
        new("engine.start",   "engine.start",                                    "Start engine sound"),
        new("engine.stop",    "engine.stop",                                     "Stop engine sound"),
        new("engine.status",  "engine.status",                                   "Engine state"),
        // Config
        new("config.reload",  "config.reload [path]",                            "Reload config from SD"),
        new("config.status",  "config.status",                                   "Config status"),
        new("config.save",    "config.save [path]",                              "Save config to SD"),
        // Storage
        new("sd.init",        "sd.init",                                         "Initialize SD card"),
        new("sd.status",      "sd.status",                                       "SD card status"),
        new("flash.status",   "flash.status",                                    "Flash status"),
        // Files
        new("file.list",      "file.list <sd|flash> [path]",                     "List directory"),
        new("file.delete",    "file.delete <sd|flash> <path>",                   "Delete file/dir"),
        new("file.mkdir",     "file.mkdir <sd|flash> <path>",                    "Create directory"),
        new("file.info",      "file.info <sd|flash> <path>",                     "File/dir info"),
        new("file.tree",      "file.tree <sd|flash> [path]",                     "Directory tree"),
        new("file.cat",       "file.cat <sd|flash> <path>",                      "Display file contents"),
        new("file.download",  "file.download <sd|flash> <remote> <local>",        "Download file"),
        new("file.upload",    "file.upload <sd|flash> <local> <remote> [--window]", "Upload file"),
        new("file.cancel",    "file.cancel",                                      "Cancel active upload"),
        // USB
        new("usb.devices",    "usb.devices",                                     "List USB devices"),
        new("usb.reset",      "usb.reset",                                       "Reset USB bus"),
    ];

    public IReadOnlyList<CommandInfo> Commands => _commands;

    public bool IsRelevant(string? detectedController) =>
        detectedController is null or "hubfx";

    public async Task<bool> TryExecuteAsync(string cmd, string[] args, CommandContext ctx)
    {
        switch (cmd)
        {
            // Slaves
            case "slaves":       return await DoSlaves(ctx);
            case "slave.init":   return await DoSlaveInit(args, ctx);
            case "slave.info":   return await DoSlaveInfo(args, ctx);
            // Audio
            case "audio.play":   return await DoAudioPlay(args, ctx);
            case "audio.stop":   return await DoAudioStop(args, ctx);
            case "audio.volume": return await DoAudioVolume(args, ctx);
            case "audio.fade":   return await DoAudioFade(args, ctx);
            case "audio.queue":  return await DoAudioQueue(args, ctx);
            case "audio.clear":  return await DoAudioClear(args, ctx);
            case "audio.status":
                if (!ctx.RequireConnection()) return true;
                return Query(ctx, await Hub(ctx).AudioStatusAsync(), ResponseFormatters.FormatAudioStatus);
            case "codec.status":
                if (!ctx.RequireConnection()) return true;
                return Query(ctx, await Hub(ctx).CodecStatusAsync(), ResponseFormatters.FormatCodecStatus);
            // Engine
            case "engine.start":
                if (!ctx.RequireConnection()) return true;
                return Ack(ctx, await Hub(ctx).EngineStartAsync(), "Engine starting");
            case "engine.stop":
                if (!ctx.RequireConnection()) return true;
                return Ack(ctx, await Hub(ctx).EngineStopAsync(), "Engine stopping");
            case "engine.status":
                if (!ctx.RequireConnection()) return true;
                return Query(ctx, await Hub(ctx).EngineStatusAsync(), ResponseFormatters.FormatEngineStatus);
            // Config
            case "config.reload": return await DoConfigReload(args, ctx);
            case "config.status":
                if (!ctx.RequireConnection()) return true;
                return Query(ctx, await Hub(ctx).ConfigStatusAsync(), ResponseFormatters.FormatConfigStatus);
            case "config.save":  return await DoConfigSave(args, ctx);
            // Storage
            case "sd.init":
                if (!ctx.RequireConnection()) return true;
                return Ack(ctx, await Hub(ctx).SdInitAsync(), "SD card initialized");
            case "sd.status":
                if (!ctx.RequireConnection()) return true;
                return Query(ctx, await Hub(ctx).SdStatusAsync(), ResponseFormatters.FormatSdStatus);
            case "flash.status":
                if (!ctx.RequireConnection()) return true;
                return Query(ctx, await Hub(ctx).FlashStatusAsync(), ResponseFormatters.FormatFlashStatus);
            // Files
            case "file.list":    return await DoFileStream(args, ctx, "file.list", (f, t, p) => f.ListAsync(t, p));
            case "file.tree":    return await DoFileStream(args, ctx, "file.tree", (f, t, p) => f.TreeAsync(t, p));
            case "file.delete":  return await DoFileAck(args, ctx, "file.delete", (f, t, p) => f.DeleteAsync(t, p), "Deleted");
            case "file.mkdir":   return await DoFileAck(args, ctx, "file.mkdir", (f, t, p) => f.MkdirAsync(t, p), "Created");
            case "file.info":    return await DoFileInfo(args, ctx);
            case "file.cat":     return await DoFileCat(args, ctx);
            case "file.download": return await DoFileDownload(args, ctx);
            case "file.upload":  return await DoFileUpload(args, ctx);
            case "file.cancel":
                if (!ctx.RequireConnection()) return true;
                return Ack(ctx, await File(ctx).CancelUploadAsync(), "Upload cancelled");
            // USB
            case "usb.devices":
                if (!ctx.RequireConnection()) return true;
                return Query(ctx, await Hub(ctx).UsbDevicesAsync(), ResponseFormatters.FormatUsbDevices);
            case "usb.reset":
                if (!ctx.RequireConnection()) return true;
                return Ack(ctx, await Hub(ctx).UsbResetAsync(), "USB bus reset");

            default: return false;
        }
    }

    // ─── API factories ───

    private static HubFxApi Hub(CommandContext ctx) => new(ctx.Connection!);
    private static FileApi File(CommandContext ctx) => new(ctx.Connection!);

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

    private static byte ParseSlaveType(string name) => name.ToLowerInvariant() switch
    {
        "gunfx"       => PacketTypes.SlaveType.GUNFX,
        "lightfx"     => PacketTypes.SlaveType.LIGHTFX,
        "gearcontrol" => PacketTypes.SlaveType.GEARCONTROL,
        _             => 0
    };

    private static bool ParseTarget(string arg, out byte target)
    {
        target = arg.ToLowerInvariant() switch
        {
            "sd"    => PacketTypes.Storage.TARGET_SD,
            "flash" => PacketTypes.Storage.TARGET_FLASH,
            _       => 0xFF
        };
        return target != 0xFF;
    }

    // ─── Slaves ───

    private async Task<bool> DoSlaves(CommandContext ctx)
    {
        if (!ctx.RequireConnection()) return true;
        return Query(ctx, await Hub(ctx).SlaveListAsync(), ResponseFormatters.FormatSlaveList);
    }

    private async Task<bool> DoSlaveInit(string[] args, CommandContext ctx)
    {
        if (args.Length < 1) { ctx.Output.WriteError("Usage: slave.init <gunfx|lightfx|gearcontrol>"); return true; }
        var t = ParseSlaveType(args[0]);
        if (t == 0) { ctx.Output.WriteError($"Unknown slave type: {args[0]}"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await Hub(ctx).SlaveInitAsync(t),
            $"Slave {PacketTypes.SlaveType.GetName(t)} initialized");
    }

    private async Task<bool> DoSlaveInfo(string[] args, CommandContext ctx)
    {
        if (args.Length < 1) { ctx.Output.WriteError("Usage: slave.info <gunfx|lightfx|gearcontrol>"); return true; }
        var t = ParseSlaveType(args[0]);
        if (t == 0) { ctx.Output.WriteError($"Unknown slave type: {args[0]}"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Query(ctx, await Hub(ctx).SlaveInfoAsync(t), ResponseFormatters.FormatSlaveInfo);
    }

    // ─── Audio ───

    private async Task<bool> DoAudioPlay(string[] args, CommandContext ctx)
    {
        if (args.Length < 2)
        {
            ctx.Output.WriteError("Usage: audio.play <ch> <path> [vol] [ch1|ch2] [loop [N|inf]]");
            return true;
        }
        if (!byte.TryParse(args[0], out var ch)) { ctx.Output.WriteError("Invalid channel"); return true; }
        string path = args[1];
        byte vol = 100, output = PacketTypes.Audio.OUTPUT_CH1;
        byte loopMode = PacketTypes.Audio.LOOP_NONE;
        ushort loopCount = 0;

        int i = 2;
        if (i < args.Length && byte.TryParse(args[i], out var v)) { vol = v; i++; }
        if (i < args.Length && args[i].ToLowerInvariant() is "ch1" or "ch2")
        {
            output = args[i].ToLowerInvariant() == "ch2"
                ? PacketTypes.Audio.OUTPUT_CH2 : PacketTypes.Audio.OUTPUT_CH1;
            i++;
        }
        if (i < args.Length && args[i].ToLowerInvariant() == "loop")
        {
            i++;
            if (i < args.Length && args[i].ToLowerInvariant() == "inf")
            { loopMode = PacketTypes.Audio.LOOP_INFINITE; i++; }
            else if (i < args.Length && ushort.TryParse(args[i], out var lc))
            { loopMode = PacketTypes.Audio.LOOP_FINITE; loopCount = lc; i++; }
            else
            { loopMode = PacketTypes.Audio.LOOP_INFINITE; }
        }

        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await Hub(ctx).AudioPlayAsync(ch, path, vol, output, loopMode, loopCount),
            $"Playing '{path}' on ch {ch} (vol={vol})");
    }

    private async Task<bool> DoAudioStop(string[] args, CommandContext ctx)
    {
        byte ch = PacketTypes.Audio.CH_ALL;
        if (args.Length > 0 && args[0].ToLowerInvariant() != "all")
        {
            if (!byte.TryParse(args[0], out ch)) { ctx.Output.WriteError("Usage: audio.stop [ch|all]"); return true; }
        }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await Hub(ctx).AudioStopAsync(ch),
            ch == PacketTypes.Audio.CH_ALL ? "All audio stopped" : $"Ch {ch} stopped");
    }

    private async Task<bool> DoAudioVolume(string[] args, CommandContext ctx)
    {
        if (args.Length < 2) { ctx.Output.WriteError("Usage: audio.volume <ch|master> <vol>"); return true; }
        if (!byte.TryParse(args[1], out var vol)) { ctx.Output.WriteError("Invalid volume"); return true; }
        byte ch = args[0].ToLowerInvariant() == "master" ? PacketTypes.Audio.CH_ALL : byte.Parse(args[0]);
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await Hub(ctx).AudioVolumeAsync(ch, vol),
            ch == PacketTypes.Audio.CH_ALL ? $"Master volume → {vol}" : $"Ch {ch} volume → {vol}");
    }

    private async Task<bool> DoAudioFade(string[] args, CommandContext ctx)
    {
        if (args.Length < 1 || !byte.TryParse(args[0], out var ch))
        { ctx.Output.WriteError("Usage: audio.fade <ch>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await Hub(ctx).AudioFadeAsync(ch), $"Ch {ch} fading out");
    }

    private async Task<bool> DoAudioQueue(string[] args, CommandContext ctx)
    {
        if (args.Length < 2)
        { ctx.Output.WriteError("Usage: audio.queue <ch> <path> [vol] [loop N]"); return true; }
        if (!byte.TryParse(args[0], out var ch)) { ctx.Output.WriteError("Invalid channel"); return true; }
        string path = args[1];
        byte vol = 100;
        ushort loopCount = 0;
        byte behavior = PacketTypes.Audio.QUEUE_FINISH_LOOP;

        int i = 2;
        if (i < args.Length && byte.TryParse(args[i], out var v)) { vol = v; i++; }
        if (i < args.Length && args[i].ToLowerInvariant() == "loop" && i + 1 < args.Length)
        {
            ushort.TryParse(args[i + 1], out loopCount);
            i += 2;
        }

        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await Hub(ctx).AudioQueueAsync(ch, path, vol, loopCount, behavior),
            $"Queued '{path}' on ch {ch}");
    }

    private async Task<bool> DoAudioClear(string[] args, CommandContext ctx)
    {
        byte ch = PacketTypes.Audio.CH_ALL;
        if (args.Length > 0 && args[0].ToLowerInvariant() != "all")
            byte.TryParse(args[0], out ch);
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await Hub(ctx).AudioQueueClearAsync(ch), "Queue cleared");
    }

    // ─── Config ───

    private async Task<bool> DoConfigReload(string[] args, CommandContext ctx)
    {
        string path = args.Length > 0 ? args[0] : "";
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await Hub(ctx).ConfigReloadAsync(path), "Config reloaded");
    }

    private async Task<bool> DoConfigSave(string[] args, CommandContext ctx)
    {
        string path = args.Length > 0 ? args[0] : "";
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await Hub(ctx).ConfigSaveAsync(path), "Config saved");
    }

    // ─── File operations ───

    private async Task<bool> DoFileStream(string[] args, CommandContext ctx, string usage,
        Func<FileApi, byte, string, Task<string?>> action)
    {
        if (args.Length < 1 || !ParseTarget(args[0], out var target))
        { ctx.Output.WriteError($"Usage: {usage} <sd|flash> [path]"); return true; }
        if (!ctx.RequireConnection()) return true;
        string path = args.Length > 1 ? args[1] : "/";
        var result = await action(File(ctx), target, path);
        if (result != null)
        {
            if (result.Length == 0)
                ctx.Output.WriteWarning("(empty)");
            else
                ctx.Output.WriteLine(result);
        }
        return true;
    }

    private async Task<bool> DoFileAck(string[] args, CommandContext ctx, string usage,
        Func<FileApi, byte, string, Task<ApiResult>> action, string msg)
    {
        if (args.Length < 2 || !ParseTarget(args[0], out var target))
        { ctx.Output.WriteError($"Usage: {usage} <sd|flash> <path>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Ack(ctx, await action(File(ctx), target, args[1]), $"{msg}: {args[1]}");
    }

    private async Task<bool> DoFileInfo(string[] args, CommandContext ctx)
    {
        if (args.Length < 2 || !ParseTarget(args[0], out var target))
        { ctx.Output.WriteError("Usage: file.info <sd|flash> <path>"); return true; }
        if (!ctx.RequireConnection()) return true;
        return Query(ctx, await File(ctx).InfoAsync(target, args[1]), ResponseFormatters.FormatFileInfo);
    }

    // ─── File cat (stream download + print) ───

    private async Task<bool> DoFileCat(string[] args, CommandContext ctx)
    {
        if (args.Length < 2 || !ParseTarget(args[0], out var target))
        { ctx.Output.WriteError("Usage: file.cat <sd|flash> <path>"); return true; }
        if (!ctx.RequireConnection()) return true;

        ctx.Output.WriteInfo($"Reading {args[1]} ...");
        var text = await File(ctx).CatAsync(target, args[1]);
        if (text != null)
        {
            ctx.Output.WriteLine("");
            ctx.Output.WriteLine(text);
            ctx.Output.WriteInfo($"({System.Text.Encoding.UTF8.GetByteCount(text)} bytes)");
        }
        return true;
    }

    // ─── File download (stream to local file) ───

    private async Task<bool> DoFileDownload(string[] args, CommandContext ctx)
    {
        if (args.Length < 3 || !ParseTarget(args[0], out var target))
        { ctx.Output.WriteError("Usage: file.download <sd|flash> <remote> <local>"); return true; }
        if (!ctx.RequireConnection()) return true;

        string remotePath = args[1], localPath = args[2];
        ctx.Output.WriteInfo($"Downloading {remotePath} ...");

        var data = await File(ctx).DownloadAsync(target, remotePath, timeout_s: 60,
            progress: (received, total) =>
            {
                if (total > 0)
                    ctx.Output.Write($"\r  {received}/{total} bytes ({received * 100 / total}%)  ");
            });

        if (data == null) return true;

        try
        {
            var dir = Path.GetDirectoryName(Path.GetFullPath(localPath));
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            await System.IO.File.WriteAllBytesAsync(localPath, data);
            ctx.Output.WriteSuccess($"Downloaded {data.Length} bytes → {localPath}");
        }
        catch (Exception ex)
        {
            ctx.Output.WriteError($"Failed to write local file: {ex.Message}");
        }
        return true;
    }

    // ─── File upload (delegates to FileApi for sync/windowed modes) ───

    private async Task<bool> DoFileUpload(string[] args, CommandContext ctx)
    {
        if (args.Length < 3 || !ParseTarget(args[0], out var target))
        { ctx.Output.WriteError("Usage: file.upload <sd|flash> <local> <remote> [--window]"); return true; }
        if (!ctx.RequireConnection()) return true;

        string localPath = args[1], remotePath = args[2];
        bool windowed = args.Any(a => a == "--window");
        var mode = windowed ? UploadMode.Window : UploadMode.Sync;

        byte[] fileData;
        try { fileData = await System.IO.File.ReadAllBytesAsync(localPath); }
        catch (Exception ex)
        { ctx.Output.WriteError($"Cannot read local file: {ex.Message}"); return true; }

        ctx.Output.WriteInfo($"Uploading {localPath} ({fileData.Length} bytes) → {remotePath} [{mode.ToString().ToLower()}]");

        var result = await File(ctx).UploadAsync(target, remotePath, fileData, mode,
            progress: (sent, total) =>
            {
                int pct = (int)(sent * 100L / total);
                ctx.Output.Write($"\r  {sent}/{total} bytes ({pct}%)  ");
            });

        if (result.Success)
        {
            ctx.Output.WriteSuccess(
                $"Uploaded {result.BytesTransferred} bytes in {result.Elapsed_s:F1}s ({result.Speed_KBs:F1} KB/s)");

            if (result.RemoteMd5 != null && result.LocalMd5 != null)
            {
                if (result.Md5Match)
                    ctx.Output.WriteSuccess($"MD5 verified: {result.RemoteMd5}");
                else
                    ctx.Output.WriteError($"MD5 MISMATCH! local={result.LocalMd5} remote={result.RemoteMd5}");
            }

            if (result.CrcErrors > 0)
                ctx.Output.WriteWarning($"Server reported {result.CrcErrors} CRC errors during transfer");
        }
        else
        {
            ctx.Output.WriteError(result.Error ?? "Upload failed");
        }
        return true;
    }
}
