using ScaleFX.Serial;

namespace ScaleFXStudio;

/// <summary>
/// Startup dialog that scans for ScaleFX boards. Shows a spinner and
/// port scan progress. Closes automatically when a board is detected,
/// or the user can skip to launch without a connection.
/// </summary>
public class BoardWaitDialog : Form
{
    private readonly Label _titleLabel;
    private readonly Label _statusLabel;
    private readonly Label _spinnerLabel;
    private readonly ListBox _portList;
    private readonly Button _skipButton;
    private readonly Button _connectButton;
    private readonly Panel _boardInfoPanel;
    private readonly Label _boardInfoLabel;
    private readonly System.Windows.Forms.Timer _animTimer;
    private readonly System.Windows.Forms.Timer _scanTimer;

    private readonly BoardDetector _detector;
    private CancellationTokenSource? _scanCts;
    private int _spinnerFrame;
    private static readonly string[] SpinnerFrames = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];

    /// <summary>The board selected by the user (null if skipped).</summary>
    public BoardInfo? SelectedBoard { get; private set; }

    public BoardWaitDialog()
    {
        _detector = new BoardDetector
        {
            ProbeTimeout = TimeSpan.FromSeconds(3)
        };

        // ─── Form ───
        Text = "ScaleFX Studio";
        Size = new Size(480, 380);
        MinimumSize = new Size(480, 380);
        MaximumSize = new Size(480, 500);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        ShowInTaskbar = true;
        BackColor = SystemColors.Control;
        ForeColor = SystemColors.ControlText;
        KeyPreview = true;

        // ─── Title ───
        _titleLabel = new Label
        {
            Text = "ScaleFX Studio",
            Font = new Font("Segoe UI", 16F, FontStyle.Bold),
            ForeColor = SystemColors.Highlight,
            Dock = DockStyle.Top,
            Height = 50,
            TextAlign = ContentAlignment.MiddleCenter,
            Padding = new Padding(0, 10, 0, 0)
        };

        // ─── Spinner + Status ───
        var statusPanel = new Panel
        {
            Dock = DockStyle.Top,
            Height = 35
        };

        _spinnerLabel = new Label
        {
            Text = SpinnerFrames[0],
            Font = new Font("Segoe UI", 14F),
            ForeColor = Color.FromArgb(0, 128, 0),
            Location = new Point(110, 5),
            AutoSize = true
        };

        _statusLabel = new Label
        {
            Text = "Scanning for boards...",
            Font = new Font("Segoe UI", 10F),
            ForeColor = SystemColors.ControlText,
            Location = new Point(135, 8),
            AutoSize = true
        };

        statusPanel.Controls.Add(_spinnerLabel);
        statusPanel.Controls.Add(_statusLabel);

        // ─── Port List ───
        _portList = new ListBox
        {
            Dock = DockStyle.Fill,
            BackColor = SystemColors.Window,
            ForeColor = SystemColors.WindowText,
            Font = new Font("Consolas", 9.5F),
            BorderStyle = BorderStyle.FixedSingle,
            SelectionMode = SelectionMode.One,
            IntegralHeight = false,
        };
        _portList.SelectedIndexChanged += OnPortSelected;

        // keep this minimal container for list indentation
        var listPanel = new Panel
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(20, 5, 20, 5)
        };
        listPanel.Controls.Add(_portList);

        // ─── Board Info (shown when a board is detected) ───
        _boardInfoPanel = new Panel
        {
            Dock = DockStyle.Bottom,
            Height = 55,
            Padding = new Padding(20, 0, 20, 0),
            Visible = false
        };

        _boardInfoLabel = new Label
        {
            Dock = DockStyle.Fill,
            Font = new Font("Consolas", 9F),
            ForeColor = Color.FromArgb(0, 128, 0),
            TextAlign = ContentAlignment.MiddleLeft,
        };
        _boardInfoPanel.Controls.Add(_boardInfoLabel);

        // ─── Buttons ───
        var buttonPanel = new Panel
        {
            Dock = DockStyle.Bottom,
            Height = 50,
            Padding = new Padding(20, 8, 20, 8)
        };

        _connectButton = new Button
        {
            Text = "Connect",
            Width = 100,
            Height = 32,
            Dock = DockStyle.Right,
            Enabled = false,
            Font = new Font("Segoe UI", 9F, FontStyle.Bold),
        };
        _connectButton.Click += OnConnect;

        _skipButton = new Button
        {
            Text = "Skip",
            Width = 80,
            Height = 32,
            Dock = DockStyle.Left,
            Font = new Font("Segoe UI", 9F),
        };
        _skipButton.Click += OnSkip;

        buttonPanel.Controls.Add(_connectButton);
        buttonPanel.Controls.Add(_skipButton);

        // ─── Layout ───
        Controls.Add(listPanel);
        Controls.Add(_boardInfoPanel);
        Controls.Add(buttonPanel);
        Controls.Add(statusPanel);
        Controls.Add(_titleLabel);

        // ─── Timers ───
        _animTimer = new System.Windows.Forms.Timer { Interval = 80 };
        _animTimer.Tick += OnAnimTick;

        _scanTimer = new System.Windows.Forms.Timer { Interval = 3000 };
        _scanTimer.Tick += OnScanTick;

        // ─── Events ───
        Shown += OnShown;
        FormClosing += OnFormClosing;
        KeyDown += OnKeyDown;
    }

    // ─── Lifecycle ───

    private async void OnShown(object? sender, EventArgs e)
    {
        _animTimer.Start();
        await RunScanAsync();
        _scanTimer.Start();
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs e)
    {
        _animTimer.Stop();
        _scanTimer.Stop();
        _scanCts?.Cancel();

        // If user X-closes without selecting, treat as skip
        if (DialogResult == DialogResult.None)
            DialogResult = DialogResult.Cancel;
    }

    private void OnKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.KeyCode == Keys.Escape)
        {
            OnSkip(this, EventArgs.Empty);
            e.Handled = true;
        }
        else if (e.KeyCode == Keys.Enter && _connectButton.Enabled)
        {
            OnConnect(this, EventArgs.Empty);
            e.Handled = true;
        }
    }

    // ─── Scanning ───

    private async Task RunScanAsync()
    {
        _scanCts?.Cancel();
        _scanCts = new CancellationTokenSource();

        _portList.Items.Clear();
        _statusLabel.Text = "Scanning for boards...";

        _detector.ScanningPort += port =>
        {
            if (InvokeRequired) BeginInvoke(() => UpdateScanStatus(port));
            else UpdateScanStatus(port);
        };

        try
        {
            var boards = await _detector.ScanOnceAsync(_scanCts.Token);

            if (IsDisposed) return;

            _portList.Items.Clear();

            if (boards.Count == 0)
            {
                _statusLabel.Text = "No boards found. Retrying...";
                _connectButton.Enabled = false;
                _boardInfoPanel.Visible = false;
            }
            else
            {
                _statusLabel.Text = $"Found {boards.Count} board{(boards.Count > 1 ? "s" : "")}:";
                foreach (var board in boards)
                {
                    _portList.Items.Add(new BoardListItem(board));
                }

                // Auto-select first (or single) board
                _portList.SelectedIndex = 0;

                // If only one board, auto-connect after brief delay
                if (boards.Count == 1)
                {
                    _connectButton.Enabled = true;
                    ShowBoardInfo(boards[0]);
                }
            }
        }
        catch (OperationCanceledException) { /* expected */ }
        catch (Exception ex)
        {
            if (!IsDisposed)
                _statusLabel.Text = $"Scan error: {ex.Message}";
        }
    }

    private bool _refreshing;

    private async void OnScanTick(object? sender, EventArgs e)
    {
        if (_refreshing) return;

        if (_portList.Items.Count == 0)
        {
            await RunScanAsync();
        }
        else
        {
            // Refresh slave lists for HubFX boards already discovered
            await RefreshSlaveListsAsync();
        }
    }

    /// <summary>
    /// Queries slave lists for all detected HubFX boards and updates the
    /// display if the list has changed since the last query.
    /// </summary>
    private async Task RefreshSlaveListsAsync()
    {
        _refreshing = true;
        try
        {
            bool anyChanged = false;

            foreach (var item in _portList.Items.OfType<BoardListItem>().ToList())
            {
                var board = item.Board;
                if (board.BoardType != BoardType.HubFX ||
                    board.Connection?.IsConnected != true)
                    continue;

                try
                {
                    var slaves = await BoardDetector.QuerySlaveListAsync(
                        board.Connection, CancellationToken.None);
                    if (slaves == null) continue;

                    if (HasSlaveListChanged(board.Slaves, slaves))
                    {
                        board.Slaves.Clear();
                        board.Slaves.AddRange(slaves);
                        anyChanged = true;
                    }
                }
                catch { /* slave refresh failure is non-fatal */ }
            }

            if (anyChanged)
                RefreshBoardDisplay();
        }
        finally
        {
            _refreshing = false;
        }
    }

    private static bool HasSlaveListChanged(List<SlaveInfo> current, List<SlaveInfo> updated)
    {
        if (current.Count != updated.Count) return true;
        for (int i = 0; i < current.Count; i++)
        {
            if (current[i].Type != updated[i].Type ||
                current[i].Connected != updated[i].Connected ||
                current[i].Ready != updated[i].Ready)
                return true;
        }
        return false;
    }

    /// <summary>
    /// Forces the listbox to re-evaluate item display text and
    /// updates the board info panel for the currently selected board.
    /// </summary>
    private void RefreshBoardDisplay()
    {
        var selected = _portList.SelectedIndex;

        _portList.BeginUpdate();
        for (int i = 0; i < _portList.Items.Count; i++)
            _portList.Items[i] = _portList.Items[i]; // force ToString() refresh
        _portList.EndUpdate();

        if (selected >= 0) _portList.SelectedIndex = selected;

        if (_portList.SelectedItem is BoardListItem sel)
            ShowBoardInfo(sel.Board);
    }

    private void UpdateScanStatus(string port)
    {
        _statusLabel.Text = $"Probing {port}...";
    }

    // ─── Board Selection ───

    private void OnPortSelected(object? sender, EventArgs e)
    {
        if (_portList.SelectedItem is BoardListItem item)
        {
            _connectButton.Enabled = true;
            ShowBoardInfo(item.Board);
        }
        else
        {
            _connectButton.Enabled = false;
            _boardInfoPanel.Visible = false;
        }
    }

    private void ShowBoardInfo(BoardInfo board)
    {
        var lines = $"{board.DeviceName}  v{board.Version}  build {board.BuildNumber}  " +
                    $"{board.Platform}  {board.CpuMHz}MHz  {board.FreeRam / 1024}KB free";

        if (board.Slaves.Count > 0)
        {
            var connected = board.Slaves.Where(s => s.IsConnected).Select(s => s.TypeName).ToList();
            if (connected.Count > 0)
                lines += $"\nSlaves: {string.Join(", ", connected)}";
            else
                lines += "\nNo slaves connected";
        }

        _boardInfoLabel.Text = lines;
        _boardInfoPanel.Visible = true;
    }

    // ─── Actions ───

    private void OnConnect(object? sender, EventArgs e)
    {
        if (_portList.SelectedItem is BoardListItem item)
        {
            SelectedBoard = item.Board;
            DialogResult = DialogResult.OK;
            Close();
        }
    }

    private void OnSkip(object? sender, EventArgs e)
    {
        // Close connections from any detected-but-not-selected boards
        foreach (var item in _portList.Items.OfType<BoardListItem>())
        {
            if (item.Board != SelectedBoard)
            {
                item.Board.Connection?.Close();
                item.Board.Connection = null;
            }
        }

        SelectedBoard = null;
        DialogResult = DialogResult.Cancel;
        Close();
    }

    // ─── Animation ───

    private void OnAnimTick(object? sender, EventArgs e)
    {
        _spinnerFrame = (_spinnerFrame + 1) % SpinnerFrames.Length;
        _spinnerLabel.Text = SpinnerFrames[_spinnerFrame];
    }

    // ─── Helpers ───

    /// <summary>ListBox item wrapping a BoardInfo for display.</summary>
    private sealed class BoardListItem
    {
        public BoardInfo Board { get; }
        
        public BoardListItem(BoardInfo board) => Board = board;

        public override string ToString()
        {
            var name = Board.DisplayName;
            var extra = Board.BoardType == BoardType.HubFX && Board.Slaves.Count > 0
                ? $"  [{Board.Slaves.Count(s => s.IsConnected)} slave(s)]"
                : "";
            return $"  {Board.PortName}  \u2014  {name} v{Board.Version}{extra}";
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _animTimer.Dispose();
            _scanTimer.Dispose();
            _scanCts?.Cancel();
            _scanCts?.Dispose();
        }
        base.Dispose(disposing);
    }
}
