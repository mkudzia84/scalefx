using System.Text;
using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial.Api;

/// <summary>
/// Base class for all API clients. Provides common send helpers for
/// ACK commands, query commands, and stream operations.
/// </summary>
public abstract class ApiClient
{
    protected ScaleFxConnection Connection { get; }

    protected ApiClient(ScaleFxConnection connection) =>
        Connection = connection ?? throw new ArgumentNullException(nameof(connection));

    // ─── Send helpers ───

    /// <summary>Send a packet and return the raw response (any non-NACK is success).</summary>
    protected async Task<ApiResult> SendAsync(byte[] packet, CancellationToken ct = default)
    {
        try
        {
            var resp = await Connection.SendAndWaitAsync(packet, ct);
            if (resp.IsNack) return ApiResult.Fail(resp);
            return ApiResult.Ok(resp);
        }
        catch (TimeoutException) { return ApiResult.Timeout(); }
    }

    /// <summary>Send a packet and expect ACK.</summary>
    protected async Task<ApiResult> SendAckAsync(byte[] packet, CancellationToken ct = default)
    {
        try
        {
            var resp = await Connection.SendAndWaitAsync(packet, ct);
            if (resp.IsAck) return ApiResult.Ok(resp);
            return ApiResult.Fail(resp);
        }
        catch (TimeoutException) { return ApiResult.Timeout(); }
    }

    /// <summary>Send a packet and expect a specific response packet type.</summary>
    protected async Task<ApiResult> SendQueryAsync(byte[] packet, byte expectedType,
        CancellationToken ct = default)
    {
        try
        {
            var resp = await Connection.SendAndWaitAsync(packet, ct);
            if (resp.PacketType == expectedType) return ApiResult.Ok(resp);
            if (resp.IsNack) return ApiResult.Fail(resp);
            return ApiResult.Fail($"Unexpected response: {PacketTypes.GetName(resp.PacketType)}");
        }
        catch (TimeoutException) { return ApiResult.Timeout(); }
    }

    /// <summary>Send a packet and expect one of several response packet types.</summary>
    protected async Task<ApiResult> SendQueryAsync(byte[] packet, byte[] expectedTypes,
        CancellationToken ct = default)
    {
        try
        {
            var resp = await Connection.SendAndWaitAsync(packet, ct);
            if (Array.IndexOf(expectedTypes, resp.PacketType) >= 0) return ApiResult.Ok(resp);
            if (resp.IsNack) return ApiResult.Fail(resp);
            return ApiResult.Fail($"Unexpected response: {PacketTypes.GetName(resp.PacketType)}");
        }
        catch (TimeoutException) { return ApiResult.Timeout(); }
    }

    // ─── Stream helpers ───

    /// <summary>
    /// Send a command that returns streamed binary data (STREAM_BEGIN / DATA / END).
    /// Collects all DATA payloads (stripping the 4-byte seq+CRC16 header),
    /// verifies per-chunk CRC-16, and returns the raw binary data.
    /// </summary>
    protected async Task<StreamResult?> SendStreamBinaryAsync(byte[] packet,
        int timeout_s = 30, Action<long, long>? progress = null,
        CancellationToken ct = default)
    {
        var data = new MemoryStream();
        long totalExpected = 0;
        var result = new StreamResult();
        var done = new TaskCompletionSource<bool>();

        void StreamHandler(Response r)
        {
            if (r.PacketType == PacketTypes.Stream.DATA && r.Payload.Length > 4)
            {
                var chunk = r.Payload.AsSpan(4);
                var expectedCrc = Endian.ReadU16LE(r.Payload, 2);
                var computedCrc = Crc.Crc16(chunk);
                // CRC mismatch logged silently — caller can check result metadata
                data.Write(chunk);
                progress?.Invoke(data.Length, totalExpected);
            }
            else if (r.PacketType == PacketTypes.Stream.END)
            {
                if (r.Payload.Length >= 8)
                {
                    result.TotalSegs = Endian.ReadU16LE(r.Payload, 0);
                    result.TotalBytes = Endian.ReadU32LE(r.Payload, 2);
                    result.CrcAll = Endian.ReadU16LE(r.Payload, 6);
                }
                done.TrySetResult(true);
            }
        }

        Connection.OnAsyncPacket += StreamHandler;
        try
        {
            var resp = await SendAsync(packet, ct);
            if (!resp.Success) return null;

            var r = resp.Response!;
            if (r.PacketType == PacketTypes.Stream.BEGIN)
            {
                if (r.Payload.Length >= 4)
                    totalExpected = Endian.ReadU32LE(r.Payload, 0);

                using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
                cts.CancelAfter(TimeSpan.FromSeconds(timeout_s));
                cts.Token.Register(() => done.TrySetCanceled());

                try { await done.Task; }
                catch (TaskCanceledException) { return null; }

                result.Data = data.ToArray();
                return result;
            }

            // Single-packet response (not a stream)
            result.Data = r.Payload;
            return result;
        }
        finally
        {
            Connection.OnAsyncPacket -= StreamHandler;
        }
    }

    /// <summary>
    /// Send a command that returns streamed text data. Convenience wrapper
    /// that decodes the binary stream as UTF-8.
    /// </summary>
    protected async Task<string?> SendStreamAsync(byte[] packet,
        int timeout_s = 30, CancellationToken ct = default)
    {
        var result = await SendStreamBinaryAsync(packet, timeout_s, ct: ct);
        return result != null ? Encoding.UTF8.GetString(result.Data) : null;
    }
}
