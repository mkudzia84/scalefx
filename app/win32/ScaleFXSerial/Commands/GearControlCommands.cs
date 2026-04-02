using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial.Commands;

/// <summary>
/// GearControl command builders. Mirrors CmdGc* functions in Go CLI.
/// </summary>
public static class GearControlCommands
{
    public static byte[] Deploy(byte gearId)
        => Packet.Build(PacketTypes.GearControl.GEAR_DEPLOY, [gearId]);

    public static byte[] Retract(byte gearId)
        => Packet.Build(PacketTypes.GearControl.GEAR_RETRACT, [gearId]);

    public static byte[] Stop(byte gearId)
        => Packet.Build(PacketTypes.GearControl.GEAR_STOP, [gearId]);

    public static byte[] All(byte action)
        => Packet.Build(PacketTypes.GearControl.GEAR_ALL, [action]);

    public static byte[] ServoSet(byte id, ushort pulse_us)
    {
        var payload = new byte[3];
        payload[0] = id;
        Endian.U16LE(pulse_us).CopyTo(payload, 1);
        return Packet.Build(PacketTypes.GearControl.SERVO_SET, payload);
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
        return Packet.Build(PacketTypes.GearControl.SERVO_SETTINGS, payload);
    }

    public static byte[] GearConfig(byte gearId, byte flags, ushort stall_mA, ushort timeout_ms)
    {
        var payload = new byte[6];
        payload[0] = gearId;
        payload[1] = flags;
        Endian.U16LE(stall_mA).CopyTo(payload, 2);
        Endian.U16LE(timeout_ms).CopyTo(payload, 4);
        return Packet.Build(PacketTypes.GearControl.GEAR_CONFIG, payload);
    }

    public static byte[] DoorConfig(byte gearId, ushort open0, ushort close0, ushort open1, ushort close1)
    {
        var payload = new byte[9];
        payload[0] = gearId;
        Endian.U16LE(open0).CopyTo(payload, 1);
        Endian.U16LE(close0).CopyTo(payload, 3);
        Endian.U16LE(open1).CopyTo(payload, 5);
        Endian.U16LE(close1).CopyTo(payload, 7);
        return Packet.Build(PacketTypes.GearControl.DOOR_CONFIG, payload);
    }

    public static byte[] YawConfig(byte gearId, ushort neutral, ushort min, ushort max)
    {
        var payload = new byte[7];
        payload[0] = gearId;
        Endian.U16LE(neutral).CopyTo(payload, 1);
        Endian.U16LE(min).CopyTo(payload, 3);
        Endian.U16LE(max).CopyTo(payload, 5);
        return Packet.Build(PacketTypes.GearControl.YAW_CONFIG, payload);
    }

    public static byte[] YawInput(ushort position_us)
        => Packet.Build(PacketTypes.GearControl.YAW_INPUT, Endian.U16LE(position_us));

    public static byte[] Calibrate(byte gearId, byte timeout_s = 0)
    {
        if (timeout_s > 0)
            return Packet.Build(PacketTypes.GearControl.GEAR_CALIBRATE, [gearId, timeout_s]);
        return Packet.Build(PacketTypes.GearControl.GEAR_CALIBRATE, [gearId]);
    }

    public static byte[] CalibCancel(byte gearId)
        => Packet.Build(PacketTypes.GearControl.GEAR_CALIB_CANCEL, [gearId]);

    public static byte[] BatteryConfig(bool enabled, bool autoDeploy)
        => Packet.Build(PacketTypes.GearControl.BATTERY_CONFIG,
            [(byte)(enabled ? 1 : 0), (byte)(autoDeploy ? 1 : 0)]);

    public static byte[] DoorMode(byte gearId, byte preDeploy, byte postDeploy, ushort delay_ms)
    {
        var payload = new byte[5];
        payload[0] = gearId;
        payload[1] = preDeploy;
        payload[2] = postDeploy;
        Endian.U16LE(delay_ms).CopyTo(payload, 3);
        return Packet.Build(PacketTypes.GearControl.DOOR_MODE, payload);
    }

    public static byte[] Reset(byte gearId)
        => Packet.Build(PacketTypes.GearControl.GEAR_RESET, [gearId]);

    public static byte[] Enable(byte gearId, bool enabled)
        => Packet.Build(PacketTypes.GearControl.GEAR_ENABLE, [gearId, (byte)(enabled ? 1 : 0)]);
}
