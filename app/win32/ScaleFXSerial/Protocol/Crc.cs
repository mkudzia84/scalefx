namespace ScaleFX.Serial.Protocol;

/// <summary>
/// CRC-8 checksum (polynomial 0x07) used for packet integrity.
/// CRC-16/CCITT (polynomial 0x1021, init 0xFFFF) used for stream verification.
/// </summary>
public static class Crc
{
    /// <summary>
    /// CRC-8 polynomial used by ScaleFX protocol.
    /// </summary>
    private const byte Poly8 = 0x07;

    /// <summary>
    /// CRC-16/CCITT polynomial.
    /// </summary>
    private const ushort Poly16 = 0x1021;

    /// <summary>
    /// Pre-computed CRC-8 lookup table.
    /// </summary>
    private static readonly byte[] Table8 = BuildTable8();

    /// <summary>
    /// Calculates CRC-8 checksum over the given data.
    /// </summary>
    public static byte Crc8(ReadOnlySpan<byte> data)
    {
        byte crc = 0;
        foreach (var b in data)
            crc = Table8[crc ^ b];
        return crc;
    }

    /// <summary>
    /// Calculates CRC-16/CCITT checksum over the given data.
    /// </summary>
    public static ushort Crc16(ReadOnlySpan<byte> data)
    {
        ushort crc = 0xFFFF;
        foreach (var b in data)
        {
            crc ^= (ushort)(b << 8);
            for (int i = 0; i < 8; i++)
            {
                if ((crc & 0x8000) != 0)
                    crc = (ushort)((crc << 1) ^ Poly16);
                else
                    crc <<= 1;
            }
        }
        return crc;
    }

    private static byte[] BuildTable8()
    {
        var table = new byte[256];
        for (int i = 0; i < 256; i++)
        {
            byte crc = (byte)i;
            for (int j = 0; j < 8; j++)
            {
                if ((crc & 0x80) != 0)
                    crc = (byte)((crc << 1) ^ Poly8);
                else
                    crc <<= 1;
            }
            table[i] = crc;
        }
        return table;
    }
}
