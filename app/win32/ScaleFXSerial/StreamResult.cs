namespace ScaleFX.Serial;

/// <summary>
/// Result from a stream receive operation (STREAM_BEGIN / DATA / END).
/// </summary>
public class StreamResult
{
    public byte[] Data { get; set; } = [];
    public ushort TotalSegs { get; set; }
    public uint TotalBytes { get; set; }
    public ushort CrcAll { get; set; }
}
