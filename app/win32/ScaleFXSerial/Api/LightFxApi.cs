
using ScaleFX.Serial.Commands;

namespace ScaleFX.Serial.Api;

/// <summary>
/// LightFX API: LED control, sequences, servo, landing lights.
/// </summary>
public class LightFxApi : ApiClient
{
    public LightFxApi(ScaleFxConnection connection) : base(connection) { }

    // ─── LED ───

    public Task<ApiResult> LedSetAsync(byte ch, byte brightness, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LedSet(ch, brightness), ct);

    public Task<ApiResult> LedOffAsync(byte ch, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LedOff(ch), ct);

    /// <summary>Query all LED channel states.</summary>
    public Task<ApiResult> LedStatusAsync(CancellationToken ct = default) =>
        SendQueryAsync(LightFxCommands.LedStatus(), PacketTypes.LightFx.LED_STATUS_RESP, ct);

    // ─── Sequences ───

    public Task<ApiResult> SeqAddAsync(byte ch, byte eventType, ushort p1 = 0, ushort p2 = 0,
        byte p3 = 0, byte p4 = 0, CancellationToken ct = default, params byte[] extra) =>
        SendAckAsync(LightFxCommands.LedSeqAdd(ch, eventType, p1, p2, p3, p4, extra), ct);

    public Task<ApiResult> SeqClearAsync(byte ch, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LedSeqClear(ch), ct);

    public Task<ApiResult> SeqStartAsync(byte ch, ushort loops = 0, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LedSeqStart(ch, loops), ct);

    public Task<ApiResult> SeqStopAsync(byte ch, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LedSeqStop(ch), ct);

    public Task<ApiResult> SeqRestartAsync(byte ch, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LedSeqRestart(ch), ct);

    /// <summary>Query sequence state for a channel.</summary>
    public Task<ApiResult> SeqStatusAsync(byte ch, CancellationToken ct = default) =>
        SendQueryAsync(LightFxCommands.LedSeqStatus(ch), PacketTypes.LightFx.LED_SEQ_STATUS_RESP, ct);

    /// <summary>Query sequence queue for a channel.</summary>
    public Task<ApiResult> SeqQueueAsync(byte ch, CancellationToken ct = default) =>
        SendQueryAsync(LightFxCommands.LedSeqQueue(ch), PacketTypes.LightFx.LED_SEQ_QUEUE_RESP, ct);

    // ─── Master Brightness ───

    public Task<ApiResult> MasterBrightnessAsync(byte value, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.MasterBrightness(value), ct);

    // ─── Servo ───

    public Task<ApiResult> ServoSetAsync(byte id, ushort pulse_us, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.ServoSet(id, pulse_us), ct);

    public Task<ApiResult> ServoConfigAsync(byte id, ushort min, ushort max,
        ushort speed = 0, ushort accel = 0, ushort decel = 0, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.ServoSettings(id, min, max, speed, accel, decel), ct);

    // ─── Landing Lights ───

    public Task<ApiResult> LandingBindAsync(byte slot, byte servoId, byte ledCh,
        ushort deploy_us, ushort retract_us, byte brightness, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LandingLightBind(slot, servoId, ledCh, deploy_us, retract_us, brightness), ct);

    public Task<ApiResult> LandingUnbindAsync(byte slot, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LandingLightUnbind(slot), ct);

    public Task<ApiResult> LandingDeployAsync(byte slot, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LandingLightDeploy(slot), ct);

    public Task<ApiResult> LandingRetractAsync(byte slot, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LandingLightRetract(slot), ct);

    // ─── Reset / Enable / Disable ───

    public Task<ApiResult> ResetAsync(byte ch, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LedReset(ch), ct);

    public Task<ApiResult> EnableAsync(byte ch, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LedEnable(ch, true), ct);

    public Task<ApiResult> DisableAsync(byte ch, CancellationToken ct = default) =>
        SendAckAsync(LightFxCommands.LedEnable(ch, false), ct);
}
