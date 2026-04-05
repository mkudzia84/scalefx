using System.Text;
using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial;

/// <summary>
/// Parsed INIT_READY / IDENTIFY payload.
/// Single shared parser used by BoardDetector, ResponseFormatters, and CoreHandler.
/// Wire format: [nameLen:u8][name][verLen:u8][ver][platLen:u8][plat][cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]
/// </summary>
public sealed class InitReadyInfo
{
    public string DeviceName { get; init; } = "";
    public string Version { get; init; } = "";
    public string Platform { get; init; } = "";
    public uint CpuMHz { get; init; }
    public uint FreeRam { get; init; }
    public uint BuildNumber { get; init; }
    public BoardType BoardType { get; init; }

    /// <summary>
    /// Parses an INIT_READY or IDENTIFY response payload.
    /// Returns null if the payload is too short or malformed.
    /// </summary>
    public static InitReadyInfo? Parse(byte[] payload)
    {
        if (payload.Length < 2) return null;

        try
        {
            int offset = 0;

            string ReadString()
            {
                if (offset >= payload.Length) return "";
                byte len = payload[offset++];
                if (len == 0 || offset + len > payload.Length) return "";
                var s = Encoding.UTF8.GetString(payload, offset, len);
                offset += len;
                return s;
            }

            string name = ReadString();
            string version = ReadString();
            string platform = ReadString();

            uint cpuMHz = 0, freeRam = 0, buildNum = 0;
            if (offset + 4 <= payload.Length) { cpuMHz = Endian.ReadU32LE(payload, offset); offset += 4; }
            if (offset + 4 <= payload.Length) { freeRam = Endian.ReadU32LE(payload, offset); offset += 4; }
            if (offset + 4 <= payload.Length) { buildNum = Endian.ReadU32LE(payload, offset); }

            // Determine board type from name
            string nameLower = name.ToLowerInvariant();
            BoardType boardType;
            if (nameLower.Contains("hubfx"))        boardType = BoardType.HubFX;
            else if (nameLower.Contains("gunfx"))    boardType = BoardType.GunFX;
            else if (nameLower.Contains("lightfx"))  boardType = BoardType.LightFX;
            else if (nameLower.Contains("gear"))     boardType = BoardType.GearControl;
            else boardType = BoardType.Unknown;

            return new InitReadyInfo
            {
                DeviceName = name,
                Version = version,
                Platform = platform,
                CpuMHz = cpuMHz,
                FreeRam = freeRam,
                BuildNumber = buildNum,
                BoardType = boardType
            };
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// Creates a BoardInfo from this parsed info plus a port name and optional connection.
    /// </summary>
    public BoardInfo ToBoardInfo(string portName, ScaleFxConnection? connection = null) => new()
    {
        PortName = portName,
        BoardType = BoardType,
        DeviceName = DeviceName,
        Version = Version,
        Platform = Platform,
        CpuMHz = CpuMHz,
        FreeRam = FreeRam,
        BuildNumber = BuildNumber,
        Connection = connection
    };
}
