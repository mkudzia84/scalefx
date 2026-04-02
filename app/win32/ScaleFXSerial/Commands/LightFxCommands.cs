using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial.Commands;

/// <summary>
/// LightFX command builders. Mirrors CmdLfx* functions in Go CLI.
/// </summary>
public static class LightFxCommands
{
    public static byte[] LedSet(byte ch, byte brightness)
        => Packet.Build(PacketTypes.LightFx.LED_SET, [ch, brightness]);

    public static byte[] LedOff(byte ch)
        => Packet.Build(PacketTypes.LightFx.LED_OFF, [ch]);

    public static byte[] LedSeqClear(byte ch)
        => Packet.Build(PacketTypes.LightFx.LED_SEQ_CLEAR, [ch]);

    public static byte[] LedSeqAdd(byte ch, byte eventType, ushort param1, ushort param2,
        byte param3 = 0, byte param4 = 0)
    {
        var payload = new byte[8];
        payload[0] = ch;
        payload[1] = eventType;
        Endian.U16LE(param1).CopyTo(payload, 2);
        Endian.U16LE(param2).CopyTo(payload, 4);
        payload[6] = param3;
        payload[7] = param4;
        return Packet.Build(PacketTypes.LightFx.LED_SEQ_ADD, payload);
    }

    public static byte[] LedSeqStart(byte ch, ushort loopCount = 0)
    {
        var payload = new byte[3];
        payload[0] = ch;
        Endian.U16LE(loopCount).CopyTo(payload, 1);
        return Packet.Build(PacketTypes.LightFx.LED_SEQ_START, payload);
    }

    public static byte[] LedSeqStop(byte ch)
        => Packet.Build(PacketTypes.LightFx.LED_SEQ_STOP, [ch]);

    public static byte[] LedSeqRestart(byte ch)
        => Packet.Build(PacketTypes.LightFx.LED_SEQ_RESTART, [ch]);

    public static byte[] LedSeqStatus(byte ch)
        => Packet.Build(PacketTypes.LightFx.LED_SEQ_STATUS, [ch]);

    public static byte[] LedSeqQueue(byte ch)
        => Packet.Build(PacketTypes.LightFx.LED_SEQ_QUEUE, [ch]);

    public static byte[] LedStatus()
        => Packet.Build(PacketTypes.LightFx.LED_STATUS);

    public static byte[] MasterBrightness(byte brightness)
        => Packet.Build(PacketTypes.LightFx.LED_MASTER_BRIGHTNESS, [brightness]);

    public static byte[] LedReset(byte ch)
        => Packet.Build(PacketTypes.LightFx.LED_RESET, [ch]);

    public static byte[] LedEnable(byte ch, bool enabled)
        => Packet.Build(PacketTypes.LightFx.LED_ENABLE, [ch, (byte)(enabled ? 1 : 0)]);

    public static byte[] ServoSet(byte id, ushort pulse_us)
    {
        var payload = new byte[3];
        payload[0] = id;
        Endian.U16LE(pulse_us).CopyTo(payload, 1);
        return Packet.Build(PacketTypes.LightFx.SERVO_SET, payload);
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
        return Packet.Build(PacketTypes.LightFx.SERVO_SETTINGS, payload);
    }

    public static byte[] LandingLightBind(byte slot, byte servoId, byte ledCh,
        ushort deploy_us, ushort retract_us, byte brightness)
    {
        var payload = new byte[8];
        payload[0] = slot;
        payload[1] = servoId;
        payload[2] = ledCh;
        Endian.U16LE(deploy_us).CopyTo(payload, 3);
        Endian.U16LE(retract_us).CopyTo(payload, 5);
        payload[7] = brightness;
        return Packet.Build(PacketTypes.LightFx.LANDING_LIGHT_BIND, payload);
    }

    public static byte[] LandingLightUnbind(byte slot)
        => Packet.Build(PacketTypes.LightFx.LANDING_LIGHT_UNBIND, [slot]);

    public static byte[] LandingLightDeploy(byte slot)
        => Packet.Build(PacketTypes.LightFx.LANDING_LIGHT_DEPLOY, [slot]);

    public static byte[] LandingLightRetract(byte slot)
        => Packet.Build(PacketTypes.LightFx.LANDING_LIGHT_RETRACT, [slot]);
}
