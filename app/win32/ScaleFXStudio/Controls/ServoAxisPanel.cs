using ScaleFXStudio.Controls;

namespace ScaleFXStudio;

/// <summary>
/// Represents a servo axis configuration panel (used for Pitch and Yaw).
/// Encapsulates all controls for a single servo axis.
/// </summary>
public class ServoAxisPanel
{
    public string Name { get; }
    public GroupBox GroupBox { get; }
    public CheckBox EnabledCheckbox { get; }
    public FlowLayoutPanel SettingsPanel { get; }
    public ComboBox ChannelInput { get; }
    public NumericUpDown ServoIdInput { get; }
    public RangeInputControl InputRange { get; }
    public RangeInputControl OutputRange { get; }
    public LinkedSliderControl SpeedControl { get; }
    public LinkedSliderControl AccelControl { get; }
    public LinkedSliderControl DecelControl { get; }
    public LinkedSliderControl RecoilJerkControl { get; }
    public LinkedSliderControl RecoilVarianceControl { get; }

    public bool Enabled
    {
        get => EnabledCheckbox.Checked;
        set => EnabledCheckbox.Checked = value;
    }

    public int Channel
    {
        get => ChannelInput.SelectedIndex + 1;
        set => ChannelInput.SelectedIndex = Math.Clamp(value - 1, 0, 11);
    }

    public int ServoId
    {
        get => (int)ServoIdInput.Value;
        set => ServoIdInput.Value = Math.Clamp(value, 1, 3);
    }

    public event EventHandler? ValueChanged;

    public ServoAxisPanel(string name, int defaultServoId, int defaultChannel, Font baseFont)
    {
        Name = name;

        GroupBox = new GroupBox
        {
            Text = name,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            MinimumSize = new Size(350, 0)
        };

        var flow = ControlFactory.CreateVerticalFlow();
        flow.Dock = DockStyle.Fill;
        flow.Padding = new Padding(5);

        // Enable checkbox
        EnabledCheckbox = new CheckBox
        {
            Text = $"Enable {name}",
            AutoSize = true,
            Checked = true,
            Margin = new Padding(3, 3, 3, 5)
        };

        // Settings panel (collapsible)
        SettingsPanel = ControlFactory.CreateVerticalFlow(10);

        EnabledCheckbox.CheckedChanged += (s, e) =>
        {
            SettingsPanel.Visible = EnabledCheckbox.Checked;
            ValueChanged?.Invoke(this, e);
        };

        // Input Channel
        ChannelInput = ControlFactory.CreateChannelComboBox(defaultChannel, 60);
        ChannelInput.SelectedIndexChanged += (s, e) => ValueChanged?.Invoke(this, e);
        var channelRow = ControlFactory.CreateLabeledRow("Input Channel:", 100, ChannelInput);
        SettingsPanel.Controls.Add(channelRow);

        // Servo ID
        ServoIdInput = ControlFactory.CreateNumericUpDown(1, 3, defaultServoId, 60);
        ServoIdInput.ValueChanged += (s, e) => ValueChanged?.Invoke(this, e);
        var servoIdRow = ControlFactory.CreateLabeledRow("Servo ID:", 100, ServoIdInput);
        SettingsPanel.Controls.Add(servoIdRow);

        // Input Signal section
        var inputHeader = ControlFactory.CreateSectionHeader("Input Signal [µs]:", baseFont);
        SettingsPanel.Controls.Add(inputHeader);

        InputRange = new RangeInputControl(500, 2500, 1000, 2000);
        InputRange.ValueChanged += (s, e) => ValueChanged?.Invoke(this, e);
        SettingsPanel.Controls.Add(InputRange.Container);

        // Output Signal section
        var outputHeader = ControlFactory.CreateSectionHeader("Servo Range [µs]:", baseFont);
        SettingsPanel.Controls.Add(outputHeader);

        OutputRange = new RangeInputControl(500, 2500, 1000, 2000);
        OutputRange.ValueChanged += (s, e) => ValueChanged?.Invoke(this, e);
        SettingsPanel.Controls.Add(OutputRange.Container);

        // Motion Parameters section
        var motionHeader = ControlFactory.CreateSectionHeader("Motion Parameters:", baseFont);
        SettingsPanel.Controls.Add(motionHeader);

        SpeedControl = new LinkedSliderControl("Speed [µs/s]:", 80, 100, 10000, 4000);
        SpeedControl.ValueChanged += (s, e) => ValueChanged?.Invoke(this, e);
        SettingsPanel.Controls.Add(SpeedControl.Container);

        AccelControl = new LinkedSliderControl("Accel [µs/s²]:", 80, 100, 10000, 8000);
        AccelControl.ValueChanged += (s, e) => ValueChanged?.Invoke(this, e);
        SettingsPanel.Controls.Add(AccelControl.Container);

        DecelControl = new LinkedSliderControl("Decel [µs/s²]:", 80, 100, 10000, 8000);
        DecelControl.ValueChanged += (s, e) => ValueChanged?.Invoke(this, e);
        SettingsPanel.Controls.Add(DecelControl.Container);

        // Recoil Jerk section
        var recoilHeader = ControlFactory.CreateSectionHeader("Recoil Jerk (optional):", baseFont);
        SettingsPanel.Controls.Add(recoilHeader);

        RecoilJerkControl = new LinkedSliderControl("Jerk [µs]:", 80, 0, 200, 0);
        RecoilJerkControl.ValueChanged += (s, e) =>
        {
            // Ensure variance doesn't exceed jerk value
            if (RecoilVarianceControl.Value > RecoilJerkControl.Value)
            {
                RecoilVarianceControl.Value = RecoilJerkControl.Value;
            }
            ValueChanged?.Invoke(this, e);
        };
        SettingsPanel.Controls.Add(RecoilJerkControl.Container);

        RecoilVarianceControl = new LinkedSliderControl("Variance [µs]:", 80, 0, 100, 0);
        RecoilVarianceControl.ValueChanged += (s, e) =>
        {
            // Clamp variance to not exceed jerk value
            if (RecoilVarianceControl.Value > RecoilJerkControl.Value)
            {
                RecoilVarianceControl.Value = RecoilJerkControl.Value;
            }
            ValueChanged?.Invoke(this, e);
        };
        SettingsPanel.Controls.Add(RecoilVarianceControl.Container);

        flow.Controls.Add(EnabledCheckbox);
        flow.Controls.Add(SettingsPanel);
        GroupBox.Controls.Add(flow);
    }
}
