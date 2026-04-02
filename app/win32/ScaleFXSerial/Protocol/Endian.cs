namespace ScaleFX.Serial.Protocol;

/// <summary>
/// Little-endian encoding helpers for building packet payloads.
/// </summary>
public static class Endian
{
    /// <summary>Encodes a uint16 as 2 bytes, little-endian.</summary>
    public static byte[] U16LE(ushort value) =>
        [(byte)(value & 0xFF), (byte)((value >> 8) & 0xFF)];

    /// <summary>Encodes a uint32 as 4 bytes, little-endian.</summary>
    public static byte[] U32LE(uint value) =>
        [(byte)(value & 0xFF), (byte)((value >> 8) & 0xFF),
         (byte)((value >> 16) & 0xFF), (byte)((value >> 24) & 0xFF)];

    /// <summary>Reads a uint16 from a byte span at the given offset, little-endian.</summary>
    public static ushort ReadU16LE(ReadOnlySpan<byte> data, int offset) =>
        (ushort)(data[offset] | (data[offset + 1] << 8));

    /// <summary>Reads a int16 from a byte span at the given offset, little-endian.</summary>
    public static short ReadI16LE(ReadOnlySpan<byte> data, int offset) =>
        (short)ReadU16LE(data, offset);

    /// <summary>Reads a uint32 from a byte span at the given offset, little-endian.</summary>
    public static uint ReadU32LE(ReadOnlySpan<byte> data, int offset) =>
        (uint)(data[offset] | (data[offset + 1] << 8) |
               (data[offset + 2] << 16) | (data[offset + 3] << 24));
}
