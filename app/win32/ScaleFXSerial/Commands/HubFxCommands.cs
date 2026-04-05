using System.Text;
using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial.Commands;

/// <summary>
/// HubFX command builders. Mirrors CmdHub* functions in Go CLI.
/// </summary>
public static class HubFxCommands
{
    // ─── Slave Commands ───

    public static byte[] SlaveList()
        => Packet.Build(PacketTypes.HubFx.SLAVE_LIST);

    public static byte[] SlaveInit(byte slaveType)
        => Packet.Build(PacketTypes.HubFx.SLAVE_INIT, [slaveType]);

    public static byte[] SlaveInfo(byte slaveType)
        => Packet.Build(PacketTypes.HubFx.SLAVE_INFO, [slaveType]);

    // ─── Audio Commands ───

    public static byte[] AudioPlay(byte ch, byte volume, byte output, byte loopMode,
        ushort loopCount, string path)
    {
        var pathBytes = Encoding.UTF8.GetBytes(path);
        var payload = new byte[7 + pathBytes.Length];
        payload[0] = ch;
        payload[1] = volume;
        payload[2] = output;
        payload[3] = loopMode;
        Endian.U16LE(loopCount).CopyTo(payload, 4);
        payload[6] = (byte)pathBytes.Length;
        pathBytes.CopyTo(payload, 7);
        return Packet.Build(PacketTypes.HubFx.AUDIO_PLAY, payload);
    }

    public static byte[] AudioStop(byte ch = 0xFF)
        => Packet.Build(PacketTypes.HubFx.AUDIO_STOP, [ch]);

    public static byte[] AudioVolume(byte ch, byte volume)
        => Packet.Build(PacketTypes.HubFx.AUDIO_VOLUME, [ch, volume]);

    public static byte[] AudioFade(byte ch)
        => Packet.Build(PacketTypes.HubFx.AUDIO_FADE, [ch]);

    public static byte[] AudioQueue(byte ch, byte volume, ushort loopCount, byte behavior, string path)
    {
        var pathBytes = Encoding.UTF8.GetBytes(path);
        var payload = new byte[6 + pathBytes.Length];
        payload[0] = ch;
        payload[1] = volume;
        Endian.U16LE(loopCount).CopyTo(payload, 2);
        payload[4] = behavior;
        payload[5] = (byte)pathBytes.Length;
        pathBytes.CopyTo(payload, 6);
        return Packet.Build(PacketTypes.HubFx.AUDIO_QUEUE, payload);
    }

    public static byte[] AudioQueueClear(byte ch = 0xFF)
        => Packet.Build(PacketTypes.HubFx.AUDIO_QUEUE_CLEAR, [ch]);

    public static byte[] AudioStatusReq()
        => Packet.Build(PacketTypes.HubFx.AUDIO_STATUS_REQ);

    public static byte[] CodecStatusReq()
        => Packet.Build(PacketTypes.HubFx.CODEC_STATUS_REQ);

    // ─── Engine Commands ───

    public static byte[] EngineStart()
        => Packet.Build(PacketTypes.HubFx.ENGINE_START);

    public static byte[] EngineStop()
        => Packet.Build(PacketTypes.HubFx.ENGINE_STOP);

    public static byte[] EngineStatusReq()
        => Packet.Build(PacketTypes.HubFx.ENGINE_STATUS_REQ);

    // ─── Config Commands ───

    public static byte[] ConfigReload(string path = "")
    {
        if (string.IsNullOrEmpty(path))
            return Packet.Build(PacketTypes.HubFx.CONFIG_RELOAD);
        var pathBytes = Encoding.UTF8.GetBytes(path);
        var payload = new byte[1 + pathBytes.Length];
        payload[0] = (byte)pathBytes.Length;
        pathBytes.CopyTo(payload, 1);
        return Packet.Build(PacketTypes.HubFx.CONFIG_RELOAD, payload);
    }

    public static byte[] ConfigStatus()
        => Packet.Build(PacketTypes.HubFx.CONFIG_STATUS);

    public static byte[] ConfigSave(string path = "")
    {
        if (string.IsNullOrEmpty(path))
            return Packet.Build(PacketTypes.HubFx.CONFIG_SAVE);
        var pathBytes = Encoding.UTF8.GetBytes(path);
        var payload = new byte[1 + pathBytes.Length];
        payload[0] = (byte)pathBytes.Length;
        pathBytes.CopyTo(payload, 1);
        return Packet.Build(PacketTypes.HubFx.CONFIG_SAVE, payload);
    }

    // ─── Storage Commands ───

    public static byte[] SdInit(byte speed_mhz = 0)
        => Packet.Build(PacketTypes.HubFx.SD_INIT, [speed_mhz]);

    public static byte[] SdStatusReq()
        => Packet.Build(PacketTypes.HubFx.SD_STATUS_REQ);

    public static byte[] FlashStatusReq()
        => Packet.Build(PacketTypes.HubFx.FLASH_STATUS_REQ);

    // ─── File Commands ───

    private static byte[] FileCommand(byte packetType, string path, byte target)
    {
        var pathBytes = Encoding.UTF8.GetBytes(path);
        var payload = new byte[2 + pathBytes.Length];
        payload[0] = (byte)pathBytes.Length;
        pathBytes.CopyTo(payload, 1);
        payload[^1] = target;
        return Packet.Build(packetType, payload);
    }

    public static byte[] FileList(string path, byte target)
        => FileCommand(PacketTypes.HubFx.FILE_LIST, path, target);

    public static byte[] FileDelete(string path, byte target)
        => FileCommand(PacketTypes.HubFx.FILE_DELETE, path, target);

    public static byte[] FileMkdir(string path, byte target)
        => FileCommand(PacketTypes.HubFx.FILE_MKDIR, path, target);

    public static byte[] FileInfo(string path, byte target)
        => FileCommand(PacketTypes.HubFx.FILE_INFO, path, target);

    public static byte[] FileTree(string path, byte target)
        => FileCommand(PacketTypes.HubFx.FILE_TREE, path, target);

    public static byte[] FileDownload(string path, byte target)
        => FileCommand(PacketTypes.HubFx.FILE_DOWNLOAD, path, target);

    // ─── Upload Commands ───

    /// <summary>
    /// Begin a file upload. After ACK, send FileUploadData chunks, then FileUploadEnd.
    /// Payload: [size:u32LE][pathLen:u8][path:str][target:u8][mode:u8]
    /// </summary>
    /// <param name="path">Destination file path on device.</param>
    /// <param name="size">Total file size in bytes.</param>
    /// <param name="target">Storage target (0=SD, 1=Flash).</param>
    /// <param name="mode">Upload mode (0=sync, 3=stream).</param>
    public static byte[] FileUploadBegin(string path, uint size, byte target, byte mode = 0)
    {
        var pathBytes = Encoding.UTF8.GetBytes(path);
        var payload = new byte[4 + 1 + pathBytes.Length + 1 + 1];
        Endian.U32LE(size).CopyTo(payload, 0);
        payload[4] = (byte)pathBytes.Length;
        pathBytes.CopyTo(payload, 5);
        payload[5 + pathBytes.Length] = target;
        payload[6 + pathBytes.Length] = mode;
        return Packet.Build(PacketTypes.HubFx.FILE_UPLOAD_BEGIN, payload);
    }

    /// <summary>
    /// Send an upload data chunk with CRC-16 integrity.
    /// Payload: [seqNum:u16LE][crc16:u16LE][data]
    /// </summary>
    public static byte[] FileUploadData(ushort seq, byte[] data)
    {
        var crc = Crc.Crc16(data);
        var payload = new byte[4 + data.Length];
        Endian.U16LE(seq).CopyTo(payload, 0);
        Endian.U16LE(crc).CopyTo(payload, 2);
        data.CopyTo(payload, 4);
        return Packet.Build(PacketTypes.HubFx.FILE_UPLOAD_DATA, payload);
    }

    /// <summary>End a file upload. Server verifies total size and responds with MD5.</summary>
    public static byte[] FileUploadEnd()
        => Packet.Build(PacketTypes.HubFx.FILE_UPLOAD_END);

    /// <summary>Cancel an in-progress upload. Server deletes partial file.</summary>
    public static byte[] FileUploadCancel()
        => Packet.Build(PacketTypes.HubFx.FILE_UPLOAD_CANCEL);

    // ─── USB Commands ───

    public static byte[] UsbDevicesReq()
        => Packet.Build(PacketTypes.HubFx.USB_DEVICES_REQ);

    public static byte[] UsbResetBus()
        => Packet.Build(PacketTypes.HubFx.USB_RESET_BUS);

    // ─── Slave Route ───

    /// <summary>
    /// Wraps an inner packet for routing through a slave.
    /// </summary>
    public static byte[] SlaveRoute(byte routeType, byte[] innerPacket)
    {
        var parsed = Packet.Parse(innerPacket);
        if (parsed == null)
            return innerPacket;

        var routePayload = new byte[1 + parsed.Payload.Length];
        routePayload[0] = parsed.PacketType;
        parsed.Payload.CopyTo(routePayload, 1);
        return Packet.Build(routeType, routePayload);
    }
}
