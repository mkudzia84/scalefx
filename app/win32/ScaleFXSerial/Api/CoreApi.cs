using ScaleFX.Serial.Commands;

namespace ScaleFX.Serial.Api;

/// <summary>
/// Core protocol API: init, identify, status, reboot, diagnostics.
/// </summary>
public class CoreApi : ApiClient
{
    public CoreApi(ScaleFxConnection connection) : base(connection) { }

    /// <summary>Send INIT. Returns INIT_READY (with board info) or ACK.</summary>
    public Task<ApiResult> InitAsync(CancellationToken ct = default) =>
        SendAsync(CoreCommands.Init(), ct);

    /// <summary>Send IDENTIFY. Returns IDENTIFY response or INIT_READY (legacy).</summary>
    public Task<ApiResult> IdentifyAsync(CancellationToken ct = default) =>
        SendAsync(CoreCommands.Identify(), ct);

    /// <summary>Shutdown controller.</summary>
    public Task<ApiResult> ShutdownAsync(CancellationToken ct = default) =>
        SendAckAsync(CoreCommands.Shutdown(), ct);

    /// <summary>Reboot controller.</summary>
    public Task<ApiResult> RebootAsync(CancellationToken ct = default) =>
        SendAckAsync(CoreCommands.Reboot(), ct);

    /// <summary>Enter BOOTSEL/DFU mode.</summary>
    public Task<ApiResult> BootselAsync(CancellationToken ct = default) =>
        SendAckAsync(CoreCommands.Bootsel(), ct);

    /// <summary>Request controller status.</summary>
    public Task<ApiResult> StatusAsync(CancellationToken ct = default) =>
        SendQueryAsync(CoreCommands.StatusReq(), PacketTypes.Core.STATUS, ct);

    /// <summary>Send keepalive ping.</summary>
    public Task<ApiResult> KeepaliveAsync(CancellationToken ct = default) =>
        SendAckAsync(CoreCommands.Keepalive(), ct);

    /// <summary>Scan I2C bus.</summary>
    public Task<ApiResult> I2CScanAsync(CancellationToken ct = default) =>
        SendQueryAsync(CoreCommands.I2CScan(), PacketTypes.Core.I2C_SCAN_RES, ct);

    /// <summary>
    /// Request diagnostic log. Returns ACK with [count:u16LE] payload.
    /// Actual log entries arrive asynchronously as LOG_MESSAGE packets.
    /// </summary>
    public Task<ApiResult> DiagHistoryAsync(byte count = 0, CancellationToken ct = default) =>
        SendAckAsync(CoreCommands.DiagHistory(count), ct);
}
