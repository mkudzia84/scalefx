using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial.Commands;

/// <summary>
/// GunFX command builders. Mirrors CmdGfx* functions in Go CLI.
/// </summary>
public static class GunFxCommands
{
    public static byte[] TriggerOn(ushort rpm)
        => Packet.Build(PacketTypes.GunFx.TRIGGER_ON, Endian.U16LE(rpm));

    public static byte[] TriggerOff(ushort delay_ms = 0)
        => Packet.Build(PacketTypes.GunFx.TRIGGER_OFF, Endian.U16LE(delay_ms));

    public static byte[] ServoSet(byte id, ushort pulse_us)
    {
        var payload = new byte[3];
        payload[0] = id;
        Endian.U16LE(pulse_us).CopyTo(payload, 1);
        return Packet.Build(PacketTypes.GunFx.SERVO_SET, payload);
    }

    public static byte[] ServoSettings(byte id, ushort minPulse, ushort maxPulse,
        ushort speed = 4000, ushort accel = 8000, ushort decel = 8000)
    {
        var payload = new byte[11];
        payload[0] = id;
        Endian.U16LE(minPulse).CopyTo(payload, 1);
        Endian.U16LE(maxPulse).CopyTo(payload, 3);
        Endian.U16LE(speed).CopyTo(payload, 5);
        Endian.U16LE(accel).CopyTo(payload, 7);
        Endian.U16LE(decel).CopyTo(payload, 9);
        return Packet.Build(PacketTypes.GunFx.SERVO_SETTINGS, payload);
    }

    public static byte[] ServoRecoil(byte id, ushort jerk_us, ushort variance_us)
    {
        var payload = new byte[5];
        payload[0] = id;
        Endian.U16LE(jerk_us).CopyTo(payload, 1);
        Endian.U16LE(variance_us).CopyTo(payload, 3);
        return Packet.Build(PacketTypes.GunFx.SERVO_RECOIL, payload);
    }

    public static byte[] SmokeHeat(bool on)
        => Packet.Build(PacketTypes.GunFx.SMOKE_HEAT, [(byte)(on ? 1 : 0)]);

    public static byte[] SmokeSettings(bool pulsing, byte speed, byte high, byte low,
        ushort pulse_ms, ushort spindown_ms)
    {
        var payload = new byte[8];
        payload[0] = (byte)(pulsing ? 1 : 0);
        payload[1] = speed;
        payload[2] = high;
        payload[3] = low;
        Endian.U16LE(pulse_ms).CopyTo(payload, 4);
        Endian.U16LE(spindown_ms).CopyTo(payload, 6);
        return Packet.Build(PacketTypes.GunFx.SMOKE_SETTINGS, payload);
    }

    public static byte[] SmokeReset()
        => Packet.Build(PacketTypes.GunFx.SMOKE_RESET);

    public static byte[] SmokeCurrentLimit(byte target, ushort limit_mA)
    {
        var payload = new byte[3];
        payload[0] = target;
        Endian.U16LE(limit_mA).CopyTo(payload, 1);
        return Packet.Build(PacketTypes.GunFx.SMOKE_CURRENT_LIMIT, payload);
    }
}
