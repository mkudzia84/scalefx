using System.IO.Ports;
using System.Text;

namespace ScaleFXStudio.Services;

/// <summary>
/// Service for managing serial port connections to devices like Raspberry Pi over USB.
/// Supports USB gadget mode (Pi Zero/4) and USB-to-Serial adapters.
/// </summary>
public class SerialService : IDisposable
{
    private SerialPort? _serialPort;
    private bool _disposed;
    private readonly StringBuilder _receiveBuffer = new();
    private readonly object _bufferLock = new();

    /// <summary>
    /// Default baud rate for Raspberry Pi serial console.
    /// </summary>
    public const int DefaultBaudRate = 115200;

    /// <summary>
    /// Gets a value indicating whether the serial port is open.
    /// </summary>
    public bool IsConnected => _serialPort?.IsOpen == true;

    /// <summary>
    /// Gets the current connection information.
    /// </summary>
    public SerialConnectionInfo? ConnectionInfo { get; private set; }

    /// <summary>
    /// Event raised when connection status changes.
    /// </summary>
    public event EventHandler<SerialConnectionStatusEventArgs>? ConnectionStatusChanged;

    /// <summary>
    /// Event raised when data is received from the serial port.
    /// </summary>
    public event EventHandler<SerialDataReceivedEventArgs>? DataReceived;

    /// <summary>
    /// Gets a list of available serial ports on the system.
    /// </summary>
    /// <returns>Array of available COM port names.</returns>
    public static string[] GetAvailablePorts()
    {
        return SerialPort.GetPortNames().OrderBy(p => p).ToArray();
    }

    /// <summary>
    /// Gets detailed information about available serial ports.
    /// </summary>
    /// <returns>List of serial port information.</returns>
    public static List<SerialPortInfo> GetAvailablePortsDetailed()
    {
        var ports = new List<SerialPortInfo>();
        
        foreach (var portName in SerialPort.GetPortNames().OrderBy(p => p))
        {
            var info = new SerialPortInfo { PortName = portName };
            
            // Try to get additional info via WMI (device description)
            try
            {
                using var searcher = new System.Management.ManagementObjectSearcher(
                    $"SELECT * FROM Win32_PnPEntity WHERE Name LIKE '%({portName})%'");
                
                foreach (var device in searcher.Get())
                {
                    info.Description = device["Name"]?.ToString() ?? portName;
                    info.DeviceId = device["DeviceID"]?.ToString() ?? "";
                    break;
                }
            }
            catch
            {
                info.Description = portName;
            }

            ports.Add(info);
        }

        return ports;
    }

    /// <summary>
    /// Connects to a serial port.
    /// </summary>
    /// <param name="portName">The name of the serial port (e.g., "COM3").</param>
    /// <param name="baudRate">The baud rate (default: 115200).</param>
    /// <param name="dataBits">The data bits (default: 8).</param>
    /// <param name="parity">The parity (default: None).</param>
    /// <param name="stopBits">The stop bits (default: One).</param>
    public void Connect(string portName, int baudRate = DefaultBaudRate, int dataBits = 8, 
        Parity parity = Parity.None, StopBits stopBits = StopBits.One)
    {
        ThrowIfDisposed();
        Disconnect();

        _serialPort = new SerialPort(portName, baudRate, parity, dataBits, stopBits)
        {
            ReadTimeout = 5000,
            WriteTimeout = 5000,
            Encoding = Encoding.UTF8,
            NewLine = "\n"
        };

        _serialPort.DataReceived += OnSerialDataReceived;
        _serialPort.ErrorReceived += OnSerialErrorReceived;

        _serialPort.Open();

        ConnectionInfo = new SerialConnectionInfo(portName, baudRate, dataBits, parity, stopBits);
        OnConnectionStatusChanged(true, $"Connected to {portName} at {baudRate} baud");
    }

    /// <summary>
    /// Disconnects from the serial port.
    /// </summary>
    public void Disconnect()
    {
        if (_serialPort != null)
        {
            if (_serialPort.IsOpen)
            {
                _serialPort.DataReceived -= OnSerialDataReceived;
                _serialPort.ErrorReceived -= OnSerialErrorReceived;
                _serialPort.Close();
            }
            _serialPort.Dispose();
            _serialPort = null;
        }

        ConnectionInfo = null;
        ClearReceiveBuffer();
        OnConnectionStatusChanged(false, "Disconnected");
    }

    /// <summary>
    /// Sends a text string to the serial port.
    /// </summary>
    /// <param name="text">The text to send.</param>
    public void Send(string text)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        _serialPort!.Write(text);
    }

    /// <summary>
    /// Sends a line of text to the serial port (appends newline).
    /// </summary>
    /// <param name="line">The line to send.</param>
    public void SendLine(string line)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        _serialPort!.WriteLine(line);
    }

    /// <summary>
    /// Sends raw bytes to the serial port.
    /// </summary>
    /// <param name="data">The bytes to send.</param>
    public void SendBytes(byte[] data)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        _serialPort!.Write(data, 0, data.Length);
    }

    /// <summary>
    /// Sends a command and waits for a response.
    /// </summary>
    /// <param name="command">The command to send.</param>
    /// <param name="timeoutMs">Timeout in milliseconds.</param>
    /// <param name="expectedPrompt">Optional prompt to wait for (e.g., "$ " or "# ").</param>
    /// <returns>The response received.</returns>
    public async Task<string> SendCommandAsync(string command, int timeoutMs = 5000, string? expectedPrompt = null)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        ClearReceiveBuffer();
        SendLine(command);

        return await WaitForResponseAsync(timeoutMs, expectedPrompt);
    }

    /// <summary>
    /// Waits for a response from the serial port.
    /// </summary>
    /// <param name="timeoutMs">Timeout in milliseconds.</param>
    /// <param name="expectedPrompt">Optional prompt to wait for.</param>
    /// <returns>The received data.</returns>
    public async Task<string> WaitForResponseAsync(int timeoutMs = 5000, string? expectedPrompt = null)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        var startTime = DateTime.UtcNow;
        var timeout = TimeSpan.FromMilliseconds(timeoutMs);

        while (DateTime.UtcNow - startTime < timeout)
        {
            await Task.Delay(50);

            string buffer;
            lock (_bufferLock)
            {
                buffer = _receiveBuffer.ToString();
            }

            if (!string.IsNullOrEmpty(expectedPrompt) && buffer.Contains(expectedPrompt))
            {
                ClearReceiveBuffer();
                return buffer;
            }
        }

        string result;
        lock (_bufferLock)
        {
            result = _receiveBuffer.ToString();
        }
        ClearReceiveBuffer();
        return result;
    }

    /// <summary>
    /// Lists directory contents on the remote device.
    /// Requires a shell prompt on the Pi.
    /// </summary>
    /// <param name="remotePath">The path to list.</param>
    /// <param name="timeoutMs">Command timeout in milliseconds.</param>
    /// <returns>List of file entries.</returns>
    public async Task<List<RemoteFileEntry>> ListDirectoryAsync(string remotePath, int timeoutMs = 10000)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        var entries = new List<RemoteFileEntry>();

        // Use ls -la for detailed listing
        var response = await SendCommandAsync($"ls -la \"{remotePath}\"", timeoutMs);

        var lines = response.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        
        foreach (var line in lines)
        {
            var trimmed = line.Trim();
            
            // Skip total line and empty lines
            if (string.IsNullOrEmpty(trimmed) || trimmed.StartsWith("total "))
                continue;

            // Skip the command echo
            if (trimmed.StartsWith("ls "))
                continue;

            // Parse ls -la output: drwxr-xr-x 2 user group 4096 Dec 22 10:30 filename
            var parts = trimmed.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 9)
                continue;

            var permissions = parts[0];
            var name = string.Join(" ", parts.Skip(8)); // Handle filenames with spaces

            // Skip . and .. entries
            if (name == "." || name == "..")
                continue;

            var isDirectory = permissions.StartsWith("d");
            long.TryParse(parts[4], out var size);

            entries.Add(new RemoteFileEntry
            {
                Name = name,
                FullPath = remotePath.TrimEnd('/') + "/" + name,
                IsDirectory = isDirectory,
                Size = size,
                Permissions = permissions
            });
        }

        return entries.OrderByDescending(e => e.IsDirectory).ThenBy(e => e.Name).ToList();
    }

    /// <summary>
    /// Downloads a file from the remote device using base64 encoding.
    /// Requires base64 command on the Pi.
    /// </summary>
    /// <param name="remotePath">The remote file path.</param>
    /// <param name="localPath">The local destination path.</param>
    /// <param name="timeoutMs">Command timeout in milliseconds.</param>
    public async Task DownloadFileAsync(string remotePath, string localPath, int timeoutMs = 60000)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        // Use base64 to encode the file for transfer
        ClearReceiveBuffer();
        SendLine($"base64 \"{remotePath}\"");

        // Wait and collect the base64 output
        await Task.Delay(500);
        var response = await WaitForResponseAsync(timeoutMs);

        // Extract base64 content (skip command echo and prompt)
        var lines = response.Split('\n');
        var base64Lines = new List<string>();
        var capturing = false;

        foreach (var line in lines)
        {
            var trimmed = line.Trim();
            
            // Skip the command echo
            if (trimmed.StartsWith("base64 "))
            {
                capturing = true;
                continue;
            }

            // Stop at shell prompt
            if (capturing && (trimmed.EndsWith("$") || trimmed.EndsWith("#") || trimmed.Contains("@")))
            {
                // Check if it's just a prompt
                if (trimmed.Length < 50 && (trimmed.Contains("$") || trimmed.Contains("#")))
                    break;
            }

            if (capturing && !string.IsNullOrWhiteSpace(trimmed))
            {
                base64Lines.Add(trimmed);
            }
        }

        var base64Content = string.Join("", base64Lines);
        
        if (string.IsNullOrEmpty(base64Content))
        {
            throw new IOException($"Failed to read file: {remotePath}");
        }

        var fileBytes = Convert.FromBase64String(base64Content);
        await File.WriteAllBytesAsync(localPath, fileBytes);
    }

    /// <summary>
    /// Uploads a file to the remote device using base64 encoding.
    /// Requires base64 command on the Pi.
    /// </summary>
    /// <param name="localPath">The local file path.</param>
    /// <param name="remotePath">The remote destination path.</param>
    /// <param name="timeoutMs">Command timeout in milliseconds.</param>
    public async Task UploadFileAsync(string localPath, string remotePath, int timeoutMs = 60000)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        var fileBytes = await File.ReadAllBytesAsync(localPath);
        var base64Content = Convert.ToBase64String(fileBytes);

        // Split into chunks to avoid buffer overflow
        const int chunkSize = 512;
        
        // Start writing to a temp file
        var tempFile = $"/tmp/upload_{Guid.NewGuid():N}";
        
        await SendCommandAsync($"rm -f \"{tempFile}\"", 2000);

        for (int i = 0; i < base64Content.Length; i += chunkSize)
        {
            var chunk = base64Content.Substring(i, Math.Min(chunkSize, base64Content.Length - i));
            await SendCommandAsync($"echo -n '{chunk}' >> \"{tempFile}\"", 2000);
        }

        // Decode the base64 file to the target location
        var response = await SendCommandAsync($"base64 -d \"{tempFile}\" > \"{remotePath}\" && echo 'OK' || echo 'FAIL'", timeoutMs);
        
        // Clean up temp file
        await SendCommandAsync($"rm -f \"{tempFile}\"", 2000);

        if (!response.Contains("OK"))
        {
            throw new IOException($"Failed to upload file to: {remotePath}");
        }
    }

    /// <summary>
    /// Checks if a path exists on the remote device.
    /// </summary>
    /// <param name="remotePath">The path to check.</param>
    /// <returns>True if the path exists.</returns>
    public async Task<bool> RemotePathExistsAsync(string remotePath)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        var response = await SendCommandAsync($"test -e \"{remotePath}\" && echo 'EXISTS' || echo 'NOTFOUND'", 5000);
        return response.Contains("EXISTS");
    }

    /// <summary>
    /// Creates a directory on the remote device.
    /// </summary>
    /// <param name="remotePath">The directory path to create.</param>
    public async Task CreateRemoteDirectoryAsync(string remotePath)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        await SendCommandAsync($"mkdir -p \"{remotePath}\"", 5000);
    }

    /// <summary>
    /// Deletes a file on the remote device.
    /// </summary>
    /// <param name="remotePath">The file path to delete.</param>
    public async Task DeleteRemoteFileAsync(string remotePath)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        await SendCommandAsync($"rm -f \"{remotePath}\"", 5000);
    }

    /// <summary>
    /// Sends a login sequence for auto-login.
    /// </summary>
    /// <param name="username">The username.</param>
    /// <param name="password">The password.</param>
    /// <param name="timeoutMs">Timeout in milliseconds.</param>
    /// <returns>True if login appears successful.</returns>
    public async Task<bool> LoginAsync(string username, string password, int timeoutMs = 10000)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        // Send Enter to get a login prompt
        SendLine("");
        await Task.Delay(500);

        var response = await WaitForResponseAsync(3000);

        // Check if we already have a shell prompt
        if (response.Contains("$") || response.Contains("#"))
        {
            return true;
        }

        // Look for login prompt
        if (response.ToLower().Contains("login:"))
        {
            SendLine(username);
            await Task.Delay(500);
            response = await WaitForResponseAsync(3000);
        }

        // Look for password prompt
        if (response.ToLower().Contains("password:"))
        {
            SendLine(password);
            response = await WaitForResponseAsync(timeoutMs);
        }

        // Check for shell prompt indicating successful login
        return response.Contains("$") || response.Contains("#");
    }

    private void ClearReceiveBuffer()
    {
        lock (_bufferLock)
        {
            _receiveBuffer.Clear();
        }
    }

    private void OnSerialDataReceived(object sender, System.IO.Ports.SerialDataReceivedEventArgs e)
    {
        if (_serialPort == null || !_serialPort.IsOpen)
            return;

        try
        {
            var data = _serialPort.ReadExisting();
            
            lock (_bufferLock)
            {
                _receiveBuffer.Append(data);
            }

            DataReceived?.Invoke(this, new SerialDataReceivedEventArgs(data));
        }
        catch (Exception)
        {
            // Port may have been closed
        }
    }

    private void OnSerialErrorReceived(object sender, SerialErrorReceivedEventArgs e)
    {
        OnConnectionStatusChanged(IsConnected, $"Serial error: {e.EventType}");
    }

    private void OnConnectionStatusChanged(bool isConnected, string message)
    {
        ConnectionStatusChanged?.Invoke(this, new SerialConnectionStatusEventArgs(isConnected, message));
    }

    private void ThrowIfDisposed()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(SerialService));
        }
    }

    private void ThrowIfNotConnected()
    {
        if (!IsConnected)
        {
            throw new InvalidOperationException("Not connected to a serial port. Call Connect first.");
        }
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            Disconnect();
            _disposed = true;
        }
        GC.SuppressFinalize(this);
    }
}

/// <summary>
/// Information about an available serial port.
/// </summary>
public class SerialPortInfo
{
    public string PortName { get; set; } = string.Empty;
    public string Description { get; set; } = string.Empty;
    public string DeviceId { get; set; } = string.Empty;

    public override string ToString() => string.IsNullOrEmpty(Description) ? PortName : $"{PortName} - {Description}";
}

/// <summary>
/// Represents information about a serial connection.
/// </summary>
public record SerialConnectionInfo(string PortName, int BaudRate, int DataBits, Parity Parity, StopBits StopBits);

/// <summary>
/// Event arguments for serial connection status changes.
/// </summary>
public class SerialConnectionStatusEventArgs : EventArgs
{
    public bool IsConnected { get; }
    public string Message { get; }

    public SerialConnectionStatusEventArgs(bool isConnected, string message)
    {
        IsConnected = isConnected;
        Message = message;
    }
}

/// <summary>
/// Event arguments for serial data received.
/// </summary>
public class SerialDataReceivedEventArgs : EventArgs
{
    public string Data { get; }

    public SerialDataReceivedEventArgs(string data)
    {
        Data = data;
    }
}
