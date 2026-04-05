using System.Diagnostics;
using System.Security.Cryptography;
using ScaleFX.Serial.Commands;
using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial.Api;

/// <summary>
/// File operations API: list, tree, info, delete, mkdir, download, upload (sync/windowed), cancel.
/// Supports two upload modes:
///   - Sync (mode=0): COBS-framed chunks, ACK per chunk, CRC retry
///   - Windowed (mode=2): COBS-framed chunks, server-controlled flow via UPLOAD_PROGRESS
/// </summary>
public class FileApi : ApiClient
{
    private const int ChunkSize = 2044;      // MAX_PAYLOAD(2048) - 4 (seq+crc header)

    public FileApi(ScaleFxConnection connection) : base(connection) { }

    // ─── Directory operations ───

    /// <summary>List directory contents (streamed text).</summary>
    public Task<string?> ListAsync(byte target, string path = "/",
        int timeout_s = 30, CancellationToken ct = default) =>
        SendStreamAsync(HubFxCommands.FileList(path, target), timeout_s, ct);

    /// <summary>Directory tree (streamed text).</summary>
    public Task<string?> TreeAsync(byte target, string path = "/",
        int timeout_s = 30, CancellationToken ct = default) =>
        SendStreamAsync(HubFxCommands.FileTree(path, target), timeout_s, ct);

    /// <summary>Delete file or directory.</summary>
    public Task<ApiResult> DeleteAsync(byte target, string path,
        CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.FileDelete(path, target), ct);

    /// <summary>Create directory.</summary>
    public Task<ApiResult> MkdirAsync(byte target, string path,
        CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.FileMkdir(path, target), ct);

    /// <summary>Query file/dir info. Returns FILE_INFO_RESP.</summary>
    public Task<ApiResult> InfoAsync(byte target, string path,
        CancellationToken ct = default) =>
        SendQueryAsync(HubFxCommands.FileInfo(path, target), PacketTypes.HubFx.FILE_INFO_RESP, ct);

    // ─── Download ───

    /// <summary>Download file as raw bytes (streamed binary).</summary>
    public async Task<byte[]?> DownloadAsync(byte target, string path,
        int timeout_s = 60, Action<long, long>? progress = null,
        CancellationToken ct = default)
    {
        var result = await SendStreamBinaryAsync(
            HubFxCommands.FileDownload(path, target), timeout_s, progress, ct);
        return result?.Data;
    }

    /// <summary>Download file as UTF-8 text.</summary>
    public async Task<string?> CatAsync(byte target, string path,
        int timeout_s = 30, CancellationToken ct = default)
    {
        var data = await DownloadAsync(target, path, timeout_s, ct: ct);
        return data != null ? System.Text.Encoding.UTF8.GetString(data) : null;
    }

    // ─── Upload ───

    /// <summary>
    /// Upload file data to the controller.
    /// Supports sync (per-chunk ACK) and windowed (server-controlled flow) modes.
    /// </summary>
    public async Task<UploadResult> UploadAsync(byte target, string remotePath, byte[] fileData,
        UploadMode mode = UploadMode.Sync, int maxRetries = 3,
        Action<long, long>? progress = null, CancellationToken ct = default)
    {
        uint fileSize = (uint)fileData.Length;
        var sw = Stopwatch.StartNew();

        // ── Begin ──
        var beginResult = await SendAckAsync(
            HubFxCommands.FileUploadBegin(remotePath, fileSize, target, (byte)mode), ct);
        if (!beginResult.Success)
            return new UploadResult { Success = false, Error = beginResult.Error };

        using var md5 = MD5.Create();

        // ── Data phase ──
        string? dataError = mode switch
        {
            UploadMode.Sync => await UploadSyncAsync(fileData, md5, maxRetries, progress, ct),
            UploadMode.Window => await UploadWindowedAsync(fileData, md5, progress, ct),
            _ => $"Unsupported upload mode: {mode}"
        };

        if (dataError != null)
        {
            // Attempt to cancel the upload
            try { await SendAckAsync(HubFxCommands.FileUploadCancel(), ct); } catch { }
            sw.Stop();
            return new UploadResult
            {
                Success = false,
                Error = dataError,
                BytesTransferred = fileSize,
                Elapsed_s = sw.Elapsed.TotalSeconds
            };
        }

        // ── End + MD5 verification ──
        md5.TransformFinalBlock([], 0, 0);

        Response? endResp;
        try
        {
            endResp = await Connection.SendAndWaitAsync(HubFxCommands.FileUploadEnd(), ct);
        }
        catch (TimeoutException)
        {
            sw.Stop();
            return new UploadResult
            {
                Success = false,
                Error = "Timeout waiting for upload end response",
                BytesTransferred = fileSize,
                Elapsed_s = sw.Elapsed.TotalSeconds
            };
        }

        sw.Stop();

        string? remoteMd5 = null;
        string? localMd5 = md5.Hash != null
            ? Convert.ToHexString(md5.Hash).ToLowerInvariant() : null;
        ushort crcErrors = 0;

        if (endResp.IsAck && endResp.Payload.Length >= 16)
        {
            remoteMd5 = Convert.ToHexString(endResp.Payload.AsSpan(0, 16)).ToLowerInvariant();
            if (endResp.Payload.Length >= 18)
                crcErrors = Endian.ReadU16LE(endResp.Payload, 16);
        }

        return new UploadResult
        {
            Success = endResp.IsAck,
            Error = endResp.IsAck ? null : $"NACK: {endResp.ErrorMessage}",
            BytesTransferred = fileSize,
            Elapsed_s = sw.Elapsed.TotalSeconds,
            RemoteMd5 = remoteMd5,
            LocalMd5 = localMd5,
            CrcErrors = crcErrors
        };
    }

    /// <summary>Cancel an active upload.</summary>
    public Task<ApiResult> CancelUploadAsync(CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.FileUploadCancel(), ct);

    // ═══════════════════════════════════════════════════════════════
    // Upload mode implementations
    // ═══════════════════════════════════════════════════════════════

    /// <summary>Sync upload: COBS-framed chunks with per-chunk ACK and CRC retry.</summary>
    private async Task<string?> UploadSyncAsync(byte[] fileData, MD5 md5,
        int maxRetries, Action<long, long>? progress, CancellationToken ct)
    {
        int offset = 0;
        ushort seq = 0;
        int fileSize = fileData.Length;

        while (offset < fileSize)
        {
            int end = Math.Min(offset + ChunkSize, fileSize);
            var chunk = fileData[offset..end];
            md5.TransformBlock(chunk, 0, chunk.Length, null, 0);

            bool sent = false;
            for (int retry = 0; retry < maxRetries; retry++)
            {
                var r = await SendAckAsync(HubFxCommands.FileUploadData(seq, chunk), ct);
                if (r.Success) { sent = true; break; }

                // CRC error → retry
                if (r.Response?.Payload.Length > 0 && r.Response.Payload[0] == 0x02)
                    continue;

                // Other error → abort
                return $"Upload failed at segment {seq}: {r.Error}";
            }

            if (!sent)
                return $"Max retries ({maxRetries}) exceeded on segment {seq}";

            offset = end;
            seq++;
            progress?.Invoke(offset, fileSize);
        }

        return null; // success
    }

    /// <summary>
    /// Windowed upload: COBS-framed chunks with server-controlled flow via UPLOAD_PROGRESS.
    /// TODO: Implement windowed upload mode for C# client.
    /// </summary>
    private async Task<string?> UploadWindowedAsync(byte[] fileData, MD5 md5,
        Action<long, long>? progress, CancellationToken ct)
    {
        // Placeholder — windowed mode not yet implemented in C# client
        return await Task.FromResult("Windowed upload not yet implemented in C# client");
    }
}
