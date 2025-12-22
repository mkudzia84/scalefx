using ScaleFXStudio.Controls;

namespace ScaleFXStudio;

/// <summary>
/// Represents a sound file configuration panel (used for Start, Run, Stop sounds).
/// Encapsulates the enabled checkbox, file browser, and optional offset control.
/// </summary>
public class SoundPanel
{
    public string Name { get; }
    public GroupBox GroupBox { get; }
    public CheckBox EnabledCheckbox { get; }
    public TextBox FileTextBox { get; }
    public Button BrowseButton { get; }
    public NumericUpDown? OffsetInput { get; }
    public string? HelpText { get; }

    public bool Enabled
    {
        get => EnabledCheckbox.Checked;
        set => EnabledCheckbox.Checked = value;
    }

    public string FileName
    {
        get => FileTextBox.Text;
        set => FileTextBox.Text = value;
    }

    public int Offset
    {
        get => OffsetInput != null ? (int)OffsetInput.Value : 0;
        set { if (OffsetInput != null) OffsetInput.Value = value; }
    }

    public event EventHandler? ValueChanged;

    public SoundPanel(string name, string title, string enableText, string defaultFile,
        Font baseFont, int? defaultOffset = null, string? helpText = null)
    {
        Name = name;
        HelpText = helpText;

        GroupBox = ControlFactory.CreateGroupBox(title, 350);

        var flow = ControlFactory.CreateVerticalFlow();
        flow.Dock = DockStyle.Fill;
        flow.Padding = new Padding(5);

        // Enable checkbox
        EnabledCheckbox = new CheckBox
        {
            Text = enableText,
            AutoSize = true,
            Checked = true,
            Margin = new Padding(3, 3, 3, 5)
        };
        EnabledCheckbox.CheckedChanged += (s, e) =>
        {
            UpdateControlStates();
            ValueChanged?.Invoke(this, e);
        };
        flow.Controls.Add(EnabledCheckbox);

        // File browser row
        var (fileRow, textBox, browseBtn) = ControlFactory.CreateFileBrowserRow("File:", 60, defaultFile);
        FileTextBox = textBox;
        BrowseButton = browseBtn;
        FileTextBox.TextChanged += (s, e) => ValueChanged?.Invoke(this, e);
        BrowseButton.Click += ControlFactory.CreateBrowseSoundFileHandler(FileTextBox);
        flow.Controls.Add(fileRow);

        // Optional offset control
        if (defaultOffset.HasValue)
        {
            OffsetInput = ControlFactory.CreateNumericUpDown(0, 999999, defaultOffset.Value, 80);
            OffsetInput.ValueChanged += (s, e) => ValueChanged?.Invoke(this, e);
            var offsetRow = ControlFactory.CreateLabeledRow("Start From [ms]:", 100, OffsetInput);
            flow.Controls.Add(offsetRow);
        }

        // Optional help text
        if (!string.IsNullOrEmpty(helpText))
        {
            var helpLabel = ControlFactory.CreateHelpLabel(helpText, baseFont);
            flow.Controls.Add(helpLabel);
        }

        GroupBox.Controls.Add(flow);
    }

    public void UpdateControlStates()
    {
        var enabled = EnabledCheckbox.Checked;
        FileTextBox.Enabled = enabled;
        BrowseButton.Enabled = enabled;
        if (OffsetInput != null)
            OffsetInput.Enabled = enabled;
    }
}
