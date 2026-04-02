using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial.Commands;

/// <summary>
/// Core protocol command builders.
/// </summary>
public static class CoreCommands
{
    public static byte[] Init()        => Packet.Build(PacketTypes.Core.INIT);
    public static byte[] Shutdown()    => Packet.Build(PacketTypes.Core.SHUTDOWN);
    public static byte[] Keepalive()   => Packet.Build(PacketTypes.Core.KEEPALIVE);
    public static byte[] Reboot()      => Packet.Build(PacketTypes.Core.REBOOT);
    public static byte[] Bootsel()     => Packet.Build(PacketTypes.Core.BOOTSEL);
    public static byte[] StatusReq()   => Packet.Build(PacketTypes.Core.STATUS_REQ);
    public static byte[] Identify()    => Packet.Build(PacketTypes.Core.IDENTIFY);
    public static byte[] I2CScan()     => Packet.Build(PacketTypes.Core.I2C_SCAN);

    public static byte[] DiagHistory(byte count = 0)
    {
        if (count == 0)
            return Packet.Build(PacketTypes.Core.DIAG_HISTORY);
        return Packet.Build(PacketTypes.Core.DIAG_HISTORY, [count]);
    }
}
