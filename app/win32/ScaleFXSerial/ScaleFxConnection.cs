using System.Collections.Concurrent;
using System.IO.Ports;
using ScaleFX.Serial.Protocol;

namespace ScaleFX.Serial;

/// <summary>
/// Parsed response from a ScaleFX controller.
/// </summary>
public sealed class Response
{
    public byte PacketType { get; init; }
    public byte Tag { get; init; }
    public byte[] Payload { get; init; } = [];
    public byte[] Raw { get; init; } = [];

    public bool IsAck => PacketType == PacketTypes.Core.ACK;
    public bool IsNack => PacketType == PacketTypes.Core.NACK;
    public bool IsInitReady => PacketType == PacketTypes.Core.INIT_READY;
    public bool IsIdentify => PacketType == PacketTypes.Core.IDENTIFY;

    public byte ErrorCode => IsNack && Payload.Length > 0 ? Payload[0] : (byte)0;

    public string ErrorMessage
    {
        get
        {
            var code = ErrorCode;
            if (code == 0) return "";
            var name = ErrorCodes.GetName(code);
            if (IsNack && Payload.Length > 1)
                return $"{name}: {System.Text.Encoding.UTF8.GetString(Payload, 1, Payload.Length - 1)}";
            return name;
        }
    }

    // Payload access helpers
    public byte GetU8(int offset) => offset < Payload.Length ? Payload[offset] : (byte)0;
    public ushort GetU16LE(int offset) => Endian.ReadU16LE(Payload.AsSpan(), offset);
    public uint GetU32LE(int offset) => Endian.ReadU32LE(Payload.AsSpan(), offset);
}

/// <summary>
/// Callback for async/unsolicited packets (tag=0 or unmatched).
/// </summary>
public delegate void AsyncPacketCallback(Response response);

/// <summary>
/// Serial connection to a ScaleFX controller.
/// Single reader thread dispatches tagged responses to waiters and async packets to callback.
/// Thread-safe for concurrent SendAndWaitAsync calls.
/// </summary>
public sealed class ScaleFxConnection : IDisposable
{
    public const int DefaultBaud = 6_000_000;
    public static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(2);
    public const byte TagAsync = 0x00;

    private SerialPort? _port;
    private Thread? _readerThread;
    private volatile bool _readerRunning;

    private readonly object _writeLock = new();
    private byte _nextTag = 1;

    private readonly ConcurrentDictionary<byte, TaskCompletionSource<Response>> _tagWaiters = new();

    public string PortName { get; }
    public int BaudRate { get; }
    public TimeSpan Timeout { get; set; } = DefaultTimeout;
    public bool Verbose { get; set; }
    public bool IsConnected => _port?.IsOpen == true;

    public event AsyncPacketCallback? OnAsyncPacket;

    public ScaleFxConnection(string portName, int baudRate = DefaultBaud, bool verbose = false)
    {
        PortName = portName;
        BaudRate = baudRate > 0 ? baudRate : DefaultBaud;
        Verbose = verbose;
    }

    /// <summary>
    /// Opens the serial port and starts the reader thread.
    /// </summary>
    public void Connect()
    {
        _port = new SerialPort(PortName, BaudRate, Parity.None, 8, StopBits.One)
        {
            ReadTimeout = 100,
            WriteTimeout = 1000,
            ReadBufferSize = 8192,
            WriteBufferSize = 4096,
            DtrEnable = true,
            RtsEnable = true
        };

        _port.Open();

        // Wait for device to settle, drain boot output
        Thread.Sleep(500);
        Drain();

        StartReader();
    }

    /// <summary>
    /// Closes the serial port and stops the reader thread.
    /// </summary>
    public void Close()
    {
        StopReader();
        if (_port?.IsOpen == true)
        {
            try { _port.Close(); } catch { /* ignore */ }
        }
        _port?.Dispose();
        _port = null;

        // Cancel all pending waiters
        foreach (var kvp in _tagWaiters)
        {
            if (_tagWaiters.TryRemove(kvp.Key, out var tcs))
                tcs.TrySetCanceled();
        }
    }

    /// <summary>
    /// Closes and reopens the connection.
    /// </summary>
    public void Reconnect()
    {
        Close();
        Thread.Sleep(300);
        Connect();
    }

    /// <summary>
    /// Returns the next correlation tag (1-255, wrapping).
    /// </summary>
    public byte NextTag()
    {
        var tag = _nextTag;
        _nextTag++;
        if (_nextTag == 0) _nextTag = 1;
        return tag;
    }

    /// <summary>
    /// Sends raw bytes to the serial port.
    /// </summary>
    public void Send(byte[] data)
    {
        if (_port?.IsOpen != true)
            throw new InvalidOperationException("Not connected");

        lock (_writeLock)
        {
            if (Verbose)
            {
                var parsed = Packet.Parse(data);
                if (parsed != null)
                    Console.WriteLine($"  → TX: {PacketTypes.GetName(parsed.PacketType)} tag={parsed.Tag} [{parsed.Payload.Length} bytes]");
            }

            _port.Write(data, 0, data.Length);
        }
    }

    /// <summary>
    /// Sends a packet with auto-assigned tag and waits for the correlated response.
    /// </summary>
    public async Task<Response> SendAndWaitAsync(byte[] data, CancellationToken ct = default)
    {
        if (_port?.IsOpen != true)
            throw new InvalidOperationException("Not connected");

        var tag = NextTag();
        var tagged = Packet.InjectTag(data, tag);

        var tcs = new TaskCompletionSource<Response>(TaskCreationOptions.RunContinuationsAsynchronously);
        _tagWaiters[tag] = tcs;

        try
        {
            Send(tagged);

            using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeoutCts.CancelAfter(Timeout);

            using var reg = timeoutCts.Token.Register(() =>
            {
                if (_tagWaiters.TryRemove(tag, out var removed))
                    removed.TrySetException(new TimeoutException("Timeout waiting for response"));
            });

            return await tcs.Task;
        }
        catch
        {
            _tagWaiters.TryRemove(tag, out _);
            throw;
        }
    }

    /// <summary>
    /// Synchronous send-and-wait for convenience (blocks calling thread).
    /// </summary>
    public Response SendAndWait(byte[] data)
        => SendAndWaitAsync(data).GetAwaiter().GetResult();

    /// <summary>
    /// Sends a packet and expects ACK/NACK. Same as SendAndWait.
    /// </summary>
    public Response SendExpectAck(byte[] data) => SendAndWait(data);

    /// <summary>
    /// Sends a packet and expects ACK/NACK (async version).
    /// </summary>
    public Task<Response> SendExpectAckAsync(byte[] data, CancellationToken ct = default)
        => SendAndWaitAsync(data, ct);

    /// <summary>
    /// Discards pending serial data.
    /// </summary>
    public void Drain()
    {
        if (_port?.IsOpen != true) return;
        try { _port.DiscardInBuffer(); } catch { /* ignore */ }

        // Also clear any pending tag waiters
        foreach (var kvp in _tagWaiters)
        {
            if (_tagWaiters.TryRemove(kvp.Key, out var tcs))
                tcs.TrySetCanceled();
        }
    }

    // ─── Reader Thread ───

    private void StartReader()
    {
        _readerRunning = true;
        _readerThread = new Thread(ReaderLoop)
        {
            IsBackground = true,
            Name = "ScaleFX-Reader"
        };
        _readerThread.Start();
    }

    private void StopReader()
    {
        _readerRunning = false;
        _readerThread?.Join(2000);
        _readerThread = null;
    }

    private void ReaderLoop()
    {
        var rxBuf = new List<byte>(8192);
        var readBuf = new byte[4096];

        while (_readerRunning)
        {
            if (_port?.IsOpen != true) break;

            // Read available data (100ms read timeout on port)
            int n;
            try
            {
                n = _port.Read(readBuf, 0, readBuf.Length);
            }
            catch (TimeoutException)
            {
                continue;
            }
            catch
            {
                break;
            }

            if (n > 0)
                rxBuf.AddRange(readBuf.AsSpan(0, n).ToArray());

            // Extract and dispatch complete packets (0x00 delimited)
            while (true)
            {
                var idx = rxBuf.IndexOf(0x00);
                if (idx < 0) break;

                if (idx > 0)
                {
                    var packetData = new byte[idx + 1];
                    rxBuf.CopyTo(0, packetData, 0, idx);
                    packetData[idx] = 0x00; // delimiter

                    var parsed = Packet.Parse(packetData);
                    if (parsed != null)
                    {
                        var resp = new Response
                        {
                            PacketType = parsed.PacketType,
                            Tag = parsed.Tag,
                            Payload = parsed.Payload,
                            Raw = packetData
                        };

                        if (Verbose)
                            Console.WriteLine($"  ← RX: {PacketTypes.GetName(resp.PacketType)} tag={resp.Tag} [{resp.Payload.Length} bytes]");

                        DispatchResponse(resp);
                    }
                }

                // Advance past this packet (including delimiter)
                rxBuf.RemoveRange(0, idx + 1);
            }

            // Prevent unbounded growth
            if (rxBuf.Count > 16384)
                rxBuf.RemoveRange(0, rxBuf.Count - 4096);
        }
    }

    private void DispatchResponse(Response resp)
    {
        // Check for a registered tag waiter
        if (resp.Tag != TagAsync)
        {
            if (_tagWaiters.TryRemove(resp.Tag, out var tcs))
            {
                tcs.TrySetResult(resp);
                return;
            }
        }

        // Async or unmatched — deliver to callback
        OnAsyncPacket?.Invoke(resp);
    }

    // ─── Port Discovery ───

    /// <summary>
    /// Returns available serial port names.
    /// </summary>
    public static string[] ListPorts() => SerialPort.GetPortNames();

    public void Dispose() => Close();
}
