using ScaleFXStudio.Services;

namespace ScaleFXStudio;

/// <summary>
/// Partial class containing validation logic and UI highlighting
/// </summary>
public partial class MainForm
{
    private static readonly Color WarningBackColor = Color.FromArgb(255, 235, 200); // Light orange
    private static readonly Color ErrorBackColor = Color.FromArgb(255, 200, 200);   // Light red
    private static readonly Color NormalBackColor = SystemColors.Window;

    /// <summary>
    /// Registers a control for validation highlighting
    /// </summary>
    private void RegisterControl(string key, Control control)
    {
        _controlRegistry[key] = control;
    }

    /// <summary>
    /// Runs validation and updates UI
    /// </summary>
    private void RunValidation()
    {
        // Don't run validation if controls aren't ready yet
        if (_statusLabel == null || !IsHandleCreated) return;
        
        try
        {
            SaveUIToConfig();
            _lastValidationResult = _validationService.Validate(_config);
            ApplyValidationHighlighting();
            UpdateStatusFromValidation();
        }
        catch (Exception ex)
        {
            // Log but don't crash on validation errors during initialization
            System.Diagnostics.Debug.WriteLine($"Validation error: {ex.Message}");
        }
    }

    /// <summary>
    /// Applies visual highlighting to controls based on validation results
    /// </summary>
    private void ApplyValidationHighlighting()
    {
        // Reset all registered controls to normal state
        foreach (var control in _controlRegistry.Values)
        {
            ResetControlHighlight(control);
        }

        if (_lastValidationResult == null) return;

        // Apply highlights based on validation issues
        foreach (var issue in _lastValidationResult.Issues)
        {
            if (string.IsNullOrEmpty(issue.ControlKey)) continue;

            // Try to find exact match first
            if (_controlRegistry.TryGetValue(issue.ControlKey, out var control))
            {
                HighlightControl(control, issue.Severity, issue.Message);
            }
            else
            {
                // Strip array indices for matching: GunFx.RateOfFire[0].Rpm -> GunFx.RateOfFire.Rpm
                string normalizedKey = System.Text.RegularExpressions.Regex.Replace(issue.ControlKey, @"\[\d+\]", "");
                
                foreach (var kvp in _controlRegistry)
                {
                    // Check normalized key match, or partial match
                    if (kvp.Key == normalizedKey || 
                        normalizedKey.StartsWith(kvp.Key) || 
                        kvp.Key.StartsWith(normalizedKey))
                    {
                        HighlightControl(kvp.Value, issue.Severity, issue.Message);
                    }
                }
            }
        }

        // Highlight RoF grid rows
        HighlightRateOfFireRows();
    }

    /// <summary>
    /// Highlights rate of fire items in the ListBox
    /// </summary>
    private void HighlightRateOfFireRows()
    {
        if (_lastValidationResult == null || _ratesListBox == null) return;

        // ListBox doesn't support per-item background colors directly
        // But we can store the indices that have issues and use them elsewhere
        // For now, we'll change the overall ListBox background if any issues exist
        
        var rofIssues = _lastValidationResult.GetIssuesByCategory(ValidationCategory.RateOfFire);
        
        if (rofIssues.Any(i => i.Severity == ValidationSeverity.Error))
        {
            _ratesListBox.BackColor = ErrorBackColor;
        }
        else if (rofIssues.Any(i => i.Severity == ValidationSeverity.Warning))
        {
            _ratesListBox.BackColor = WarningBackColor;
        }
        else
        {
            _ratesListBox.BackColor = NormalBackColor;
        }
    }

    /// <summary>
    /// Highlights a control based on severity and sets tooltip
    /// </summary>
    private void HighlightControl(Control control, ValidationSeverity severity, string? tooltipText = null)
    {
        try
        {
            var color = severity switch
            {
                ValidationSeverity.Error => ErrorBackColor,
                ValidationSeverity.Warning => WarningBackColor,
                _ => NormalBackColor
            };

            if (control is TextBox || control is ComboBox || control is NumericUpDown || control is ListBox)
            {
                control.BackColor = color;
                
                // Also highlight the parent container for more visibility
                if (control.Parent != null)
                {
                    control.Parent.BackColor = color;
                }
            }
            else if (control is TrackBar trackBar)
            {
                // For trackbars, highlight the parent panel
                if (trackBar.Parent != null)
                {
                    trackBar.Parent.BackColor = color;
                }
            }
            else
            {
                control.BackColor = color;
                if (control.Parent != null)
                {
                    control.Parent.BackColor = color;
                }
            }

            // Set tooltip if provided
            if (!string.IsNullOrEmpty(tooltipText) && _validationToolTip != null)
            {
                _validationToolTip.SetToolTip(control, tooltipText);
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"HighlightControl error: {ex.Message}");
        }
    }

    /// <summary>
    /// Resets a control to its normal appearance and clears tooltip
    /// </summary>
    private void ResetControlHighlight(Control control)
    {
        try
        {
            if (control is TextBox || control is ComboBox || control is NumericUpDown || control is ListBox)
            {
                control.BackColor = NormalBackColor;
                
                // Reset parent container background
                if (control.Parent != null)
                {
                    control.Parent.BackColor = Color.Transparent;
                }
            }
            else if (control is TrackBar trackBar)
            {
                if (trackBar.Parent != null)
                {
                    trackBar.Parent.BackColor = Color.Transparent;
                }
            }
            else
            {
                control.BackColor = SystemColors.Control;
                if (control.Parent != null)
                {
                    control.Parent.BackColor = Color.Transparent;
                }
            }

            // Clear any validation tooltip
            if (_validationToolTip != null)
            {
                _validationToolTip.SetToolTip(control, null);
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"ResetControlHighlight error: {ex.Message}");
        }
    }

    /// <summary>
    /// Updates status bar based on validation results
    /// </summary>
    private void UpdateStatusFromValidation()
    {
        if (_lastValidationResult == null)
        {
            _statusLabel.Text = "Ready";
            _statusLabel.ForeColor = SystemColors.ControlText;
            return;
        }

        _statusLabel.Text = _lastValidationResult.GetStatusMessage();

        if (_lastValidationResult.ErrorCount > 0)
        {
            _statusLabel.ForeColor = Color.DarkRed;
        }
        else if (_lastValidationResult.WarningCount > 0)
        {
            _statusLabel.ForeColor = Color.DarkOrange;
        }
        else
        {
            _statusLabel.ForeColor = Color.DarkGreen;
        }
    }

    /// <summary>
    /// Gets validation issues for a specific control key
    /// </summary>
    private IEnumerable<ValidationIssue> GetIssuesForControl(string controlKey)
    {
        return _lastValidationResult?.GetIssuesForControl(controlKey) ?? Enumerable.Empty<ValidationIssue>();
    }

    /// <summary>
    /// Checks if there are validation errors (not just warnings)
    /// </summary>
    private bool HasValidationErrors()
    {
        return _lastValidationResult?.ErrorCount > 0;
    }
}
