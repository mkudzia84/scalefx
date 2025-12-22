using ScaleFXStudio.Controls;

namespace ScaleFXStudio;

/// <summary>
/// Partial class containing Engine FX tab creation and related helper methods.
/// </summary>
public partial class MainForm
{
    private TabPage CreateEngineFxTab()
    {
        var tab = new TabPage("Engine FX");
        tab.Padding = new Padding(10);

        var scrollPanel = new Panel { Dock = DockStyle.Fill, AutoScroll = true };

        var mainFlow = new FlowLayoutPanel
        {
            Dock = DockStyle.Top,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Padding = new Padding(5)
        };

        // Enable checkbox at top
        _engineFxEnabled = new CheckBox
        {
            Text = "Enable Engine FX",
            AutoSize = true,
            Checked = true,
            Font = new Font(Font.FontFamily, 10, FontStyle.Bold),
            Margin = new Padding(5, 5, 5, 10)
        };
        _engineFxEnabled.CheckedChanged += (s, e) =>
        {
            _engineFxPanel.Visible = _engineFxEnabled.Checked;
            OnControlValueChanged(s, e);
        };

        mainFlow.Controls.Add(_engineFxEnabled);

        // Panel for Engine FX settings (collapsible) - using TableLayoutPanel for 2 columns
        _engineFxPanel = ControlFactory.CreateVerticalFlow();

        // Two-column grid
        var gridPanel = new TableLayoutPanel
        {
            ColumnCount = 2,
            RowCount = 1,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Margin = new Padding(0)
        };
        gridPanel.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        gridPanel.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

        // === LEFT COLUMN: Input Settings ===
        var leftColumn = ControlFactory.CreateVerticalFlow();
        leftColumn.Margin = new Padding(0, 0, 10, 0);
        leftColumn.Controls.Add(CreateEngineInputGroup());

        // === RIGHT COLUMN: Sound Settings ===
        var rightColumn = ControlFactory.CreateVerticalFlow();
        
        _startSoundPanel = new SoundPanel(
            "start", "Starting Sound", "Enable Starting Sound", "engine_start.wav",
            Font, defaultOffset: 60000, helpText: "Start point from interrupted Spool Down");
        _startSoundPanel.ValueChanged += OnControlValueChanged;
        rightColumn.Controls.Add(_startSoundPanel.GroupBox);

        _runSoundPanel = new SoundPanel(
            "run", "Running Sound", "Enable Running Sound (Loop)", "engine_loop.wav", Font);
        _runSoundPanel.ValueChanged += OnControlValueChanged;
        rightColumn.Controls.Add(_runSoundPanel.GroupBox);

        _stopSoundPanel = new SoundPanel(
            "stop", "Stopping Sound", "Enable Stopping Sound", "engine_stop.wav",
            Font, defaultOffset: 25000, helpText: "Start point from interrupted Spool Up");
        _stopSoundPanel.ValueChanged += OnControlValueChanged;
        rightColumn.Controls.Add(_stopSoundPanel.GroupBox);

        gridPanel.Controls.Add(leftColumn, 0, 0);
        gridPanel.Controls.Add(rightColumn, 1, 0);

        _engineFxPanel.Controls.Add(gridPanel);
        mainFlow.Controls.Add(_engineFxPanel);
        scrollPanel.Controls.Add(mainFlow);
        tab.Controls.Add(scrollPanel);
        return tab;
    }

    private GroupBox CreateEngineInputGroup()
    {
        var group = ControlFactory.CreateGroupBox("Input Settings");

        var flow = ControlFactory.CreateVerticalFlow();
        flow.Dock = DockStyle.Fill;
        flow.Padding = new Padding(5);

        // Engine Type
        _engineTypeCombo = new ComboBox
        {
            Width = 150,
            DropDownStyle = ComboBoxStyle.DropDownList
        };
        _engineTypeCombo.Items.AddRange(new object[] { "turbine", "radial", "diesel" });
        _engineTypeCombo.SelectedIndex = 0;
        _engineTypeCombo.SelectedIndexChanged += OnControlValueChanged;
        var typeRow = ControlFactory.CreateLabeledRow("Engine Type:", 120, _engineTypeCombo);
        flow.Controls.Add(typeRow);

        // Input Channel
        _enginePinInput = ControlFactory.CreateChannelComboBox(1);
        _enginePinInput.SelectedIndexChanged += OnControlValueChanged;
        var channelRow = ControlFactory.CreateLabeledRow("Input Channel:", 120, _enginePinInput);
        flow.Controls.Add(channelRow);
        RegisterControl("EngineFx.InputChannel", _enginePinInput);

        // Threshold
        var thresholdRow = ControlFactory.CreateHorizontalFlow();
        _engineThresholdEnabled = new CheckBox
        {
            Text = "Threshold [µs]:",
            AutoSize = true,
            Margin = new Padding(0, 3, 10, 0)
        };
        _engineThresholdInput = ControlFactory.CreateNumericUpDown(500, 2500, 1500, 100);
        _engineThresholdInput.Enabled = false;

        _engineThresholdEnabled.CheckedChanged += (s, e) =>
        {
            _engineThresholdInput.Enabled = _engineThresholdEnabled.Checked;
            OnControlValueChanged(s, e);
        };
        _engineThresholdInput.ValueChanged += OnControlValueChanged;

        thresholdRow.Controls.Add(_engineThresholdEnabled);
        thresholdRow.Controls.Add(_engineThresholdInput);
        flow.Controls.Add(thresholdRow);

        group.Controls.Add(flow);
        return group;
    }
}
