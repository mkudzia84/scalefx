using ScaleFXStudio.Models;
using ScaleFXStudio.Services;

namespace ScaleFXStudio;

/// <summary>
/// Main form for ScaleFX Studio configuration editor.
/// This partial class contains initialization and main UI shell setup.
/// 
/// The form is split across multiple files:
/// - MainForm.cs (this file): Constructor, InitializeComponent, SetupUI
/// - MainForm.Fields.cs: All field declarations
/// - MainForm.EngineFxTab.cs: Engine FX tab creation
/// - MainForm.GunFxTab.cs: Gun FX tab creation  
/// - MainForm.ConfigBinding.cs: Load/Save config to UI
/// - MainForm.FileOperations.cs: File menu handlers
/// </summary>
public partial class MainForm : Form
{
    public MainForm()
    {
        _configService = new ConfigService();
        _validationService = new ValidationService();
        _config = _configService.CreateDefault();
        
        // Initialize validation tooltip
        _validationToolTip = new ToolTip
        {
            AutoPopDelay = 10000,  // 10 seconds
            InitialDelay = 300,
            ReshowDelay = 200,
            ShowAlways = true
        };

        InitializeComponent();
        SetupUI();
        LoadConfigToUI();
        UpdateTitle();
        
        // Run initial validation after form is shown
        Shown += (s, e) => RunValidation();
    }

    private void InitializeComponent()
    {
        SuspendLayout();

        AutoScaleDimensions = new SizeF(7F, 15F);
        AutoScaleMode = AutoScaleMode.Font;
        ClientSize = new Size(1280, 800);
        MinimumSize = new Size(1280, 800);
        MaximumSize = new Size(1280, (Screen.PrimaryScreen?.Bounds.Height) ?? 1024);
        StartPosition = FormStartPosition.CenterScreen;

        ResumeLayout(false);
    }

    private void SetupUI()
    {
        // Menu Strip
        _menuStrip = new MenuStrip();

        var fileMenu = new ToolStripMenuItem("&File");
        fileMenu.DropDownItems.Add("&New", null, OnNew);
        fileMenu.DropDownItems.Add("&Open...", null, OnOpen);
        fileMenu.DropDownItems.Add("&Save", null, OnSave);
        fileMenu.DropDownItems.Add("Save &As...", null, OnSaveAs);
        fileMenu.DropDownItems.Add(new ToolStripSeparator());
        fileMenu.DropDownItems.Add("E&xit", null, (s, e) => Close());

        var editMenu = new ToolStripMenuItem("&Edit");
        editMenu.DropDownItems.Add("&Reset to Defaults", null, OnResetDefaults);

        var helpMenu = new ToolStripMenuItem("&Help");
        helpMenu.DropDownItems.Add("&About", null, OnAbout);

        _menuStrip.Items.AddRange(new[] { fileMenu, editMenu, helpMenu });

        // Logo Banner
        _logoBanner = new PictureBox
        {
            Dock = DockStyle.Top,
            Height = 80, // Default, will be updated to image height
            SizeMode = PictureBoxSizeMode.Normal,
            BackColor = Color.White,
            BorderStyle = BorderStyle.FixedSingle
        };

        // Load embedded logo
        try
        {
            var assembly = System.Reflection.Assembly.GetExecutingAssembly();
            using var stream = assembly.GetManifestResourceStream("ScaleFXStudio.Resources.logo.jpg");
            if (stream != null)
            {
                _logoBanner.Image = System.Drawing.Image.FromStream(stream);

                // Extract rightmost pixel color for banner background
                if (_logoBanner.Image is { Width: > 0, Height: > 0 })
                {
                    var bitmap = new System.Drawing.Bitmap(_logoBanner.Image);
                    int rightPixelX = bitmap.Width - 1;
                    int middlePixelY = bitmap.Height / 2;
                    Color rightmostColor = bitmap.GetPixel(rightPixelX, middlePixelY);
                    _logoBanner.BackColor = rightmostColor;
                    
                    // Set banner height to match image height
                    _logoBanner.Height = _logoBanner.Image.Height;
                }
            }
        }
        catch
        {
            // If logo fails to load, just show a blank banner
            _logoBanner.BackColor = Color.LightGray;
        }

        // Main Tab Control
        _mainTabControl = new TabControl
        {
            Dock = DockStyle.Fill
        };

        // Create tabs
        var engineFxTab = CreateEngineFxTab();
        var gunFxTab = CreateGunFxTab();

        _mainTabControl.TabPages.Add(engineFxTab);
        _mainTabControl.TabPages.Add(gunFxTab);

        // Bottom panel with Save button
        var bottomPanel = new Panel
        {
            Dock = DockStyle.Bottom,
            Height = 50,
            Padding = new Padding(10)
        };

        var saveButton = new Button
        {
            Text = "Save",
            Width = 100,
            Height = 35,
            Anchor = AnchorStyles.Right,
            Location = new Point(bottomPanel.Width - 110, 7),
            Font = new Font(Font.FontFamily, 10, FontStyle.Bold)
        };
        saveButton.Click += OnSave;
        bottomPanel.Controls.Add(saveButton);

        // Adjust button position on resize
        bottomPanel.Resize += (s, e) =>
        {
            saveButton.Location = new Point(bottomPanel.Width - 110, 7);
        };

        // Status Strip
        _statusStrip = new StatusStrip();
        _statusLabel = new ToolStripStatusLabel
        {
            Text = "Ready",
            Spring = true,
            TextAlign = ContentAlignment.MiddleLeft
        };
        _statusStrip.Items.Add(_statusLabel);

        // Layout (added in reverse order due to docking)
        Controls.Add(_mainTabControl);
        Controls.Add(bottomPanel);
        Controls.Add(_statusStrip);
        Controls.Add(_logoBanner);
        Controls.Add(_menuStrip);

        MainMenuStrip = _menuStrip;
    }
}
