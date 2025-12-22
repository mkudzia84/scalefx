namespace ScaleFXStudio.Controls;

/// <summary>
/// Factory class for creating common UI control patterns with consistent styling.
/// Eliminates boilerplate code for creating labeled rows, slider-textbox pairs, etc.
/// </summary>
public static class ControlFactory
{
    /// <summary>
    /// Creates a horizontal row with a label and control.
    /// </summary>
    public static FlowLayoutPanel CreateLabeledRow(string labelText, int labelWidth, Control control)
    {
        var row = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.LeftToRight,
            AutoSize = true,
            WrapContents = false,
            Margin = new Padding(5, 2, 5, 2)
        };
        var label = new Label
        {
            Text = labelText,
            AutoSize = true,
            Width = labelWidth,
            Margin = new Padding(0, 5, 10, 0)
        };
        row.Controls.Add(label);
        row.Controls.Add(control);
        return row;
    }

    /// <summary>
    /// Creates a ComboBox pre-populated with channel numbers 1-12.
    /// </summary>
    public static ComboBox CreateChannelComboBox(int defaultChannel = 1, int width = 80)
    {
        var combo = new ComboBox
        {
            Width = width,
            DropDownStyle = ComboBoxStyle.DropDownList,
            FlatStyle = FlatStyle.Popup  // Required for background color to show
        };
        for (int i = 1; i <= 12; i++)
            combo.Items.Add(i.ToString());
        combo.SelectedIndex = Math.Clamp(defaultChannel - 1, 0, 11);
        return combo;
    }

    /// <summary>
    /// Creates a standard NumericUpDown control.
    /// </summary>
    public static NumericUpDown CreateNumericUpDown(int min, int max, int value, int width = 80)
    {
        return new NumericUpDown
        {
            Width = width,
            Minimum = min,
            Maximum = max,
            Value = Math.Clamp(value, min, max)
        };
    }

    /// <summary>
    /// Creates a vertical FlowLayoutPanel with standard settings for grouping controls.
    /// </summary>
    public static FlowLayoutPanel CreateVerticalFlow(int leftMargin = 0)
    {
        return new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Margin = new Padding(leftMargin, 0, 0, 0)
        };
    }

    /// <summary>
    /// Creates a horizontal FlowLayoutPanel.
    /// </summary>
    public static FlowLayoutPanel CreateHorizontalFlow(Padding? margin = null)
    {
        return new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Margin = margin ?? new Padding(5, 2, 5, 2)
        };
    }

    /// <summary>
    /// Creates a GroupBox with auto-sizing and standard styling.
    /// </summary>
    public static GroupBox CreateGroupBox(string text, int minWidth = 300)
    {
        return new GroupBox
        {
            Text = text,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            MinimumSize = new Size(minWidth, 0),
            Margin = new Padding(5)
        };
    }

    /// <summary>
    /// Creates a section header label with bold styling.
    /// </summary>
    public static Label CreateSectionHeader(string text, Font baseFont)
    {
        return new Label
        {
            Text = text,
            AutoSize = true,
            Font = new Font(baseFont, FontStyle.Bold),
            Margin = new Padding(0, 10, 0, 3)
        };
    }

    /// <summary>
    /// Creates a help/hint label with gray italic styling.
    /// </summary>
    public static Label CreateHelpLabel(string text, Font baseFont)
    {
        return new Label
        {
            Text = text,
            AutoSize = true,
            ForeColor = Color.Gray,
            Font = new Font(baseFont, FontStyle.Italic),
            Margin = new Padding(5, 0, 5, 5)
        };
    }

    /// <summary>
    /// Creates a file browser row with TextBox and browse button.
    /// </summary>
    public static (FlowLayoutPanel Row, TextBox TextBox, Button BrowseButton) CreateFileBrowserRow(
        string labelText, int labelWidth, string defaultValue, int textBoxWidth = 200)
    {
        var row = CreateHorizontalFlow();
        var label = new Label 
        { 
            Text = labelText, 
            AutoSize = true, 
            Width = labelWidth, 
            Margin = new Padding(0, 5, 5, 0) 
        };
        var textBox = new TextBox { Width = textBoxWidth, Text = defaultValue };
        var browseBtn = new Button { Text = "...", Width = 30, Margin = new Padding(5, 0, 0, 0) };
        
        row.Controls.AddRange(new Control[] { label, textBox, browseBtn });
        return (row, textBox, browseBtn);
    }

    /// <summary>
    /// Creates a standard browse button click handler for sound files.
    /// </summary>
    public static EventHandler CreateBrowseSoundFileHandler(TextBox targetTextBox)
    {
        return (s, e) =>
        {
            using var dialog = new OpenFileDialog
            {
                Filter = "WAV Files (*.wav)|*.wav|All Files (*.*)|*.*",
                Title = "Select Sound File"
            };

            if (dialog.ShowDialog() == DialogResult.OK)
            {
                targetTextBox.Text = Path.GetFileName(dialog.FileName);
            }
        };
    }
}
