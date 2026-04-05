using ScaleFX.Serial.Commands;

namespace ScaleFX.Serial.Api;

/// <summary>
/// GunFX API: trigger control, servo, smoke system.
/// </summary>
public class GunFxApi : ApiClient
{
    public GunFxApi(ScaleFxConnection connection) : base(connection) { }

    // ─── Trigger ───

    public Task<ApiResult> TriggerOnAsync(ushort rpm, CancellationToken ct = default) =>
        SendAckAsync(GunFxCommands.TriggerOn(rpm), ct);

    public Task<ApiResult> TriggerOffAsync(ushort delay_ms = 0, CancellationToken ct = default) =>
        SendAckAsync(GunFxCommands.TriggerOff(delay_ms), ct);

    // ─── Servo ───

    public Task<ApiResult> ServoSetAsync(byte id, ushort pulse_us, CancellationToken ct = default) =>
        SendAckAsync(GunFxCommands.ServoSet(id, pulse_us), ct);

    public Task<ApiResult> ServoConfigAsync(byte id, ushort min, ushort max,
        ushort speed = 0, ushort accel = 0, ushort decel = 0, CancellationToken ct = default) =>
        SendAckAsync(GunFxCommands.ServoSettings(id, min, max, speed, accel, decel), ct);

    public Task<ApiResult> ServoRecoilAsync(byte id, ushort jerk_us, byte variance,
        CancellationToken ct = default) =>
        SendAckAsync(GunFxCommands.ServoRecoil(id, jerk_us, variance), ct);

    // ─── Smoke ───

    public Task<ApiResult> SmokeHeatAsync(bool on, CancellationToken ct = default) =>
        SendAckAsync(GunFxCommands.SmokeHeat(on), ct);

    public Task<ApiResult> SmokeConfigAsync(bool pulsing, byte speed, byte high, byte low,
        ushort pulse_ms, ushort spindown_ms, CancellationToken ct = default) =>
        SendAckAsync(GunFxCommands.SmokeSettings(pulsing, speed, high, low, pulse_ms, spindown_ms), ct);

    public Task<ApiResult> SmokeResetAsync(CancellationToken ct = default) =>
        SendAckAsync(GunFxCommands.SmokeReset(), ct);

    public Task<ApiResult> SmokeLimitAsync(byte target_mA, ushort limit_mA,
        CancellationToken ct = default) =>
        SendAckAsync(GunFxCommands.SmokeCurrentLimit(target_mA, limit_mA), ct);
}
