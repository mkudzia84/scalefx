using System.Text;
using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial.Console;

/// <summary>
/// Execution context shared by all command handlers. Holds the serial
/// connection, output interface, and session state.
/// </summary>
public class CommandContext
{
    public ScaleFxConnection? Connection { get; set; }
    public IConsoleOutput Output { get; }
    public string? DetectedController { get; set; }
    public bool Verbose { get; set; }
    public bool IsConnected => Connection?.IsConnected == true;

    /// <summary>Fired when an async (unsolicited) packet arrives.</summary>
    public event Action<Response>? OnAsyncPacket;

    // Temporary per-command stream/async interceptors
    private readonly List<Action<Response>> _tempHandlers = new();

    public CommandContext(IConsoleOutput output) => Output = output;

    /// <summary>Called by the connection's async callback (reader thread).</summary>
    public void HandleAsyncPacket(Response resp)
    {
        foreach (var h in _tempHandlers.ToList()) h(resp);
        OnAsyncPacket?.Invoke(resp);
    }

    // ─── Require helpers ───

    public bool RequireConnection()
    {
        if (IsConnected) return true;
        Output.WriteError("Not connected. Use 'connect <port>' first.");
        return false;
    }

    // ─── Send helpers ───

    /// <summary>Send a packet and return the first tag-matched response.</summary>
    public async Task<Response?> SendAsync(byte[] packet)
    {
        if (!RequireConnection()) return null;
        try
        {
            return await Connection!.SendExpectAckAsync(packet);
        }
        catch (TimeoutException)
        {
            Output.WriteError("Timeout waiting for response");
            return null;
        }
        catch (Exception ex)
        {
            Output.WriteError($"Send failed: {ex.Message}");
            return null;
        }
    }

    /// <summary>Send a packet expecting ACK. Returns true on ACK.</summary>
    public async Task<bool> SendAckAsync(byte[] packet, string successMsg = "")
    {
        var resp = await SendAsync(packet);
        if (resp == null) return false;
        if (resp.IsAck)
        {
            Output.WriteSuccess(string.IsNullOrEmpty(successMsg) ? "OK" : successMsg);
            return true;
        }
        if (resp.IsNack)
        {
            Output.WriteError($"NACK: {resp.ErrorMessage}");
            return false;
        }
        Output.WriteWarning($"Unexpected response: {PacketTypes.GetName(resp.PacketType)}");
        return false;
    }

    /// <summary>
    /// Send a command that returns streamed data (STREAM_BEGIN / DATA / END).
    /// Collects all DATA payloads (stripping the 4-byte seq+CRC16 header from each chunk),
    /// verifies per-chunk CRC-16, and returns the raw binary data.
    /// Returns null on error or timeout.
    /// </summary>
    public async Task<StreamResult?> SendStreamBinaryAsync(byte[] packet, int timeout_s = 30,
        Action<long, long>? progress = null)
    {
        if (!RequireConnection()) return null;

        var data = new MemoryStream();
        long totalExpected = 0;
        var result = new StreamResult();
        var done = new TaskCompletionSource<bool>();

        void StreamHandler(Response r)
        {
            if (r.PacketType == PacketTypes.Stream.DATA && r.Payload.Length > 4)
            {
                // Strip [seq:u16LE][crc16:u16LE] header, keep only data
                var chunk = r.Payload.AsSpan(4);
                var expectedCrc = Endian.ReadU16LE(r.Payload, 2);
                var computedCrc = Crc.Crc16(chunk);
                if (computedCrc != expectedCrc && Verbose)
                    Output.WriteWarning($"CRC mismatch on segment {Endian.ReadU16LE(r.Payload, 0)}");
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

        _tempHandlers.Add(StreamHandler);
        try
        {
            var resp = await SendAsync(packet);
            if (resp == null) return null;

            if (resp.PacketType == PacketTypes.Stream.BEGIN)
            {
                if (resp.Payload.Length >= 4)
                    totalExpected = Endian.ReadU32LE(resp.Payload, 0);

                using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(timeout_s));
                cts.Token.Register(() => done.TrySetCanceled());
                try { await done.Task; }
                catch (TaskCanceledException) { Output.WriteError("Stream timeout"); return null; }

                result.Data = data.ToArray();
                return result;
            }

            if (resp.IsNack) { Output.WriteError($"NACK: {resp.ErrorMessage}"); return null; }

            // Single-packet response (not a stream)
            result.Data = resp.Payload;
            return result;
        }
        finally
        {
            _tempHandlers.Remove(StreamHandler);
        }
    }

    /// <summary>
    /// Send a command that returns streamed text data (STREAM_BEGIN / DATA / END).
    /// Convenience wrapper that decodes the binary stream as UTF-8.
    /// </summary>
    public async Task<string?> SendStreamAsync(byte[] packet, int timeout_s = 30)
    {
        var result = await SendStreamBinaryAsync(packet, timeout_s);
        return result != null ? Encoding.UTF8.GetString(result.Data) : null;
    }

    /// <summary>
    /// Send a packet and wait for an ACK, returning the full response
    /// (including any ACK payload, e.g. MD5 hash from upload end).
    /// Returns (true, response) on ACK, (false, response) on NACK/error.
    /// </summary>
    public async Task<(bool Success, Response? Response)> SendAckWithResponseAsync(byte[] packet)
    {
        var resp = await SendAsync(packet);
        if (resp == null) return (false, null);
        if (resp.IsAck) return (true, resp);
        if (resp.IsNack) { Output.WriteError($"NACK: {resp.ErrorMessage}"); return (false, resp); }
        Output.WriteWarning($"Unexpected response: {PacketTypes.GetName(resp.PacketType)}");
        return (false, resp);
    }
}
