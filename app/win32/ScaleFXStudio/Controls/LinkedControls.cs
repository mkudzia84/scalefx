namespace ScaleFXStudio.Controls;

/// <summary>
/// Represents a linked slider-textbox control pair where changes to one update the other.
/// Commonly used for motion parameters like speed, acceleration, etc.
/// </summary>
public class LinkedSliderControl
{
    public TrackBar Slider { get; }
    public TextBox TextBox { get; }
    public FlowLayoutPanel Container { get; }

    public int Value
    {
        get => Slider.Value;
        set
        {
            var clamped = Math.Clamp(value, Slider.Minimum, Slider.Maximum);
            Slider.Value = clamped;
            TextBox.Text = clamped.ToString();
        }
    }

    public event EventHandler? ValueChanged;

    public LinkedSliderControl(string labelText, int labelWidth, int min, int max, int defaultValue,
        int sliderWidth = 150, int textBoxWidth = 45, int tickFrequency = 1000)
    {
        Container = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Margin = new Padding(10, 0, 0, 2)
        };

        var label = new Label
        {
            Text = labelText,
            AutoSize = true,
            Margin = new Padding(0, 8, 5, 0),
            Width = labelWidth
        };

        Slider = new TrackBar
        {
            Width = sliderWidth,
            Minimum = min,
            Maximum = max,
            Value = Math.Clamp(defaultValue, min, max),
            TickFrequency = tickFrequency,
            LargeChange = tickFrequency / 2
        };

        TextBox = new TextBox
        {
            Text = defaultValue.ToString(),
            Width = textBoxWidth,
            Margin = new Padding(5, 5, 0, 0)
        };

        // Link slider -> textbox
        Slider.ValueChanged += (s, e) =>
        {
            TextBox.Text = Slider.Value.ToString();
            ValueChanged?.Invoke(this, e);
        };

        // Link textbox -> slider
        TextBox.TextChanged += (s, e) =>
        {
            if (int.TryParse(TextBox.Text, out int val))
            {
                var clamped = Math.Clamp(val, min, max);
                if (Slider.Value != clamped)
                {
                    Slider.Value = clamped;
                }
            }
        };

        Container.Controls.AddRange(new Control[] { label, Slider, TextBox });
    }
}

/// <summary>
/// Represents a Min/Max range input control pair.
/// </summary>
public class RangeInputControl
{
    public NumericUpDown MinInput { get; }
    public NumericUpDown MaxInput { get; }
    public FlowLayoutPanel Container { get; }

    public int MinValue
    {
        get => (int)MinInput.Value;
        set => MinInput.Value = value;
    }

    public int MaxValue
    {
        get => (int)MaxInput.Value;
        set => MaxInput.Value = value;
    }

    public event EventHandler? ValueChanged;

    public RangeInputControl(int min, int max, int defaultMin, int defaultMax, int inputWidth = 70)
    {
        Container = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Margin = new Padding(10, 0, 0, 2)
        };

        var minLabel = new Label { Text = "Min:", AutoSize = true, Margin = new Padding(0, 5, 5, 0) };
        MinInput = new NumericUpDown
        {
            Width = inputWidth,
            Minimum = min,
            Maximum = max,
            Value = Math.Clamp(defaultMin, min, max)
        };
        MinInput.ValueChanged += (s, e) => ValueChanged?.Invoke(this, e);

        var maxLabel = new Label { Text = "Max:", AutoSize = true, Margin = new Padding(15, 5, 5, 0) };
        MaxInput = new NumericUpDown
        {
            Width = inputWidth,
            Minimum = min,
            Maximum = max,
            Value = Math.Clamp(defaultMax, min, max)
        };
        MaxInput.ValueChanged += (s, e) => ValueChanged?.Invoke(this, e);

        Container.Controls.AddRange(new Control[] { minLabel, MinInput, maxLabel, MaxInput });
    }
}
