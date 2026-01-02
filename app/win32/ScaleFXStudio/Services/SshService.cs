using Renci.SshNet;
using Renci.SshNet.Sftp;

namespace ScaleFXStudio.Services;

/// <summary>
/// Service for managing SSH connections to remote devices.
/// Provides functionality for listing directories and transferring files.
/// </summary>
public class SshService : IDisposable
{
    private SshClient? _sshClient;
    private SftpClient? _sftpClient;
    private bool _disposed;

    /// <summary>
    /// Gets a value indicating whether the SSH connection is established.
    /// </summary>
    public bool IsConnected => _sshClient?.IsConnected == true;

    /// <summary>
    /// Gets the current connection information.
    /// </summary>
    public SshConnectionInfo? ConnectionInfo { get; private set; }

    /// <summary>
    /// Event raised when connection status changes.
    /// </summary>
    public event EventHandler<SshConnectionStatusEventArgs>? ConnectionStatusChanged;

    /// <summary>
    /// Event raised when a file transfer progress updates.
    /// </summary>
    public event EventHandler<SshFileTransferProgressEventArgs>? FileTransferProgress;

    /// <summary>
    /// Connects to a remote device using password authentication.
    /// </summary>
    /// <param name="host">The hostname or IP address of the remote device.</param>
    /// <param name="port">The SSH port (default: 22).</param>
    /// <param name="username">The username for authentication.</param>
    /// <param name="password">The password for authentication.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    public async Task ConnectAsync(string host, int port, string username, string password, CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();

        await Task.Run(() =>
        {
            Disconnect();

            var connectionInfo = new ConnectionInfo(host, port, username,
                new PasswordAuthenticationMethod(username, password));

            _sshClient = new SshClient(connectionInfo);
            _sftpClient = new SftpClient(connectionInfo);

            _sshClient.Connect();
            _sftpClient.Connect();

            ConnectionInfo = new SshConnectionInfo(host, port, username);
            OnConnectionStatusChanged(true, "Connected successfully");
        }, cancellationToken);
    }

    /// <summary>
    /// Connects to a remote device using private key authentication.
    /// </summary>
    /// <param name="host">The hostname or IP address of the remote device.</param>
    /// <param name="port">The SSH port (default: 22).</param>
    /// <param name="username">The username for authentication.</param>
    /// <param name="privateKeyPath">Path to the private key file.</param>
    /// <param name="passphrase">Optional passphrase for the private key.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    public async Task ConnectWithKeyAsync(string host, int port, string username, string privateKeyPath, string? passphrase = null, CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();

        await Task.Run(() =>
        {
            Disconnect();

            PrivateKeyFile keyFile = string.IsNullOrEmpty(passphrase)
                ? new PrivateKeyFile(privateKeyPath)
                : new PrivateKeyFile(privateKeyPath, passphrase);

            var connectionInfo = new ConnectionInfo(host, port, username,
                new PrivateKeyAuthenticationMethod(username, keyFile));

            _sshClient = new SshClient(connectionInfo);
            _sftpClient = new SftpClient(connectionInfo);

            _sshClient.Connect();
            _sftpClient.Connect();

            ConnectionInfo = new SshConnectionInfo(host, port, username);
            OnConnectionStatusChanged(true, "Connected successfully");
        }, cancellationToken);
    }

    /// <summary>
    /// Disconnects from the remote device.
    /// </summary>
    public void Disconnect()
    {
        if (_sftpClient?.IsConnected == true)
        {
            _sftpClient.Disconnect();
        }
        _sftpClient?.Dispose();
        _sftpClient = null;

        if (_sshClient?.IsConnected == true)
        {
            _sshClient.Disconnect();
        }
        _sshClient?.Dispose();
        _sshClient = null;

        ConnectionInfo = null;
        OnConnectionStatusChanged(false, "Disconnected");
    }

    /// <summary>
    /// Lists the contents of a remote directory.
    /// </summary>
    /// <param name="remotePath">The path to the remote directory.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    /// <returns>A list of remote file entries.</returns>
    public async Task<List<RemoteFileEntry>> ListDirectoryAsync(string remotePath, CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        return await Task.Run(() =>
        {
            var entries = new List<RemoteFileEntry>();
            var files = _sftpClient!.ListDirectory(remotePath);

            foreach (var file in files)
            {
                cancellationToken.ThrowIfCancellationRequested();

                // Skip . and .. entries
                if (file.Name == "." || file.Name == "..")
                    continue;

                entries.Add(new RemoteFileEntry
                {
                    Name = file.Name,
                    FullPath = file.FullName,
                    IsDirectory = file.IsDirectory,
                    Size = file.Length,
                    LastModified = file.LastWriteTime,
                    Permissions = file.IsDirectory ? "d" + GetPermissionString(file) : "-" + GetPermissionString(file)
                });
            }

            return entries.OrderByDescending(e => e.IsDirectory).ThenBy(e => e.Name).ToList();
        }, cancellationToken);
    }

    /// <summary>
    /// Downloads a file from the remote device.
    /// </summary>
    /// <param name="remotePath">The path to the remote file.</param>
    /// <param name="localPath">The local destination path.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    public async Task DownloadFileAsync(string remotePath, string localPath, CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        await Task.Run(() =>
        {
            using var fileStream = File.Create(localPath);
            var fileInfo = _sftpClient!.Get(remotePath);
            var totalBytes = fileInfo.Length;
            var fileName = Path.GetFileName(remotePath);

            _sftpClient.DownloadFile(remotePath, fileStream, bytesTransferred =>
            {
                var progress = totalBytes > 0 ? (int)((bytesTransferred * 100) / (ulong)totalBytes) : 0;
                OnFileTransferProgress(fileName, bytesTransferred, (ulong)totalBytes, progress, false);
            });

            OnFileTransferProgress(fileName, (ulong)totalBytes, (ulong)totalBytes, 100, true);
        }, cancellationToken);
    }

    /// <summary>
    /// Uploads a file to the remote device.
    /// </summary>
    /// <param name="localPath">The path to the local file.</param>
    /// <param name="remotePath">The remote destination path.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    public async Task UploadFileAsync(string localPath, string remotePath, CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        await Task.Run(() =>
        {
            using var fileStream = File.OpenRead(localPath);
            var totalBytes = (ulong)fileStream.Length;
            var fileName = Path.GetFileName(localPath);

            _sftpClient!.UploadFile(fileStream, remotePath, bytesTransferred =>
            {
                var progress = totalBytes > 0 ? (int)((bytesTransferred * 100) / totalBytes) : 0;
                OnFileTransferProgress(fileName, bytesTransferred, totalBytes, progress, false);
            });

            OnFileTransferProgress(fileName, totalBytes, totalBytes, 100, true);
        }, cancellationToken);
    }

    /// <summary>
    /// Downloads multiple files from a remote directory.
    /// </summary>
    /// <param name="remotePath">The remote directory path.</param>
    /// <param name="localPath">The local destination directory.</param>
    /// <param name="recursive">Whether to download subdirectories recursively.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    public async Task DownloadDirectoryAsync(string remotePath, string localPath, bool recursive = true, CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        Directory.CreateDirectory(localPath);
        var entries = await ListDirectoryAsync(remotePath, cancellationToken);

        foreach (var entry in entries)
        {
            cancellationToken.ThrowIfCancellationRequested();

            var localEntryPath = Path.Combine(localPath, entry.Name);

            if (entry.IsDirectory && recursive)
            {
                await DownloadDirectoryAsync(entry.FullPath, localEntryPath, recursive, cancellationToken);
            }
            else if (!entry.IsDirectory)
            {
                await DownloadFileAsync(entry.FullPath, localEntryPath, cancellationToken);
            }
        }
    }

    /// <summary>
    /// Uploads multiple files to a remote directory.
    /// </summary>
    /// <param name="localPath">The local directory path.</param>
    /// <param name="remotePath">The remote destination directory.</param>
    /// <param name="recursive">Whether to upload subdirectories recursively.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    public async Task UploadDirectoryAsync(string localPath, string remotePath, bool recursive = true, CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        await Task.Run(() =>
        {
            if (!_sftpClient!.Exists(remotePath))
            {
                _sftpClient.CreateDirectory(remotePath);
            }
        }, cancellationToken);

        foreach (var file in Directory.GetFiles(localPath))
        {
            cancellationToken.ThrowIfCancellationRequested();
            var remoteFilePath = $"{remotePath}/{Path.GetFileName(file)}";
            await UploadFileAsync(file, remoteFilePath, cancellationToken);
        }

        if (recursive)
        {
            foreach (var directory in Directory.GetDirectories(localPath))
            {
                cancellationToken.ThrowIfCancellationRequested();
                var dirName = Path.GetFileName(directory);
                var remoteSubPath = $"{remotePath}/{dirName}";
                await UploadDirectoryAsync(directory, remoteSubPath, recursive, cancellationToken);
            }
        }
    }

    /// <summary>
    /// Executes a command on the remote device.
    /// </summary>
    /// <param name="command">The command to execute.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    /// <returns>The command result.</returns>
    public async Task<SshCommandResult> ExecuteCommandAsync(string command, CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        return await Task.Run(() =>
        {
            using var cmd = _sshClient!.CreateCommand(command);
            var result = cmd.Execute();
            return new SshCommandResult
            {
                Output = result,
                Error = cmd.Error,
                ExitCode = cmd.ExitStatus
            };
        }, cancellationToken);
    }

    /// <summary>
    /// Checks if a remote path exists.
    /// </summary>
    /// <param name="remotePath">The remote path to check.</param>
    /// <returns>True if the path exists, false otherwise.</returns>
    public bool RemotePathExists(string remotePath)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        return _sftpClient!.Exists(remotePath);
    }

    /// <summary>
    /// Creates a directory on the remote device.
    /// </summary>
    /// <param name="remotePath">The path of the directory to create.</param>
    public void CreateRemoteDirectory(string remotePath)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        _sftpClient!.CreateDirectory(remotePath);
    }

    /// <summary>
    /// Deletes a file on the remote device.
    /// </summary>
    /// <param name="remotePath">The path of the file to delete.</param>
    public void DeleteRemoteFile(string remotePath)
    {
        ThrowIfDisposed();
        ThrowIfNotConnected();

        _sftpClient!.DeleteFile(remotePath);
    }

    private static string GetPermissionString(ISftpFile file)
    {
        // Simplified permission string
        return file.IsRegularFile ? "rw-r--r--" : "rwxr-xr-x";
    }

    private void OnConnectionStatusChanged(bool isConnected, string message)
    {
        ConnectionStatusChanged?.Invoke(this, new SshConnectionStatusEventArgs(isConnected, message));
    }

    private void OnFileTransferProgress(string fileName, ulong bytesTransferred, ulong totalBytes, int percentage, bool isComplete)
    {
        FileTransferProgress?.Invoke(this, new SshFileTransferProgressEventArgs(fileName, bytesTransferred, totalBytes, percentage, isComplete));
    }

    private void ThrowIfDisposed()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(SshService));
        }
    }

    private void ThrowIfNotConnected()
    {
        if (!IsConnected)
        {
            throw new InvalidOperationException("Not connected to a remote device. Call ConnectAsync first.");
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
/// Represents information about an SSH connection.
/// </summary>
public record SshConnectionInfo(string Host, int Port, string Username);

/// <summary>
/// Represents a remote file or directory entry.
/// </summary>
public class RemoteFileEntry
{
    public string Name { get; set; } = string.Empty;
    public string FullPath { get; set; } = string.Empty;
    public bool IsDirectory { get; set; }
    public long Size { get; set; }
    public DateTime LastModified { get; set; }
    public string Permissions { get; set; } = string.Empty;

    public string SizeFormatted => IsDirectory ? "<DIR>" : FormatFileSize(Size);

    private static string FormatFileSize(long bytes)
    {
        string[] sizes = { "B", "KB", "MB", "GB", "TB" };
        int order = 0;
        double size = bytes;
        while (size >= 1024 && order < sizes.Length - 1)
        {
            order++;
            size /= 1024;
        }
        return $"{size:0.##} {sizes[order]}";
    }
}

/// <summary>
/// Represents the result of an SSH command execution.
/// </summary>
public class SshCommandResult
{
    public string Output { get; set; } = string.Empty;
    public string Error { get; set; } = string.Empty;
    public int ExitCode { get; set; }
    public bool IsSuccess => ExitCode == 0;
}

/// <summary>
/// Event arguments for SSH connection status changes.
/// </summary>
public class SshConnectionStatusEventArgs : EventArgs
{
    public bool IsConnected { get; }
    public string Message { get; }

    public SshConnectionStatusEventArgs(bool isConnected, string message)
    {
        IsConnected = isConnected;
        Message = message;
    }
}

/// <summary>
/// Event arguments for file transfer progress.
/// </summary>
public class SshFileTransferProgressEventArgs : EventArgs
{
    public string FileName { get; }
    public ulong BytesTransferred { get; }
    public ulong TotalBytes { get; }
    public int Percentage { get; }
    public bool IsComplete { get; }

    public SshFileTransferProgressEventArgs(string fileName, ulong bytesTransferred, ulong totalBytes, int percentage, bool isComplete)
    {
        FileName = fileName;
        BytesTransferred = bytesTransferred;
        TotalBytes = totalBytes;
        Percentage = percentage;
        IsComplete = isComplete;
    }
}
