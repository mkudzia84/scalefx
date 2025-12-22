namespace ScaleFXStudio;

/// <summary>
/// Partial class containing file operations (New, Open, Save, SaveAs) and menu handlers.
/// </summary>
public partial class MainForm
{
    private void UpdateTitle()
    {
        var fileName = _currentFilePath != null ? Path.GetFileName(_currentFilePath) : "Untitled";
        var dirty = _isDirty ? " *" : "";
        Text = $"ScaleFX Studio - {fileName}{dirty}";
    }

    private void OnControlValueChanged(object? sender, EventArgs e)
    {
        if (_isLoading) return;
        
        _isDirty = true;
        UpdateTitle();
        SaveUIToConfig();
        RunValidation();
    }

    private void OnNew(object? sender, EventArgs e)
    {
        if (!ConfirmDiscard()) return;

        _config = _configService.CreateDefault();
        _currentFilePath = null;
        _isDirty = false;
        LoadConfigToUI();
        UpdateTitle();
        RunValidation();
    }

    private void OnOpen(object? sender, EventArgs e)
    {
        if (!ConfirmDiscard()) return;

        using var dialog = new OpenFileDialog
        {
            Filter = "YAML Files (*.yaml;*.yml)|*.yaml;*.yml|All Files (*.*)|*.*",
            Title = "Open Configuration File"
        };

        if (dialog.ShowDialog() == DialogResult.OK)
        {
            try
            {
                _config = _configService.Load(dialog.FileName);
                _currentFilePath = dialog.FileName;
                _isDirty = false;
                LoadConfigToUI();
                UpdateTitle();
                RunValidation();
            }
            catch (Exception ex)
            {
                var detailedMessage = $"Error loading file:\n\n{ex.Message}\n\n";
                
                if (ex.InnerException != null)
                {
                    detailedMessage += $"Inner Exception:\n{ex.InnerException.Message}\n\n";
                }
                
                detailedMessage += $"Stack Trace:\n{ex.StackTrace}";
                
                // Write to console for easy copy/paste
                Console.WriteLine("=== DESERIALIZATION ERROR ===");
                Console.WriteLine(detailedMessage);
                Console.WriteLine("=============================");
                
                MessageBox.Show(detailedMessage, "Deserialization Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
    }

    private void OnSave(object? sender, EventArgs e)
    {
        if (_currentFilePath == null)
        {
            OnSaveAs(sender, e);
            return;
        }

        // Show summary before saving to existing file
        if (!ShowSaveSummary())
            return;

        SaveToFile(_currentFilePath);
    }

    private void OnSaveAs(object? sender, EventArgs e)
    {
        // Show summary before file dialog
        if (!ShowSaveSummary())
            return;

        using var dialog = new SaveFileDialog
        {
            Filter = "YAML Files (*.yaml)|*.yaml|All Files (*.*)|*.*",
            Title = "Save Configuration File",
            DefaultExt = "yaml",
            FileName = "config.yaml"
        };

        if (dialog.ShowDialog() == DialogResult.OK)
        {
            SaveToFile(dialog.FileName);
        }
    }

    private bool ShowSaveSummary()
    {
        try
        {
            SaveUIToConfig();
            RunValidation();
            bool hasErrors = HasValidationErrors();
            var summary = GenerateConfigSummary();
            using var summaryDialog = new Form
            {
                Text = "Configuration Summary",
                Width = 700,
                Height = 550,
                StartPosition = FormStartPosition.CenterParent,
                FormBorderStyle = FormBorderStyle.FixedDialog,
                MaximizeBox = false,
                MinimizeBox = false
            };

            var richTextBox = new RichTextBox
            {
                Multiline = true,
                ReadOnly = true,
                ScrollBars = RichTextBoxScrollBars.Vertical,
                Dock = DockStyle.Fill,
                Font = new Font("Consolas", 9),
                Rtf = summary,
                BackColor = Color.White,
                BorderStyle = BorderStyle.None
            };

            var buttonPanel = new FlowLayoutPanel
            {
                Dock = DockStyle.Bottom,
                FlowDirection = FlowDirection.RightToLeft,
                Height = 50,
                Padding = new Padding(10)
            };

            var saveButton = new Button
            {
                Text = "Save",
                Width = 80,
                Height = 30,
                DialogResult = DialogResult.OK,
                Enabled = !hasErrors
            };

            var cancelButton = new Button
            {
                Text = "Cancel",
                Width = 80,
                Height = 30,
                DialogResult = DialogResult.Cancel,
                Margin = new Padding(0, 0, 10, 0)
            };

            buttonPanel.Controls.Add(saveButton);
            buttonPanel.Controls.Add(cancelButton);
            summaryDialog.Controls.Add(richTextBox);
            summaryDialog.Controls.Add(buttonPanel);
            summaryDialog.AcceptButton = saveButton;
            summaryDialog.CancelButton = cancelButton;

            return summaryDialog.ShowDialog() == DialogResult.OK;
        }
        catch
        {
            return false;
        }
    }

    private void SaveToFile(string filePath)
    {
        try
        {
            _configService.Save(filePath, _config);
            _currentFilePath = filePath;
            _isDirty = false;
            UpdateTitle();
            MessageBox.Show($"Configuration saved successfully to {Path.GetFileName(filePath)}", "Save Successful", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Error saving file:\n{ex.Message}", "Error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private string GenerateConfigSummary()
    {
        var rtf = new System.Text.StringBuilder();
        // RTF header with color table: 1=Black, 2=Blue(headers), 3=Green(OK), 4=Orange(unused), 5=Red(errors/warnings), 6=Yellow, 7=Gray
        rtf.AppendLine(@"{\rtf1\ansi\ansicpg1252\deff0 {\fonttbl{\f0\fnil\fcharset0 Consolas;}}");
        rtf.AppendLine(@"{\colortbl;\red0\green0\blue0;\red0\green102\blue204;\red0\green128\blue0;\red255\green140\blue0;\red220\green20\blue60;\red180\green140\blue0;\red128\green128\blue128;}");
        rtf.AppendLine(@"{\*\expandedcolortbl;;\cscolortbl0 \cshtab;\cscolortbl0 \cshtab;\cscolortbl0 \cshtab;\cscolortbl0 \cshtab;\cscolortbl0 \cshtab;\cscolortbl0 \cshtab;}");
        rtf.AppendLine(@"\uc1\pard\f0\fs24");
        
        // Use validation service for data collection
        var validationResult = _validationService.Validate(_config);
        var channelUsageMap = _validationService.GetChannelUsageMap();

        // SECTION 1: Input Channels (at the top)
        AppendRtfText(rtf, "INPUT CHANNELS", 2, isBold: true);
        AppendRtfLine(rtf);
        if (channelUsageMap.Count > 0)
        {
            foreach (var ch in channelUsageMap.OrderBy(x => x.Key))
            {
                bool isConflicted = ch.Value.Count > 1;
                int colorIdx = isConflicted ? 5 : 3; // 5=Red for conflicts, 3=Green for OK
                string prefix = isConflicted ? "  \\u9888? " : "  \\u10003? "; // ⚠ and ✓ in RTF
                string functions = string.Join(", ", ch.Value.Select(u => u.Function));
                rtf.Append($"\\cf{colorIdx} {prefix}Channel {ch.Key}: {functions}");
                AppendRtfLine(rtf);
            }
        }
        else
        {
            AppendRtfText(rtf, "  No input channels configured", 7); // 7=Gray
            AppendRtfLine(rtf);
        }
        AppendRtfLine(rtf);

        // SECTION 2: Validation Issues (Errors and Warnings)
        if (validationResult.HasIssues)
        {
            if (validationResult.ErrorCount > 0)
            {
                AppendRtfText(rtf, "ERRORS", 5, isBold: true); // 5=Red bold
                AppendRtfLine(rtf);
                foreach (var error in validationResult.Errors)
                {
                    rtf.Append($"\\cf5   \\u10060? {error.Message}"); // ❌ in RTF, 5=Red
                    AppendRtfLine(rtf);
                }
                AppendRtfLine(rtf);
            }

            if (validationResult.WarningCount > 0)
            {
                AppendRtfText(rtf, "WARNINGS", 5, isBold: true); // 5=Red bold
                AppendRtfLine(rtf);
                foreach (var warning in validationResult.Warnings)
                {
                    rtf.Append($"\\cf5   \\u9888? {warning.Message}"); // ⚠ in RTF, 5=Red
                    AppendRtfLine(rtf);
                }
                AppendRtfLine(rtf);
            }
        }

        // SECTION 3: Engine FX Details
        if (_config.EngineFx != null)
        {
            AppendRtfText(rtf, "ENGINE FX", 2, isBold: true);
            AppendRtfLine(rtf);
            AppendRtfText(rtf, $"  Type: {_config.EngineFx.Type}", 3);
            AppendRtfLine(rtf);
            
            if (_config.EngineFx.EngineToggle != null)
            {
                rtf.Append($"\\cf3   Input: Channel {_config.EngineFx.EngineToggle.InputChannel} @ {_config.EngineFx.EngineToggle.ThresholdUs} \\u181?s"); // µ in RTF, 3=Green
                AppendRtfLine(rtf);
            }

            if (_config.EngineFx.Sounds != null)
            {
                bool hasAnySound = _config.EngineFx.Sounds.Starting != null || 
                                   _config.EngineFx.Sounds.Running != null || 
                                   _config.EngineFx.Sounds.Stopping != null;
                
                if (hasAnySound)
                {
                    AppendRtfText(rtf, "  Sounds:", 3, isBold: true); // 3=Green
                    AppendRtfLine(rtf);
                    if (_config.EngineFx.Sounds.Starting != null)
                    {
                        rtf.Append($"\\cf3     \\u8226? Starting: {_config.EngineFx.Sounds.Starting}"); // • in RTF, 3=Green
                        AppendRtfLine(rtf);
                    }
                    if (_config.EngineFx.Sounds.Running != null)
                    {
                        rtf.Append($"\\cf3     \\u8226? Running: {_config.EngineFx.Sounds.Running}"); // • in RTF, 3=Green
                        AppendRtfLine(rtf);
                    }
                    if (_config.EngineFx.Sounds.Stopping != null)
                    {
                        rtf.Append($"\\cf3     \\u8226? Stopping: {_config.EngineFx.Sounds.Stopping}"); // • in RTF, 3=Green
                        AppendRtfLine(rtf);
                    }
                }
            }
            AppendRtfLine(rtf);
        }

        // SECTION 4: Gun FX Details
        if (_config.GunFx != null)
        {
            AppendRtfText(rtf, "GUN FX", 2, isBold: true); // 2=Blue
            AppendRtfLine(rtf);

            // Trigger
            if (_config.GunFx.Trigger != null)
            {
                AppendRtfText(rtf, $"  Trigger: Channel {_config.GunFx.Trigger.InputChannel}", 3); // 3=Green
                AppendRtfLine(rtf);
            }

            // Smoke
            if (_config.GunFx.Smoke != null && _config.GunFx.Smoke.Enabled)
            {
                rtf.Append($"\\cf3   Smoke: Channel {_config.GunFx.Smoke.HeaterToggleChannel} @ {_config.GunFx.Smoke.HeaterPwmThresholdUs} \\u181?s (Fan delay: {_config.GunFx.Smoke.FanOffDelayMs} ms)"); // µ in RTF, 3=Green
                AppendRtfLine(rtf);
            }

            // Rate of Fire with sound files - using validation results
            if (_config.GunFx.RateOfFire != null && _config.GunFx.RateOfFire.Count > 0)
            {
                var sortedRates = _config.GunFx.RateOfFire.OrderBy(r => r.PwmThresholdUs).ToList();
                AppendRtfText(rtf, $"  Rate of Fire ({sortedRates.Count} rates):", 3, isBold: true); // 3=Green
                AppendRtfLine(rtf);

                // Get RoF validation issues
                var rofIssues = validationResult.GetIssuesByCategory(Services.ValidationCategory.RateOfFire);

                for (int i = 0; i < sortedRates.Count; i++)
                {
                    var rate = sortedRates[i];
                    int originalIndex = _config.GunFx.RateOfFire.IndexOf(rate);
                    
                    // Check if this rate has validation issues
                    bool hasError = rofIssues.Any(iss => 
                        iss.Severity == Services.ValidationSeverity.Error && 
                        iss.ControlKey.Contains($"[{originalIndex}]"));
                    bool hasWarning = rofIssues.Any(iss => 
                        iss.Severity == Services.ValidationSeverity.Warning && 
                        iss.ControlKey.Contains($"[{originalIndex}]"));
                    
                    int colorIdx = hasError ? 5 : (hasWarning ? 5 : 3); // 5=Red for error/warning, 3=Green for OK
                    string statusMark = hasError ? " \\u10060?" : (hasWarning ? " \\u9888?" : ""); // ❌ or ⚠
                    
                    string soundInfo = !string.IsNullOrEmpty(rate.SoundFile) 
                        ? $" \\u8594? {rate.SoundFile}" // → in RTF
                        : "";
                    
                    rtf.Append($"\\cf{colorIdx}     \\u8226? {rate.Rpm} RPM @ {rate.PwmThresholdUs} \\u181?s{soundInfo}{statusMark}"); // • and µ in RTF
                    AppendRtfLine(rtf);
                }
            }

            // Turret
            if (_config.GunFx.TurretControl != null)
            {
                var pitch = _config.GunFx.TurretControl.Pitch;
                var yaw = _config.GunFx.TurretControl.Yaw;
                
                if (pitch != null || yaw != null)
                {
                    AppendRtfText(rtf, "  Turret Control:", 3, isBold: true); // 3=Green
                    AppendRtfLine(rtf);

                    if (pitch != null)
                    {
                        rtf.Append($"\\cf3     \\u8226? Pitch: Ch {pitch.InputChannel} \\u8594? Servo {pitch.ServoId}"); // • and → in RTF, 3=Green
                        AppendRtfLine(rtf);
                    }

                    if (yaw != null)
                    {
                        rtf.Append($"\\cf3     \\u8226? Yaw: Ch {yaw.InputChannel} \\u8594? Servo {yaw.ServoId}"); // • and → in RTF, 3=Green
                        AppendRtfLine(rtf);
                    }
                }
            }
            AppendRtfLine(rtf);
        }

        AppendRtfText(rtf, "═══════════════════════════════════════════════════════════", 1);
        AppendRtfLine(rtf);
        
        rtf.AppendLine("}");
        return rtf.ToString();
    }

    private void AppendRtfText(System.Text.StringBuilder rtf, string text, int colorIndex, bool isBold = false)
    {
        // Escape special RTF characters
        text = text.Replace("\\", "\\\\").Replace("{", "\\{").Replace("}", "\\}");
        
        rtf.Append($"\\cf{colorIndex} ");
        if (isBold)
            rtf.Append("\\b ");
            
        rtf.Append(text);
        
        if (isBold)
            rtf.Append("\\b0");
    }

    private void AppendRtfLine(System.Text.StringBuilder rtf)
    {
        rtf.Append("\\line ");
    }

    private void OnResetDefaults(object? sender, EventArgs e)
    {
        if (MessageBox.Show("Reset all settings to defaults?", "Confirm Reset",
            MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
        {
            _config = _configService.CreateDefault();
            _isDirty = true;
            LoadConfigToUI();
            UpdateTitle();
        }
    }

    private void OnAbout(object? sender, EventArgs e)
    {
        MessageBox.Show(
            "ScaleFX Studio Configuration Editor\n\n" +
            "Version 1.0\n\n" +
            "GUI editor for ScaleFX Hub scale model effects system configuration.\n\n" +
            "© 2025 MSB (Marcin Scale Builds)",
            "About",
            MessageBoxButtons.OK,
            MessageBoxIcon.Information);
    }

    private bool ConfirmDiscard()
    {
        if (!_isDirty) return true;

        var result = MessageBox.Show(
            "You have unsaved changes. Do you want to save before continuing?",
            "Unsaved Changes",
            MessageBoxButtons.YesNoCancel,
            MessageBoxIcon.Warning);

        if (result == DialogResult.Yes)
        {
            OnSave(null, EventArgs.Empty);
            return !_isDirty; // Returns true if save succeeded
        }

        return result == DialogResult.No;
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (!ConfirmDiscard())
        {
            e.Cancel = true;
        }
        base.OnFormClosing(e);
    }
}
