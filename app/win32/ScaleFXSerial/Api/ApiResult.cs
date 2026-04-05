namespace ScaleFX.Serial.Api;

/// <summary>
/// Result from an API operation. Wraps success/failure, error details, and the raw response.
/// </summary>
public class ApiResult
{
    public bool Success { get; init; }
    public string? Error { get; init; }
    public byte ErrorCode { get; init; }
    public Response? Response { get; init; }

    public static ApiResult Ok(Response? resp = null) =>
        new() { Success = true, Response = resp };

    public static ApiResult Fail(string error) =>
        new() { Success = false, Error = error };

    public static ApiResult Fail(Response resp) =>
        new()
        {
            Success = false,
            Error = resp.IsNack ? $"NACK: {resp.ErrorMessage}" : $"Unexpected: {PacketTypes.GetName(resp.PacketType)}",
            ErrorCode = resp.ErrorCode,
            Response = resp
        };

    public static ApiResult Timeout() =>
        new() { Success = false, Error = "Timeout waiting for response" };
}

/// <summary>
/// Result from a file upload operation.
/// </summary>
public class UploadResult
{
    public bool Success { get; init; }
    public string? Error { get; init; }
    public long BytesTransferred { get; init; }
    public double Elapsed_s { get; init; }
    public double Speed_KBs => Elapsed_s > 0 ? BytesTransferred / Elapsed_s / 1024.0 : 0;
    public string? RemoteMd5 { get; init; }
    public string? LocalMd5 { get; init; }
    public bool Md5Match => RemoteMd5 != null && LocalMd5 != null && RemoteMd5 == LocalMd5;
    public ushort CrcErrors { get; init; }
}

/// <summary>
/// Upload transfer mode.
/// </summary>
public enum UploadMode : byte
{
    /// <summary>COBS-framed chunks, ACK per chunk, CRC retry on error.</summary>
    Sync = 0,

    /// <summary>COBS packets, server-controlled flow via UPLOAD_PROGRESS.</summary>
    Window = 2
}
