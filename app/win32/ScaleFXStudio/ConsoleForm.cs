using ScaleFX.Serial;
using ScaleFX.Serial.Console;
using ScaleFX.Serial.Console.Handlers;

namespace ScaleFXStudio;

/// <summary>
/// Interactive serial console window. Exposes the same protocol
/// commands as the CLI through a modular handler architecture.
/// Dark terminal theme with colorized output.
/// </summary>
public class ConsoleForm : Form
{
    private static readonly Color BgColor = Color.FromArgb(24, 24, 24);
    private static readonly Color FgColor = Color.FromArgb(204, 204, 204);
    private static readonly Color InputBg = Color.FromArgb(36, 36, 36);
    private static readonly Color StatusBg = Color.FromArgb(30, 30, 30);

    private readonly RichTextBox _output;
    private readonly TextBox _input;
    private readonly StatusStrip _statusStrip;
    private readonly ToolStripStatusLabel _statusLabel;

    private readonly CommandDispatcher _dispatcher;
    private readonly CommandContext _context;
    private readonly List<string> _history = [];
    private int _historyIndex = -1;

    public ConsoleForm(BoardInfo? board = null)
    {
        var consoleOutput = new RichTextBoxOutput(this);
        _context = new CommandContext(consoleOutput);
        _context.OnAsyncPacket += OnAsyncPacket;

        // If a board connection was provided, wire it into the context
        if (board?.Connection?.IsConnected == true)
        {
            _context.Connection = board.Connection;
            _context.Connection.OnAsyncPacket += _context.HandleAsyncPacket;
            _context.DetectedController = BoardDetector.GetControllerName(board.BoardType);
        }

        _dispatcher = new CommandDispatcher();
        _dispatcher.Register(new CoreHandler());
        _dispatcher.Register(new GunFxHandler());
        _dispatcher.Register(new LightFxHandler());
        _dispatcher.Register(new GearControlHandler());
        _dispatcher.Register(new HubFxHandler());

        // ─── Form Layout ───

        Text = "ScaleFX Console";
        Size = new Size(1000, 700);
        MinimumSize = new Size(600, 400);
        StartPosition = FormStartPosition.CenterScreen;
        KeyPreview = true;
        BackColor = BgColor;

        _output = new RichTextBox
        {
            Dock = DockStyle.Fill,
            ReadOnly = true,
            BackColor = BgColor,
            ForeColor = FgColor,
            Font = new Font("Consolas", 12F),
            BorderStyle = BorderStyle.None,
            WordWrap = true,
        };

        var inputPanel = new Panel
        {
            Dock = DockStyle.Bottom,
            Height = 34,
            Padding = new Padding(6, 4, 6, 4),
            BackColor = InputBg,
        };

        var promptLabel = new Label
        {
            Text = "> ",
            Dock = DockStyle.Left,
            ForeColor = Colors.Prompt,
            Font = new Font("Consolas", 12F),
            AutoSize = true,
            TextAlign = ContentAlignment.MiddleLeft,
            Padding = new Padding(2, 3, 0, 0),
        };

        _input = new TextBox
        {
            Dock = DockStyle.Fill,
            BackColor = InputBg,
            ForeColor = FgColor,
            Font = new Font("Consolas", 12F),
            BorderStyle = BorderStyle.None,
        };
        _input.KeyDown += OnInputKeyDown;

        inputPanel.Controls.Add(_input);
        inputPanel.Controls.Add(promptLabel);

        _statusStrip = new StatusStrip
        {
            BackColor = StatusBg,
            SizingGrip = false,
        };
        _statusStrip.Renderer = new DarkStatusStripRenderer();
        _statusLabel = new ToolStripStatusLabel
        {
            Text = "Disconnected",
            ForeColor = Colors.Dim,
            Spring = true,
            TextAlign = ContentAlignment.MiddleLeft,
        };
        _statusStrip.Items.Add(_statusLabel);

        Controls.Add(_output);
        Controls.Add(inputPanel);
        Controls.Add(_statusStrip);

        Shown += OnShown;
        FormClosing += OnFormClosing;
    }

    // ─── Lifecycle ───

    private void OnShown(object? sender, EventArgs e)
    {
        AppendColored("ScaleFX Console\n", Colors.Info);
        if (_context.IsConnected)
        {
            var ctrl = _context.DetectedController ?? "unknown";
            AppendColored($"Connected to {ctrl} on {_context.Connection!.PortName}\n", Colors.Success);
            AppendColored("Type 'help' for available commands.\n\n", Colors.Default);
        }
        else
        {
            AppendColored("Type 'help' for available commands, 'connect <port>' to connect.\n\n", Colors.Default);
        }
        _input.Focus();
        UpdateStatus();
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs e)
    {
        // Don't close the connection — MainForm owns it.
        // Just unhook the async packet handler so reader thread doesn't call disposed form.
        if (_context.Connection != null)
            _context.Connection.OnAsyncPacket -= _context.HandleAsyncPacket;
    }

    // ─── Input Handling ───

    private async void OnInputKeyDown(object? sender, KeyEventArgs e)
    {
        switch (e.KeyCode)
        {
            case Keys.Enter:
                e.SuppressKeyPress = true;
                var text = _input.Text.Trim();
                if (string.IsNullOrEmpty(text)) return;

                _history.Add(text);
                _historyIndex = _history.Count;
                _input.Clear();

                AppendColored($"> {text}\n", Colors.Prompt);

                if (text == "clear")
                {
                    _output.Clear();
                    return;
                }

                _input.Enabled = false;
                try
                {
                    await _dispatcher.ExecuteAsync(text, _context);
                }
                catch (Exception ex)
                {
                    AppendColored($"Error: {ex.Message}\n", Colors.Error);
                }
                finally
                {
                    _input.Enabled = true;
                    _input.Focus();
                    UpdateStatus();
                }
                break;

            case Keys.Up:
                e.SuppressKeyPress = true;
                if (_history.Count > 0 && _historyIndex > 0)
                {
                    _historyIndex--;
                    _input.Text = _history[_historyIndex];
                    _input.SelectionStart = _input.Text.Length;
                }
                break;

            case Keys.Down:
                e.SuppressKeyPress = true;
                if (_historyIndex < _history.Count - 1)
                {
                    _historyIndex++;
                    _input.Text = _history[_historyIndex];
                    _input.SelectionStart = _input.Text.Length;
                }
                else
                {
                    _historyIndex = _history.Count;
                    _input.Text = "";
                }
                break;

            case Keys.L when e.Control:
                e.SuppressKeyPress = true;
                _output.Clear();
                break;

            case Keys.Escape:
                e.SuppressKeyPress = true;
                _input.Clear();
                break;
        }
    }

    // ─── Async Packet Handler (reader thread → UI thread) ───

    private void OnAsyncPacket(Response resp)
    {
        if (IsDisposed) return;
        if (InvokeRequired) { BeginInvoke(() => OnAsyncPacket(resp)); return; }

        switch (resp.PacketType)
        {
            case PacketTypes.Core.LOG_MESSAGE:
                ResponseFormatters.FormatLogMessage(resp, _context.Output);
                break;
            case PacketTypes.Core.ACK:
                break; // ignore async ACKs (keepalive responses etc.)
            case PacketTypes.Core.NACK:
                AppendColored($"[async] NACK: {resp.ErrorMessage}\n", Colors.Error);
                break;
            case PacketTypes.GearControl.GEAR_SEQ_STATUS:
                ResponseFormatters.FormatGearSeqStatus(resp, _context.Output);
                break;
            case PacketTypes.GearControl.GEAR_DOOR_STATUS:
                ResponseFormatters.FormatGearDoorStatus(resp, _context.Output);
                break;
            case PacketTypes.GearControl.GEAR_CALIB_STATUS:
                ResponseFormatters.FormatGearCalibStatus(resp, _context.Output);
                break;
            case PacketTypes.LightFx.LANDING_LIGHT_STATUS:
                ResponseFormatters.FormatLandingLightStatus(resp, _context.Output);
                break;
            default:
                AppendColored($"[async] {PacketTypes.GetName(resp.PacketType)} [{resp.Payload.Length} bytes]\n",
                    Colors.Dim);
                break;
        }
    }

    // ─── Status Bar ───

    private void UpdateStatus()
    {
        if (_context.IsConnected)
        {
            var ctrl = _context.DetectedController ?? "unknown";
            _statusLabel.Text = $"Connected: {_context.Connection!.PortName} ({ctrl})";
            _statusLabel.ForeColor = Colors.Success;
        }
        else
        {
            _statusLabel.Text = "Disconnected";
            _statusLabel.ForeColor = Colors.Dim;
        }
    }

    // ─── RichTextBox Helpers ───

    private void AppendColored(string text, Color color)
    {
        if (IsDisposed) return;
        if (InvokeRequired) { BeginInvoke(() => AppendColored(text, color)); return; }

        _output.SelectionStart = _output.TextLength;
        _output.SelectionLength = 0;
        _output.SelectionColor = color;
        _output.AppendText(text);
        _output.ScrollToCaret();
    }

    // ─── Color Palette (dark terminal theme) ───

    internal static class Colors
    {
        // Base text
        public static readonly Color Default = Color.FromArgb(204, 204, 204);  // light gray
        public static readonly Color Dim     = Color.FromArgb(110, 110, 110);  // muted gray
        public static readonly Color Prompt  = Color.FromArgb(80, 180, 255);   // bright blue

        // Semantic
        public static readonly Color Success = Color.FromArgb(78, 201, 110);   // green
        public static readonly Color Error   = Color.FromArgb(255, 85, 85);    // red
        public static readonly Color Warning = Color.FromArgb(230, 180, 50);   // amber

        // Info / headings
        public static readonly Color Info    = Color.FromArgb(86, 156, 214);   // VS Code blue
        public static readonly Color Heading = Color.FromArgb(198, 120, 221);  // purple

        // Data formatting
        public static readonly Color Label   = Color.FromArgb(150, 150, 150);  // dim label
        public static readonly Color Value   = Color.FromArgb(206, 145, 80);   // orange-brown

        // Command-specific colors
        public static readonly Color Core    = Color.FromArgb(86, 156, 214);   // blue
        public static readonly Color GunFx   = Color.FromArgb(255, 85, 85);    // red
        public static readonly Color LightFx = Color.FromArgb(230, 180, 50);   // amber
        public static readonly Color Gear    = Color.FromArgb(78, 201, 110);   // green
        public static readonly Color HubFx   = Color.FromArgb(198, 120, 221);  // purple
        public static readonly Color Audio   = Color.FromArgb(0, 188, 212);    // cyan
    }

    /// <summary>
    /// Returns a group-specific color for help headings and command echo.
    /// </summary>
    internal static Color GetGroupColor(string groupName)
    {
        return groupName.ToLowerInvariant() switch
        {
            "core" => Colors.Core,
            "gunfx" => Colors.GunFx,
            "lightfx" => Colors.LightFx,
            "gearcontrol" => Colors.Gear,
            "hubfx" => Colors.HubFx,
            _ => Colors.Info
        };
    }

    // ─── IConsoleOutput Implementation ───

    /// <summary>
    /// Routes handler output to the RichTextBox with colored formatting.
    /// Thread-safe via BeginInvoke.
    /// </summary>
    private sealed class RichTextBoxOutput(ConsoleForm form) : IConsoleOutput
    {
        public void Write(string text) => form.AppendColored(text, Colors.Default);
        public void WriteLine(string text = "") => form.AppendColored(text + "\n", Colors.Default);
        public void WriteSuccess(string text) => form.AppendColored(text + "\n", Colors.Success);
        public void WriteError(string text) => form.AppendColored(text + "\n", Colors.Error);
        public void WriteWarning(string text) => form.AppendColored(text + "\n", Colors.Warning);
        public void WriteInfo(string text) => form.AppendColored(text + "\n", Colors.Info);

        public void WriteHeading(string text, string? group = null)
        {
            var color = group != null ? GetGroupColor(group) : Colors.Info;
            form.AppendColored(text + "\n", color);
        }

        public void WriteData(string label, string value)
        {
            int pad = Math.Max(1, 22 - label.Length);
            form.AppendColored($"  {label}{new string(' ', pad)}", Colors.Label);
            form.AppendColored(value + "\n", Colors.Value);
        }
    }

    // ─── Dark StatusStrip Renderer ───

    private sealed class DarkStatusStripRenderer : ToolStripProfessionalRenderer
    {
        protected override void OnRenderToolStripBackground(ToolStripRenderEventArgs e)
        {
            using var brush = new SolidBrush(StatusBg);
            e.Graphics.FillRectangle(brush, e.AffectedBounds);
        }

        protected override void OnRenderToolStripBorder(ToolStripRenderEventArgs e) { }
    }
}
