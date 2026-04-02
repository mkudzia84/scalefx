namespace ScaleFX.Serial.Protocol;

/// <summary>
/// Consistent Overhead Byte Stuffing (COBS) encoder/decoder.
/// Removes 0x00 bytes from data for framing; 0x00 is the packet delimiter.
/// </summary>
public static class Cobs
{
    /// <summary>
    /// COBS-encodes the given data. The 0x00 delimiter is NOT appended.
    /// </summary>
    public static byte[] Encode(ReadOnlySpan<byte> data)
    {
        if (data.Length == 0)
            return [0x01];

        var output = new List<byte>(data.Length + data.Length / 254 + 2);
        output.Add(0); // placeholder for first code byte
        int codeIdx = 0;
        byte code = 1;

        foreach (var b in data)
        {
            if (b == 0)
            {
                output[codeIdx] = code;
                codeIdx = output.Count;
                output.Add(0); // placeholder for next code
                code = 1;
            }
            else
            {
                output.Add(b);
                code++;
                if (code == 0xFF)
                {
                    output[codeIdx] = code;
                    codeIdx = output.Count;
                    output.Add(0);
                    code = 1;
                }
            }
        }

        output[codeIdx] = code;
        return output.ToArray();
    }

    /// <summary>
    /// COBS-decodes the given data. Returns null if the data is invalid.
    /// </summary>
    public static byte[]? Decode(ReadOnlySpan<byte> data)
    {
        if (data.Length == 0)
            return null;

        var output = new List<byte>(data.Length);
        int idx = 0;

        while (idx < data.Length)
        {
            byte code = data[idx];
            if (code == 0)
                return null; // invalid: 0x00 in encoded data
            idx++;

            for (byte i = 1; i < code; i++)
            {
                if (idx >= data.Length)
                    return null; // truncated
                output.Add(data[idx]);
                idx++;
            }

            if (code < 0xFF && idx < data.Length)
                output.Add(0);
        }

        return output.ToArray();
    }
}
