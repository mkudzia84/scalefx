using ScaleFX.Serial.Commands;

namespace ScaleFX.Serial.Api;

/// <summary>
/// HubFX API: slave management, audio, engine, config, storage status, USB.
/// For file operations, use <see cref="FileApi"/>.
/// </summary>
public class HubFxApi : ApiClient
{
    public HubFxApi(ScaleFxConnection connection) : base(connection) { }

    // ─── Slaves ───

    /// <summary>List connected slaves. Response contains slave type+status array.</summary>
    public Task<ApiResult> SlaveListAsync(CancellationToken ct = default) =>
        SendQueryAsync(HubFxCommands.SlaveList(), PacketTypes.HubFx.SLAVE_LIST_RESP, ct);

    /// <summary>Initialize a slave by type (GUNFX=1, LIGHTFX=2, GEARCONTROL=3).</summary>
    public Task<ApiResult> SlaveInitAsync(byte slaveType, CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.SlaveInit(slaveType), ct);

    /// <summary>Query slave identity/info. Returns SLAVE_INFO_RESP.</summary>
    public Task<ApiResult> SlaveInfoAsync(byte slaveType, CancellationToken ct = default) =>
        SendQueryAsync(HubFxCommands.SlaveInfo(slaveType), PacketTypes.HubFx.SLAVE_INFO_RESP, ct);

    // ─── Audio ───

    public Task<ApiResult> AudioPlayAsync(byte ch, string path, byte volume = 100,
        byte output = 0x01, byte loopMode = 0, ushort loopCount = 0,
        CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.AudioPlay(ch, volume, output, loopMode, loopCount, path), ct);

    public Task<ApiResult> AudioStopAsync(byte ch = 0xFF, CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.AudioStop(ch), ct);

    public Task<ApiResult> AudioVolumeAsync(byte ch, byte volume,
        CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.AudioVolume(ch, volume), ct);

    public Task<ApiResult> AudioFadeAsync(byte ch, CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.AudioFade(ch), ct);

    public Task<ApiResult> AudioQueueAsync(byte ch, string path, byte volume = 100,
        ushort loopCount = 0, byte behavior = 0, CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.AudioQueue(ch, volume, loopCount, behavior, path), ct);

    public Task<ApiResult> AudioQueueClearAsync(byte ch = 0xFF, CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.AudioQueueClear(ch), ct);

    /// <summary>Query audio mixer status.</summary>
    public Task<ApiResult> AudioStatusAsync(CancellationToken ct = default) =>
        SendQueryAsync(HubFxCommands.AudioStatusReq(), PacketTypes.HubFx.AUDIO_STATUS_RESP, ct);

    /// <summary>Query codec status.</summary>
    public Task<ApiResult> CodecStatusAsync(CancellationToken ct = default) =>
        SendQueryAsync(HubFxCommands.CodecStatusReq(), PacketTypes.HubFx.CODEC_STATUS_RESP, ct);

    // ─── Engine ───

    public Task<ApiResult> EngineStartAsync(CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.EngineStart(), ct);

    public Task<ApiResult> EngineStopAsync(CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.EngineStop(), ct);

    /// <summary>Query engine state.</summary>
    public Task<ApiResult> EngineStatusAsync(CancellationToken ct = default) =>
        SendQueryAsync(HubFxCommands.EngineStatusReq(), PacketTypes.HubFx.ENGINE_STATUS_RESP, ct);

    // ─── Config ───

    public Task<ApiResult> ConfigReloadAsync(string path = "", CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.ConfigReload(path), ct);

    /// <summary>Query config status.</summary>
    public Task<ApiResult> ConfigStatusAsync(CancellationToken ct = default) =>
        SendQueryAsync(HubFxCommands.ConfigStatus(), PacketTypes.HubFx.CONFIG_STATUS_RESP, ct);

    public Task<ApiResult> ConfigSaveAsync(string path = "", CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.ConfigSave(path), ct);

    // ─── Storage ───

    public Task<ApiResult> SdInitAsync(CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.SdInit(), ct);

    /// <summary>Query SD card status.</summary>
    public Task<ApiResult> SdStatusAsync(CancellationToken ct = default) =>
        SendQueryAsync(HubFxCommands.SdStatusReq(), PacketTypes.HubFx.SD_STATUS_RESP, ct);

    /// <summary>Query flash status.</summary>
    public Task<ApiResult> FlashStatusAsync(CancellationToken ct = default) =>
        SendQueryAsync(HubFxCommands.FlashStatusReq(), PacketTypes.HubFx.FLASH_STATUS_REQ, ct);

    // ─── USB ───

    /// <summary>List USB devices.</summary>
    public Task<ApiResult> UsbDevicesAsync(CancellationToken ct = default) =>
        SendQueryAsync(HubFxCommands.UsbDevicesReq(), PacketTypes.HubFx.USB_DEVICES_RESP, ct);

    public Task<ApiResult> UsbResetAsync(CancellationToken ct = default) =>
        SendAckAsync(HubFxCommands.UsbResetBus(), ct);
}
