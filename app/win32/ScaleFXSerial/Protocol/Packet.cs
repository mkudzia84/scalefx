using System.Buffers.Binary;

namespace ScaleFX.Serial.Protocol;

/// <summary>
/// Packet builder and parser for the ScaleFX binary protocol.
/// Packet structure (pre-COBS): [type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]
/// Wire format: COBS-encoded packet + 0x00 delimiter.
/// </summary>
public static class Packet
{
    /// <summary>Maximum payload size in bytes.</summary>
    public const int MaxPayload = 512;

    /// <summary>Header size: type(1) + tag(1) + length(2).</summary>
    public const int HeaderSize = 4;

    /// <summary>
    /// Builds a complete COBS-encoded packet with 0x00 delimiter.
    /// </summary>
    /// <param name="packetType">Packet type byte.</param>
    /// <param name="payload">Payload bytes (may be empty/null).</param>
    /// <param name="tag">Correlation tag (0 = async/broadcast).</param>
    /// <returns>Wire-ready bytes (COBS-encoded + 0x00 delimiter).</returns>
    public static byte[] Build(byte packetType, ReadOnlySpan<byte> payload, byte tag = 0)
    {
        int len = payload.Length;
        var raw = new byte[HeaderSize + len + 1]; // +1 for CRC

        raw[0] = packetType;
        raw[1] = tag;
        raw[2] = (byte)(len & 0xFF);
        raw[3] = (byte)((len >> 8) & 0xFF);

        payload.CopyTo(raw.AsSpan(HeaderSize));
        raw[^1] = Crc.Crc8(raw.AsSpan(0, raw.Length - 1));

        var encoded = Cobs.Encode(raw);
        var wire = new byte[encoded.Length + 1];
        encoded.CopyTo(wire, 0);
        wire[^1] = 0x00; // delimiter
        return wire;
    }

    /// <summary>
    /// Builds a packet with no payload.
    /// </summary>
    public static byte[] Build(byte packetType, byte tag = 0)
        => Build(packetType, ReadOnlySpan<byte>.Empty, tag);

    /// <summary>
    /// Parses a COBS-encoded packet (with or without trailing 0x00 delimiter).
    /// Returns null if the packet is invalid (bad COBS, wrong length, CRC mismatch).
    /// </summary>
    public static ParsedPacket? Parse(ReadOnlySpan<byte> data)
    {
        // Strip delimiter if present
        if (data.Length > 0 && data[^1] == 0x00)
            data = data[..^1];

        var decoded = Cobs.Decode(data);
        if (decoded == null || decoded.Length < HeaderSize + 1) // header + CRC
            return null;

        byte packetType = decoded[0];
        byte tag = decoded[1];
        int length = decoded[2] | (decoded[3] << 8);

        if (decoded.Length != HeaderSize + length + 1) // length mismatch
            return null;

        // CRC check
        byte receivedCrc = decoded[^1];
        byte expectedCrc = Crc.Crc8(decoded.AsSpan(0, decoded.Length - 1));
        if (receivedCrc != expectedCrc)
            return null;

        var payload = new byte[length];
        Array.Copy(decoded, HeaderSize, payload, 0, length);

        return new ParsedPacket(packetType, tag, payload);
    }

    /// <summary>
    /// Re-encodes a packet with a different correlation tag.
    /// Returns the original data unchanged if parsing fails.
    /// </summary>
    public static byte[] InjectTag(ReadOnlySpan<byte> data, byte newTag)
    {
        var parsed = Parse(data);
        if (parsed == null)
            return data.ToArray();
        return Build(parsed.PacketType, parsed.Payload, newTag);
    }
}

/// <summary>
/// A successfully parsed ScaleFX packet.
/// </summary>
public sealed class ParsedPacket
{
    public byte PacketType { get; }
    public byte Tag { get; }
    public byte[] Payload { get; }

    public ParsedPacket(byte packetType, byte tag, byte[] payload)
    {
        PacketType = packetType;
        Tag = tag;
        Payload = payload;
    }

    public int PayloadLength => Payload.Length;

    // ─── Convenience Properties ───

    public bool IsAck => PacketType == PacketTypes.Core.ACK;
    public bool IsNack => PacketType == PacketTypes.Core.NACK;
    public bool IsInitReady => PacketType == PacketTypes.Core.INIT_READY;
    public bool IsIdentify => PacketType == PacketTypes.Core.IDENTIFY;

    /// <summary>
    /// Returns the error code from a NACK payload, or 0 if not a NACK.
    /// </summary>
    public byte ErrorCode => IsNack && Payload.Length > 0 ? Payload[0] : (byte)0;

    /// <summary>
    /// Returns the error message from a NACK payload.
    /// </summary>
    public string ErrorMessage
    {
        get
        {
            if (!IsNack || Payload.Length == 0)
                return string.Empty;

            var name = ErrorCodes.GetName(Payload[0]);
            if (Payload.Length > 1)
            {
                var msg = System.Text.Encoding.UTF8.GetString(Payload, 1, Payload.Length - 1);
                return $"{name}: {msg}";
            }
            return name;
        }
    }

    // ─── Payload Helpers ───

    public byte GetU8(int offset) => Payload[offset];

    public ushort GetU16LE(int offset) =>
        (ushort)(Payload[offset] | (Payload[offset + 1] << 8));

    public uint GetU32LE(int offset) =>
        (uint)(Payload[offset] | (Payload[offset + 1] << 8) |
               (Payload[offset + 2] << 16) | (Payload[offset + 3] << 24));
}
