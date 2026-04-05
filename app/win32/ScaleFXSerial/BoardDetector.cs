using System.IO.Ports;
using System.Text;
using ScaleFX.Serial.Commands;
using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial;

/// <summary>
/// Identifies a controller board type.
/// </summary>
public enum BoardType
{
    Unknown,
    HubFX,
    GunFX,
    LightFX,
    GearControl
}

/// <summary>
/// Information about a connected slave controller (via HubFX).
/// Wire format per slave: [type:u8][connected:u8][ready:u8][nameLen:u8][name:str]
/// </summary>
public sealed class SlaveInfo
{
    public byte Type { get; init; }
    public bool Connected { get; init; }
    public bool Ready { get; init; }
    public string Name { get; init; } = "";
    public string TypeName => PacketTypes.SlaveType.GetName(Type);
    public bool IsConnected => Connected;
}

/// <summary>
/// Describes a detected ScaleFX board on a serial port.
/// </summary>
public sealed class BoardInfo
{
    public string PortName { get; init; } = "";
    public BoardType BoardType { get; init; }
    public string DeviceName { get; init; } = "";
    public string Version { get; init; } = "";
    public string Platform { get; init; } = "";
    public uint CpuMHz { get; init; }
    public uint FreeRam { get; init; }
    public uint BuildNumber { get; init; }
    public List<SlaveInfo> Slaves { get; init; } = [];
    public ScaleFxConnection? Connection { get; set; }

    public string DisplayName => BoardType switch
    {
        BoardType.HubFX => "HubFX",
        BoardType.GunFX => "GunFX",
        BoardType.LightFX => "LightFX",
        BoardType.GearControl => "GearControl",
        _ => DeviceName
    };

    public override string ToString()
    {
        var sb = new StringBuilder();
        sb.Append($"{DisplayName} v{Version} (build {BuildNumber}) on {PortName}");
        if (Slaves.Count > 0)
        {
            var connected = Slaves.Where(s => s.IsConnected).Select(s => s.TypeName);
            sb.Append($" [slaves: {string.Join(", ", connected)}]");
        }
        return sb.ToString();
    }
}

/// <summary>
/// Scans serial ports for ScaleFX boards. Supports one-shot detection
/// and periodic background scanning with events for connect/disconnect.
/// </summary>
public sealed class BoardDetector : IDisposable
{
    private readonly object _lock = new();
    private readonly Dictionary<string, BoardInfo> _boards = new();
    private CancellationTokenSource? _scanCts;
    private Task? _scanTask;
    private bool _disposed;

    /// <summary>Timeout for IDENTIFY probe on each port.</summary>
    public TimeSpan ProbeTimeout { get; set; } = TimeSpan.FromSeconds(3);

    /// <summary>Interval between periodic scans.</summary>
    public TimeSpan ScanInterval { get; set; } = TimeSpan.FromSeconds(5);

    /// <summary>Interval between slave list refreshes for HubFX.</summary>
    public TimeSpan SlaveRefreshInterval { get; set; } = TimeSpan.FromSeconds(10);

    /// <summary>Fired when a new board is detected.</summary>
    public event Action<BoardInfo>? BoardDetected;

    /// <summary>Fired when a board is no longer reachable.</summary>
    public event Action<BoardInfo>? BoardDisconnected;

    /// <summary>Fired when the slave list of a HubFX board changes.</summary>
    public event Action<BoardInfo>? SlaveListUpdated;

    /// <summary>Fired on each scan pass with the port currently being probed.</summary>
    public event Action<string>? ScanningPort;

    /// <summary>Current snapshot of detected boards.</summary>
    public IReadOnlyCollection<BoardInfo> DetectedBoards
    {
        get { lock (_lock) return _boards.Values.ToList(); }
    }

    // ─── One-Shot Scan ───

    /// <summary>
    /// Scans all serial ports once and returns any detected boards.
    /// Does not start background monitoring.
    /// </summary>
    public async Task<List<BoardInfo>> ScanOnceAsync(CancellationToken ct = default)
    {
        var results = new List<BoardInfo>();
        var ports = SerialPort.GetPortNames();

        foreach (var port in ports)
        {
            ct.ThrowIfCancellationRequested();
            ScanningPort?.Invoke(port);

            var board = await ProbePortAsync(port, ct);
            if (board != null)
                results.Add(board);
        }

        return results;
    }

    // ─── Background Scanning ───

    /// <summary>
    /// Starts periodic background scanning. Fires BoardDetected/BoardDisconnected events.
    /// </summary>
    public void StartScanning()
    {
        if (_scanTask != null) return;
        _scanCts = new CancellationTokenSource();
        _scanTask = ScanLoopAsync(_scanCts.Token);
    }

    /// <summary>
    /// Stops background scanning.
    /// </summary>
    public void StopScanning()
    {
        _scanCts?.Cancel();
        try { _scanTask?.Wait(2000); } catch { /* ignore */ }
        _scanTask = null;
        _scanCts?.Dispose();
        _scanCts = null;
    }

    private async Task ScanLoopAsync(CancellationToken ct)
    {
        var lastSlaveRefresh = DateTime.MinValue;

        while (!ct.IsCancellationRequested)
        {
            try
            {
                var ports = SerialPort.GetPortNames();
                var seenPorts = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

                foreach (var port in ports)
                {
                    if (ct.IsCancellationRequested) break;
                    seenPorts.Add(port);

                    bool alreadyKnown;
                    lock (_lock) alreadyKnown = _boards.ContainsKey(port);

                    if (!alreadyKnown)
                    {
                        ScanningPort?.Invoke(port);
                        var board = await ProbePortAsync(port, ct);
                        if (board != null)
                        {
                            lock (_lock) _boards[port] = board;
                            BoardDetected?.Invoke(board);
                        }
                    }
                }

                // Check for disconnected boards
                List<BoardInfo> disconnected;
                lock (_lock)
                {
                    disconnected = _boards
                        .Where(kv => !seenPorts.Contains(kv.Key) || !IsPortOpen(kv.Value))
                        .Select(kv => kv.Value)
                        .ToList();

                    foreach (var b in disconnected)
                        _boards.Remove(b.PortName);
                }

                foreach (var b in disconnected)
                {
                    b.Connection?.Close();
                    b.Connection = null;
                    BoardDisconnected?.Invoke(b);
                }

                // Periodic slave refresh for HubFX boards
                if (DateTime.UtcNow - lastSlaveRefresh > SlaveRefreshInterval)
                {
                    lastSlaveRefresh = DateTime.UtcNow;
                    await RefreshSlaveListsAsync(ct);
                }

                await Task.Delay(ScanInterval, ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch
            {
                // Scan errors are non-fatal, retry next cycle
                try { await Task.Delay(ScanInterval, ct); } catch { break; }
            }
        }
    }

    // ─── Port Probing ───

    /// <summary>
    /// Attempts to connect to a port and identify the board.
    /// Returns null if the port is not a ScaleFX device.
    /// </summary>
    private async Task<BoardInfo?> ProbePortAsync(string port, CancellationToken ct)
    {
        ScaleFxConnection? conn = null;
        try
        {
            // Skip ports that are already tracked
            lock (_lock)
            {
                if (_boards.ContainsKey(port)) return null;
            }

            conn = new ScaleFxConnection(port)
            {
                Timeout = ProbeTimeout,
                Verbose = false
            };
            conn.Connect();

            // Send IDENTIFY
            var identifyPacket = CoreCommands.Identify();
            var resp = await conn.SendAndWaitAsync(identifyPacket, ct);

            if (resp == null) { conn.Close(); return null; }

            // Accept IDENTIFY response or INIT_READY (legacy firmware)
            if (resp.PacketType != PacketTypes.Core.IDENTIFY &&
                resp.PacketType != PacketTypes.Core.INIT_READY)
            {
                conn.Close();
                return null;
            }

            var board = ParseIdentifyResponse(resp, port);
            if (board == null) { conn.Close(); return null; }

            board.Connection = conn;

            // If HubFX, also query slave list
            if (board.BoardType == BoardType.HubFX)
            {
                var slaves = await QuerySlaveListAsync(conn, ct);
                if (slaves != null)
                {
                    // Replace the readonly list with the actual slaves
                    board = new BoardInfo
                    {
                        PortName = board.PortName,
                        BoardType = board.BoardType,
                        DeviceName = board.DeviceName,
                        Version = board.Version,
                        Platform = board.Platform,
                        CpuMHz = board.CpuMHz,
                        FreeRam = board.FreeRam,
                        BuildNumber = board.BuildNumber,
                        Slaves = slaves,
                        Connection = conn
                    };
                }
            }

            return board;
        }
        catch
        {
            conn?.Close();
            return null;
        }
    }

    /// <summary>
    /// Parses IDENTIFY / INIT_READY payload into a BoardInfo.
    /// Delegates to <see cref="InitReadyInfo.Parse"/> for the shared parsing logic.
    /// </summary>
    internal static BoardInfo? ParseIdentifyResponse(Response resp, string port)
    {
        var info = InitReadyInfo.Parse(resp.Payload);
        return info?.ToBoardInfo(port);
    }

    /// <summary>
    /// Determine board type string for CommandContext (matches CoreHandler convention).
    /// </summary>
    public static string? GetControllerName(BoardType type) => type switch
    {
        BoardType.HubFX => "hubfx",
        BoardType.GunFX => "gunfx",
        BoardType.LightFX => "lightfx",
        BoardType.GearControl => "gearcontrol",
        _ => null
    };

    // ─── Slave List ───

    public static async Task<List<SlaveInfo>?> QuerySlaveListAsync(
        ScaleFxConnection conn, CancellationToken ct)
    {
        try
        {
            var resp = await conn.SendAndWaitAsync(HubFxCommands.SlaveList(), ct);
            if (resp?.PacketType != PacketTypes.HubFx.SLAVE_LIST_RESP) return null;
            return ParseSlaveList(resp);
        }
        catch
        {
            return null;
        }
    }

    public static List<SlaveInfo> ParseSlaveList(Response resp)
    {
        var slaves = new List<SlaveInfo>();
        var p = resp.Payload;
        if (p.Length < 1) return slaves;

        byte count = p[0];
        int offset = 1;
        for (int i = 0; i < count && offset + 4 <= p.Length; i++)
        {
            byte type = p[offset];
            bool connected = p[offset + 1] != 0;
            bool ready = p[offset + 2] != 0;
            byte nameLen = p[offset + 3];
            offset += 4;

            string name = "";
            if (nameLen > 0 && offset + nameLen <= p.Length)
            {
                name = Encoding.UTF8.GetString(p, offset, nameLen);
                offset += nameLen;
            }

            slaves.Add(new SlaveInfo
            {
                Type = type,
                Connected = connected,
                Ready = ready,
                Name = name
            });
        }

        return slaves;
    }

    private async Task RefreshSlaveListsAsync(CancellationToken ct)
    {
        List<BoardInfo> hubBoards;
        lock (_lock)
        {
            hubBoards = _boards.Values
                .Where(b => b.BoardType == BoardType.HubFX && b.Connection?.IsConnected == true)
                .ToList();
        }

        foreach (var board in hubBoards)
        {
            if (ct.IsCancellationRequested) break;
            try
            {
                var slaves = await QuerySlaveListAsync(board.Connection!, ct);
                if (slaves != null)
                {
                    // Check for changes
                    bool changed = slaves.Count != board.Slaves.Count;
                    if (!changed)
                    {
                        for (int i = 0; i < slaves.Count; i++)
                        {
                            if (slaves[i].Type != board.Slaves[i].Type ||
                                slaves[i].Connected != board.Slaves[i].Connected ||
                                slaves[i].Ready != board.Slaves[i].Ready)
                            {
                                changed = true;
                                break;
                            }
                        }
                    }

                    if (changed)
                    {
                        board.Slaves.Clear();
                        board.Slaves.AddRange(slaves);
                        SlaveListUpdated?.Invoke(board);
                    }
                }
            }
            catch
            {
                // Slave refresh failure is non-fatal
            }
        }
    }

    // ─── Helpers ───

    private static bool IsPortOpen(BoardInfo board)
    {
        return board.Connection?.IsConnected == true;
    }

    /// <summary>
    /// Disconnects and disposes all tracked board connections.
    /// </summary>
    public void DisconnectAll()
    {
        List<BoardInfo> boards;
        lock (_lock)
        {
            boards = _boards.Values.ToList();
            _boards.Clear();
        }

        foreach (var b in boards)
        {
            b.Connection?.Close();
            b.Connection = null;
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        StopScanning();
        DisconnectAll();
    }
}
