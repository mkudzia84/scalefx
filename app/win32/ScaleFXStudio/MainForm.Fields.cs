using ScaleFXStudio.Models;
using ScaleFXStudio.Services;

namespace ScaleFXStudio;

/// <summary>
/// Partial class containing all field declarations for MainForm.
/// Separates field definitions from UI construction logic.
/// </summary>
public partial class MainForm
{
    // Services
    private readonly ConfigService _configService;
    private readonly ValidationService _validationService;
    private ScaleFXConfiguration _config;
    private string? _currentFilePath;
    private bool _isDirty;
    private bool _isLoading;
    private ValidationResult? _lastValidationResult;

    // Shell controls
    private MenuStrip _menuStrip = null!;
    private PictureBox _logoBanner = null!;
    private TabControl _mainTabControl = null!;
    private StatusStrip _statusStrip = null!;
    private ToolStripStatusLabel _statusLabel = null!;
    private ToolTip _validationToolTip = null!;

    // Control registry for validation highlighting
    private readonly Dictionary<string, Control> _controlRegistry = new();

    // === Engine FX Controls ===
    private CheckBox _engineFxEnabled = null!;
    private FlowLayoutPanel _engineFxPanel = null!;
    
    // Engine Input
    private ComboBox _engineTypeCombo = null!;
    private ComboBox _enginePinInput = null!;
    private CheckBox _engineThresholdEnabled = null!;
    private NumericUpDown _engineThresholdInput = null!;
    
    // Engine Sound Panels (using reusable SoundPanel component)
    private SoundPanel _startSoundPanel = null!;
    private SoundPanel _runSoundPanel = null!;
    private SoundPanel _stopSoundPanel = null!;

    // === Gun FX Controls ===
    private CheckBox _gunFxEnabled = null!;
    private FlowLayoutPanel _gunFxPanel = null!;
    
    // Trigger
    private ComboBox _triggerChannelInput = null!;
    
    // Smoke
    private CheckBox _smokeEnabled = null!;
    private FlowLayoutPanel _smokePanel = null!;
    private ComboBox _smokeChannelInput = null!;
    private NumericUpDown _smokeThresholdInput = null!;
    private NumericUpDown _smokeFanDelayInput = null!;
    
    // Rate of Fire
    private ListBox _ratesListBox = null!;
    private NumericUpDown _rateRpmInput = null!;
    private TrackBar _rateThresholdSlider = null!;
    private TextBox _rateThresholdInput = null!;
    private TextBox _rateSoundFile = null!;
    private Button _rateAddButton = null!;
    private Button _rateRemoveButton = null!;
    private Button _rateUpdateButton = null!;
    
    // Turret (using reusable ServoAxisPanel components)
    private CheckBox _turretEnabled = null!;
    private FlowLayoutPanel _turretPanel = null!;
    private ServoAxisPanel _pitchAxis = null!;
    private ServoAxisPanel _yawAxis = null!;
}
