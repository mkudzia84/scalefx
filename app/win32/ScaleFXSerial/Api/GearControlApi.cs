using ScaleFX.Serial.Commands;

namespace ScaleFX.Serial.Api;

/// <summary>
/// GearControl API: deploy/retract, servo, gear/door/yaw config, calibration, battery.
/// </summary>
public class GearControlApi : ApiClient
{
    // GEAR_ALL action bytes
    private const byte ActionRetract = 0;
    private const byte ActionDeploy  = 1;
    private const byte ActionStop    = 2;

    public GearControlApi(ScaleFxConnection connection) : base(connection) { }

    // ─── Deploy / Retract / Stop ───

    public Task<ApiResult> DeployAsync(byte id, CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.Deploy(id), ct);

    public Task<ApiResult> RetractAsync(byte id, CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.Retract(id), ct);

    public Task<ApiResult> StopAsync(byte id, CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.Stop(id), ct);

    /// <summary>
    /// Deploy/retract/stop ALL gears. May return GEAR_SEQ_STATUS intermediate response.
    /// </summary>
    public Task<ApiResult> AllDeployAsync(CancellationToken ct = default) =>
        SendAsync(GearControlCommands.All(ActionDeploy), ct);

    public Task<ApiResult> AllRetractAsync(CancellationToken ct = default) =>
        SendAsync(GearControlCommands.All(ActionRetract), ct);

    public Task<ApiResult> AllStopAsync(CancellationToken ct = default) =>
        SendAsync(GearControlCommands.All(ActionStop), ct);

    // ─── Servo ───

    public Task<ApiResult> ServoSetAsync(byte id, ushort pulse_us, CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.ServoSet(id, pulse_us), ct);

    public Task<ApiResult> ServoConfigAsync(byte id, ushort min, ushort max,
        ushort speed = 4000, ushort accel = 8000, ushort decel = 8000,
        CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.ServoSettings(id, min, max, speed, accel, decel), ct);

    // ─── Config ───

    public Task<ApiResult> GearConfigAsync(byte id, byte flags, ushort stall_mA = 0,
        ushort timeout_ms = 0, CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.GearConfig(id, flags, stall_mA, timeout_ms), ct);

    public Task<ApiResult> DoorConfigAsync(byte id, ushort open0, ushort close0,
        ushort open1, ushort close1, CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.DoorConfig(id, open0, close0, open1, close1), ct);

    public Task<ApiResult> DoorModeAsync(byte id, byte pre, byte post, ushort delay_ms,
        CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.DoorMode(id, pre, post, delay_ms), ct);

    public Task<ApiResult> YawConfigAsync(byte id, ushort neutral, ushort min, ushort max,
        CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.YawConfig(id, neutral, min, max), ct);

    public Task<ApiResult> YawInputAsync(ushort position_us, CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.YawInput(position_us), ct);

    // ─── Calibration ───

    public Task<ApiResult> CalibrateAsync(byte id, byte timeout_s = 0,
        CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.Calibrate(id, timeout_s), ct);

    public Task<ApiResult> CalibrateCancelAsync(byte id, CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.CalibCancel(id), ct);

    // ─── Battery ───

    public Task<ApiResult> BatteryConfigAsync(bool enable, bool autoDeploy,
        CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.BatteryConfig(enable, autoDeploy), ct);

    // ─── Reset / Enable ───

    public Task<ApiResult> ResetAsync(byte id, CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.Reset(id), ct);

    public Task<ApiResult> EnableAsync(byte id, bool enabled, CancellationToken ct = default) =>
        SendAckAsync(GearControlCommands.Enable(id, enabled), ct);
}
