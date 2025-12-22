using ScaleFXStudio.Controls;
using ScaleFXStudio.Models;

namespace ScaleFXStudio;

/// <summary>
/// Partial class containing Gun FX tab creation and related helper methods.
/// </summary>
public partial class MainForm
{
    private TabPage CreateGunFxTab()
    {
        var tab = new TabPage("Gun FX");
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
        _gunFxEnabled = new CheckBox
        {
            Text = "Enable Gun FX",
            AutoSize = true,
            Checked = true,
            Font = new Font(Font.FontFamily, 10, FontStyle.Bold),
            Margin = new Padding(5, 5, 5, 10)
        };
        _gunFxEnabled.CheckedChanged += (s, e) =>
        {
            _gunFxPanel.Visible = _gunFxEnabled.Checked;
            
            // When enabling Gun FX, ensure at least one rate of fire exists
            if (_gunFxEnabled.Checked && (_config.GunFx?.RateOfFire == null || _config.GunFx.RateOfFire.Count == 0))
            {
                _config.GunFx ??= new GunFxConfig();
                _config.GunFx.RateOfFire = new List<RateOfFireConfig>
                {
                    new() { Name = "200", Rpm = 200, PwmThresholdUs = 1400 }
                };
                RefreshRatesList();
                if (_ratesListBox.Items.Count > 0)
                {
                    _ratesListBox.SelectedIndex = 0;
                }
            }
            
            OnControlValueChanged(s, e);
        };

        mainFlow.Controls.Add(_gunFxEnabled);

        // Panel for Gun FX settings (collapsible)
        _gunFxPanel = ControlFactory.CreateVerticalFlow();

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

        // === LEFT COLUMN: Trigger, Smoke, Rate of Fire ===
        var leftColumn = ControlFactory.CreateVerticalFlow();
        leftColumn.Margin = new Padding(0, 0, 10, 0);
        leftColumn.Controls.Add(CreateTriggerGroup());
        leftColumn.Controls.Add(CreateSmokeGroup());
        leftColumn.Controls.Add(CreateRateOfFireGroup());

        // === RIGHT COLUMN: Turret Control ===
        var rightColumn = ControlFactory.CreateVerticalFlow();
        rightColumn.Controls.Add(CreateTurretGroup());

        gridPanel.Controls.Add(leftColumn, 0, 0);
        gridPanel.Controls.Add(rightColumn, 1, 0);

        _gunFxPanel.Controls.Add(gridPanel);
        mainFlow.Controls.Add(_gunFxPanel);
        scrollPanel.Controls.Add(mainFlow);
        tab.Controls.Add(scrollPanel);

        return tab;
    }

    private GroupBox CreateTriggerGroup()
    {
        var group = ControlFactory.CreateGroupBox("Trigger", 320);

        var flow = ControlFactory.CreateVerticalFlow();
        flow.Dock = DockStyle.Fill;
        flow.Padding = new Padding(5);

        _triggerChannelInput = ControlFactory.CreateChannelComboBox(2);
        _triggerChannelInput.SelectedIndexChanged += OnControlValueChanged;
        var channelRow = ControlFactory.CreateLabeledRow("Trigger Input Channel:", 160, _triggerChannelInput);
        flow.Controls.Add(channelRow);
        RegisterControl("GunFx.Trigger.InputChannel", _triggerChannelInput);

        group.Controls.Add(flow);
        return group;
    }

    private GroupBox CreateSmokeGroup()
    {
        var group = ControlFactory.CreateGroupBox("Smoke Generator", 320);

        var flow = ControlFactory.CreateVerticalFlow();
        flow.Dock = DockStyle.Fill;
        flow.Padding = new Padding(5);

        _smokeEnabled = new CheckBox
        {
            Text = "Enable Smoke Generator",
            AutoSize = true,
            Checked = true,
            Margin = new Padding(3, 3, 3, 5)
        };
        _smokeEnabled.CheckedChanged += (s, e) =>
        {
            _smokePanel.Visible = _smokeEnabled.Checked;
            UpdateSmokeControls();
            OnControlValueChanged(s, e);
        };
        flow.Controls.Add(_smokeEnabled);

        // Collapsible panel for smoke settings
        _smokePanel = ControlFactory.CreateVerticalFlow(10);

        _smokeChannelInput = ControlFactory.CreateChannelComboBox(3);
        _smokeChannelInput.SelectedIndexChanged += OnControlValueChanged;
        var channelRow = ControlFactory.CreateLabeledRow("Input Channel:", 140, _smokeChannelInput);
        RegisterControl("GunFx.Smoke.HeaterToggleChannel", _smokeChannelInput);

        _smokeThresholdInput = ControlFactory.CreateNumericUpDown(500, 2500, 1500);
        _smokeThresholdInput.ValueChanged += OnControlValueChanged;
        var thresholdRow = ControlFactory.CreateLabeledRow("Threshold [µs]:", 140, _smokeThresholdInput);

        _smokeFanDelayInput = ControlFactory.CreateNumericUpDown(0, 30000, 2000);
        _smokeFanDelayInput.ValueChanged += OnControlValueChanged;
        var fanDelayRow = ControlFactory.CreateLabeledRow("Fan Off Delay [ms]:", 140, _smokeFanDelayInput);

        _smokePanel.Controls.AddRange(new Control[] { channelRow, thresholdRow, fanDelayRow });
        flow.Controls.Add(_smokePanel);

        group.Controls.Add(flow);
        return group;
    }

    private GroupBox CreateRateOfFireGroup()
    {
        var group = ControlFactory.CreateGroupBox("Rate of Fire", 320);

        var mainFlow = ControlFactory.CreateVerticalFlow();
        mainFlow.Dock = DockStyle.Fill;
        mainFlow.Padding = new Padding(5);

        var descLabel = ControlFactory.CreateHelpLabel("Define Firing Rates and Trigger Input.", Font);
        descLabel.Margin = new Padding(0, 0, 0, 5);
        mainFlow.Controls.Add(descLabel);

        _ratesListBox = new ListBox
        {
            Size = new Size(290, 80),
            Margin = new Padding(0, 0, 0, 5)
        };
        _ratesListBox.SelectedIndexChanged += OnRateSelected;
        mainFlow.Controls.Add(_ratesListBox);
        RegisterControl("GunFx.RateOfFire", _ratesListBox);

        // RPM row
        _rateRpmInput = ControlFactory.CreateNumericUpDown(1, 6000, 200);
        _rateRpmInput.ValueChanged += OnControlValueChanged;
        RegisterControl("GunFx.RateOfFire.Rpm", _rateRpmInput);
        var rpmRow = ControlFactory.CreateLabeledRow("RPM:", 100, _rateRpmInput);
        mainFlow.Controls.Add(rpmRow);

        // Threshold row with slider and textbox
        var thresholdRow = ControlFactory.CreateHorizontalFlow();
        var thresholdLabel = new Label { Text = "Threshold [µs]:", AutoSize = true, Margin = new Padding(0, 8, 5, 0), Width = 95 };
        
        _rateThresholdSlider = new TrackBar
        {
            Width = 120,
            Minimum = 1000,
            Maximum = 2000,
            Value = 1400,
            TickFrequency = 100,
            LargeChange = 100,
            SmallChange = 10,
            Height = 30
        };
        
        _rateThresholdInput = new TextBox { Text = "1400", Width = 50, Margin = new Padding(5, 5, 0, 0) };
        RegisterControl("GunFx.RateOfFire.Threshold", _rateThresholdInput);

        _rateThresholdSlider.ValueChanged += (s, e) =>
        {
            _rateThresholdInput.Text = _rateThresholdSlider.Value.ToString();
            OnControlValueChanged(s, e);
        };

        _rateThresholdInput.TextChanged += (s, e) =>
        {
            if (int.TryParse(_rateThresholdInput.Text, out int val))
            {
                val = Math.Clamp(val, 1000, 2000);
                if (_rateThresholdSlider.Value != val)
                    _rateThresholdSlider.Value = val;
            }
        };
        
        // Validate on leave to ensure value is in range
        _rateThresholdInput.Leave += (s, e) =>
        {
            if (int.TryParse(_rateThresholdInput.Text, out int val))
            {
                val = Math.Clamp(val, 1000, 2000);
                _rateThresholdInput.Text = val.ToString();
            }
            else
            {
                _rateThresholdInput.Text = _rateThresholdSlider.Value.ToString();
            }
        };
        
        thresholdRow.Controls.Add(thresholdLabel);
        thresholdRow.Controls.Add(_rateThresholdSlider);
        thresholdRow.Controls.Add(_rateThresholdInput);
        mainFlow.Controls.Add(thresholdRow);

        // Sound file row
        var soundRow = ControlFactory.CreateHorizontalFlow();
        var soundLabel = new Label { Text = "Sound:", AutoSize = true, Margin = new Padding(0, 5, 5, 0), Width = 95 };
        _rateSoundFile = new TextBox { Width = 140 };
        var browseButton = new Button { Text = "...", Width = 30, Margin = new Padding(5, 0, 0, 0) };
        browseButton.Click += ControlFactory.CreateBrowseSoundFileHandler(_rateSoundFile);
        soundRow.Controls.AddRange(new Control[] { soundLabel, _rateSoundFile, browseButton });
        mainFlow.Controls.Add(soundRow);

        // Buttons row
        var buttonRow = ControlFactory.CreateHorizontalFlow(new Padding(5, 5, 5, 2));
        _rateAddButton = new Button { Text = "Add", Width = 60, Margin = new Padding(0, 0, 5, 0) };
        _rateAddButton.Click += OnRateAdd;
        _rateUpdateButton = new Button { Text = "Update", Width = 60, Margin = new Padding(0, 0, 5, 0) };
        _rateUpdateButton.Click += OnRateUpdate;
        _rateRemoveButton = new Button { Text = "Remove", Width = 60, Margin = new Padding(0, 0, 0, 0) };
        _rateRemoveButton.Click += OnRateRemove;
        buttonRow.Controls.AddRange(new Control[] { _rateAddButton, _rateUpdateButton, _rateRemoveButton });
        mainFlow.Controls.Add(buttonRow);

        group.Controls.Add(mainFlow);
        return group;
    }

    private GroupBox CreateTurretGroup()
    {
        var group = ControlFactory.CreateGroupBox("Turret Control", 380);

        var flow = ControlFactory.CreateVerticalFlow();
        flow.Dock = DockStyle.Fill;
        flow.Padding = new Padding(5);

        _turretEnabled = new CheckBox
        {
            Text = "Enable Turret Control",
            AutoSize = true,
            Checked = true,
            Font = new Font(Font, FontStyle.Bold),
            Margin = new Padding(3, 3, 3, 5)
        };
        _turretEnabled.CheckedChanged += (s, e) =>
        {
            _turretPanel.Visible = _turretEnabled.Checked;
            OnControlValueChanged(s, e);
        };
        flow.Controls.Add(_turretEnabled);

        // Turret panel (collapsible) - horizontal layout for pitch and yaw side by side
        _turretPanel = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Margin = new Padding(0)
        };

        // Create servo axis panels using the reusable component
        _pitchAxis = new ServoAxisPanel("Pitch", 1, 4, Font);
        _pitchAxis.ValueChanged += OnControlValueChanged;
        _pitchAxis.GroupBox.Margin = new Padding(0, 0, 5, 0);
        _turretPanel.Controls.Add(_pitchAxis.GroupBox);
        RegisterControl("GunFx.TurretControl.Pitch.InputChannel", _pitchAxis.ChannelInput);
        RegisterControl("GunFx.TurretControl.Pitch.ServoId", _pitchAxis.ServoIdInput);

        _yawAxis = new ServoAxisPanel("Yaw", 2, 5, Font);
        _yawAxis.ValueChanged += OnControlValueChanged;
        _turretPanel.Controls.Add(_yawAxis.GroupBox);
        RegisterControl("GunFx.TurretControl.Yaw.InputChannel", _yawAxis.ChannelInput);
        RegisterControl("GunFx.TurretControl.Yaw.ServoId", _yawAxis.ServoIdInput);

        flow.Controls.Add(_turretPanel);
        group.Controls.Add(flow);
        return group;
    }

    private void UpdateSmokeControls()
    {
        var enabled = _smokeEnabled.Checked;
        _smokeChannelInput.Enabled = enabled;
        _smokeThresholdInput.Enabled = enabled;
        _smokeFanDelayInput.Enabled = enabled;
    }

    private void OnRateSelected(object? sender, EventArgs e)
    {
        if (_ratesListBox.SelectedIndex >= 0 && _config.GunFx?.RateOfFire != null)
        {
            // Get the sorted list to match what's displayed
            var sortedRates = _config.GunFx.RateOfFire.OrderBy(r => r.PwmThresholdUs).ToList();
            if (_ratesListBox.SelectedIndex < sortedRates.Count)
            {
                var rate = sortedRates[_ratesListBox.SelectedIndex];
                _rateRpmInput.Value = rate.Rpm;
                _rateThresholdSlider.Value = Math.Clamp(rate.PwmThresholdUs, 1000, 2000);
                _rateThresholdInput.Text = _rateThresholdSlider.Value.ToString();
                _rateSoundFile.Text = rate.SoundFile != null ? Path.GetFileName(rate.SoundFile) : "";
            }
        }
    }

    private void OnRateAdd(object? sender, EventArgs e)
    {
        _config.GunFx ??= new GunFxConfig();
        _config.GunFx.RateOfFire ??= new List<RateOfFireConfig>();

        var newRate = new RateOfFireConfig
        {
            Name = $"{(int)_rateRpmInput.Value}",
            Rpm = (int)_rateRpmInput.Value,
            PwmThresholdUs = _rateThresholdSlider.Value,
            SoundFile = string.IsNullOrWhiteSpace(_rateSoundFile.Text) ? null : $"~/assets/{_rateSoundFile.Text}"
        };

        _config.GunFx.RateOfFire.Add(newRate);
        RefreshRatesList();
        _isDirty = true;
        UpdateTitle();
        RunValidation();
    }

    private void OnRateUpdate(object? sender, EventArgs e)
    {
        if (_ratesListBox.SelectedIndex >= 0 && _config.GunFx?.RateOfFire != null)
        {
            // Get the sorted list to find the correct rate
            var sortedRates = _config.GunFx.RateOfFire.OrderBy(r => r.PwmThresholdUs).ToList();
            if (_ratesListBox.SelectedIndex < sortedRates.Count)
            {
                var rate = sortedRates[_ratesListBox.SelectedIndex];
                
                // Update the rate values
                rate.Rpm = (int)_rateRpmInput.Value;
                rate.Name = $"{rate.Rpm}";
                rate.PwmThresholdUs = _rateThresholdSlider.Value;
                rate.SoundFile = string.IsNullOrWhiteSpace(_rateSoundFile.Text) ? null : $"~/assets/{_rateSoundFile.Text}";

                RefreshRatesList();
                _isDirty = true;
                UpdateTitle();
                RunValidation();
            }
        }
    }

    private void OnRateRemove(object? sender, EventArgs e)
    {
        if (_ratesListBox.SelectedIndex >= 0 && _config.GunFx?.RateOfFire != null)
        {
            if (_config.GunFx.RateOfFire.Count <= 1)
            {
                MessageBox.Show("At least one rate of fire is required.", "Cannot Remove",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            // Get the sorted list to find the correct rate to remove
            var sortedRates = _config.GunFx.RateOfFire.OrderBy(r => r.PwmThresholdUs).ToList();
            if (_ratesListBox.SelectedIndex < sortedRates.Count)
            {
                var rateToRemove = sortedRates[_ratesListBox.SelectedIndex];
                _config.GunFx.RateOfFire.Remove(rateToRemove);
                
                RefreshRatesList();
                _isDirty = true;
                UpdateTitle();
                RunValidation();
            }
        }
    }

    private void RefreshRatesList()
    {
        _ratesListBox.Items.Clear();
        if (_config.GunFx?.RateOfFire != null)
        {
            var sortedRates = _config.GunFx.RateOfFire.OrderBy(r => r.PwmThresholdUs).ToList();
            foreach (var rate in sortedRates)
            {
                _ratesListBox.Items.Add($"{rate.Rpm} RPM @ {rate.PwmThresholdUs}µs");
            }
        }
    }
}
