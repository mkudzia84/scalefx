using ScaleFXStudio.Models;

namespace ScaleFXStudio.Services;

/// <summary>
/// Validation severity levels
/// </summary>
public enum ValidationSeverity
{
    Info,
    Warning,
    Error
}

/// <summary>
/// Categories of validation issues for grouping
/// </summary>
public enum ValidationCategory
{
    InputChannel,
    RateOfFire,
    Servo,
    Sound,
    General
}

/// <summary>
/// Represents a single validation issue
/// </summary>
public class ValidationIssue
{
    public ValidationSeverity Severity { get; init; }
    public ValidationCategory Category { get; init; }
    public string Message { get; init; } = string.Empty;
    public string ControlKey { get; init; } = string.Empty; // Key to identify which control to highlight
    public object? Context { get; init; } // Additional context data

    public override string ToString() => $"[{Severity}] {Message}";
}

/// <summary>
/// Tracks usage of an input channel
/// </summary>
public class ChannelUsage
{
    public int Channel { get; init; }
    public string Function { get; init; } = string.Empty;
    public string ControlKey { get; init; } = string.Empty;
}

/// <summary>
/// Result of validation containing all issues found
/// </summary>
public class ValidationResult
{
    private readonly List<ValidationIssue> _issues = new();

    public IReadOnlyList<ValidationIssue> Issues => _issues;
    public IEnumerable<ValidationIssue> Errors => _issues.Where(i => i.Severity == ValidationSeverity.Error);
    public IEnumerable<ValidationIssue> Warnings => _issues.Where(i => i.Severity == ValidationSeverity.Warning);
    public IEnumerable<ValidationIssue> Infos => _issues.Where(i => i.Severity == ValidationSeverity.Info);

    public bool IsValid => !Errors.Any();
    public bool HasWarnings => Warnings.Any();
    public bool HasIssues => _issues.Any();

    public int ErrorCount => Errors.Count();
    public int WarningCount => Warnings.Count();

    public void AddIssue(ValidationIssue issue) => _issues.Add(issue);
    public void AddIssues(IEnumerable<ValidationIssue> issues) => _issues.AddRange(issues);

    public IEnumerable<ValidationIssue> GetIssuesForControl(string controlKey)
        => _issues.Where(i => i.ControlKey == controlKey);

    public IEnumerable<ValidationIssue> GetIssuesByCategory(ValidationCategory category)
        => _issues.Where(i => i.Category == category);

    public string GetStatusMessage()
    {
        if (!HasIssues) return "Configuration is valid";
        
        var parts = new List<string>();
        if (ErrorCount > 0) parts.Add($"{ErrorCount} error(s)");
        if (WarningCount > 0) parts.Add($"{WarningCount} warning(s)");
        return string.Join(", ", parts);
    }
}

/// <summary>
/// Service for validating ScaleFX configuration
/// </summary>
public class ValidationService
{
    private readonly List<ChannelUsage> _channelUsages = new();
    
    /// <summary>
    /// Validates the entire configuration
    /// </summary>
    public ValidationResult Validate(ScaleFXConfiguration config)
    {
        var result = new ValidationResult();
        _channelUsages.Clear();

        // Collect all channel usages first
        CollectChannelUsages(config);

        // Validate channels for conflicts
        result.AddIssues(ValidateChannelConflicts());

        // Validate Rate of Fire
        if (config.GunFx?.RateOfFire != null)
        {
            result.AddIssues(ValidateRateOfFire(config.GunFx.RateOfFire));
        }

        // Validate servo configurations
        if (config.GunFx?.TurretControl != null)
        {
            result.AddIssues(ValidateServos(config.GunFx.TurretControl));
        }

        return result;
    }

    /// <summary>
    /// Gets all channel usages collected during validation
    /// </summary>
    public IReadOnlyList<ChannelUsage> GetChannelUsages() => _channelUsages;

    /// <summary>
    /// Gets channel usage summary grouped by channel
    /// </summary>
    public Dictionary<int, List<ChannelUsage>> GetChannelUsageMap()
    {
        return _channelUsages
            .GroupBy(u => u.Channel)
            .ToDictionary(g => g.Key, g => g.ToList());
    }

    private void CollectChannelUsages(ScaleFXConfiguration config)
    {
        // Engine FX
        if (config.EngineFx?.EngineToggle != null)
        {
            _channelUsages.Add(new ChannelUsage
            {
                Channel = config.EngineFx.EngineToggle.InputChannel,
                Function = "Engine Toggle",
                ControlKey = "EngineFx.InputChannel"
            });
        }

        // Gun FX
        if (config.GunFx != null)
        {
            if (config.GunFx.Trigger != null)
            {
                _channelUsages.Add(new ChannelUsage
                {
                    Channel = config.GunFx.Trigger.InputChannel,
                    Function = "Trigger",
                    ControlKey = "GunFx.Trigger.InputChannel"
                });
            }

            if (config.GunFx.Smoke?.Enabled == true)
            {
                _channelUsages.Add(new ChannelUsage
                {
                    Channel = config.GunFx.Smoke.HeaterToggleChannel,
                    Function = "Smoke Heater",
                    ControlKey = "GunFx.Smoke.HeaterToggleChannel"
                });
            }

            if (config.GunFx.TurretControl?.Pitch != null)
            {
                _channelUsages.Add(new ChannelUsage
                {
                    Channel = config.GunFx.TurretControl.Pitch.InputChannel,
                    Function = "Turret Pitch",
                    ControlKey = "GunFx.TurretControl.Pitch.InputChannel"
                });
            }

            if (config.GunFx.TurretControl?.Yaw != null)
            {
                _channelUsages.Add(new ChannelUsage
                {
                    Channel = config.GunFx.TurretControl.Yaw.InputChannel,
                    Function = "Turret Yaw",
                    ControlKey = "GunFx.TurretControl.Yaw.InputChannel"
                });
            }
        }
    }

    private IEnumerable<ValidationIssue> ValidateChannelConflicts()
    {
        var issues = new List<ValidationIssue>();
        var channelMap = GetChannelUsageMap();

        foreach (var kvp in channelMap.Where(k => k.Value.Count > 1))
        {
            var functions = string.Join(", ", kvp.Value.Select(u => u.Function));
            
            // Create a warning for each control using this channel
            foreach (var usage in kvp.Value)
            {
                issues.Add(new ValidationIssue
                {
                    Severity = ValidationSeverity.Warning,
                    Category = ValidationCategory.InputChannel,
                    Message = $"Channel {kvp.Key} is used by multiple functions: {functions}",
                    ControlKey = usage.ControlKey,
                    Context = kvp.Value
                });
            }
        }

        return issues;
    }

    /// <summary>
    /// Validates Rate of Fire configuration
    /// </summary>
    public IEnumerable<ValidationIssue> ValidateRateOfFire(List<RateOfFireConfig> rates)
    {
        var issues = new List<ValidationIssue>();

        if (rates == null || rates.Count == 0)
            return issues;

        // Check for duplicate RPM values
        var rpmGroups = rates.GroupBy(r => r.Rpm).Where(g => g.Count() > 1);
        foreach (var group in rpmGroups)
        {
            foreach (var rate in group)
            {
                int index = rates.IndexOf(rate);
                issues.Add(new ValidationIssue
                {
                    Severity = ValidationSeverity.Error,
                    Category = ValidationCategory.RateOfFire,
                    Message = $"Duplicate RPM value: {rate.Rpm} RPM",
                    ControlKey = $"GunFx.RateOfFire[{index}].Rpm",
                    Context = rate
                });
            }
        }

        // Check for threshold spacing (must be at least 100µs apart)
        var sortedRates = rates.OrderBy(r => r.PwmThresholdUs).ToList();
        for (int i = 1; i < sortedRates.Count; i++)
        {
            int diff = sortedRates[i].PwmThresholdUs - sortedRates[i - 1].PwmThresholdUs;
            if (diff < 100)
            {
                int currentIndex = rates.IndexOf(sortedRates[i]);
                int prevIndex = rates.IndexOf(sortedRates[i - 1]);

                issues.Add(new ValidationIssue
                {
                    Severity = ValidationSeverity.Warning,
                    Category = ValidationCategory.RateOfFire,
                    Message = $"{sortedRates[i].Rpm} RPM: Threshold {sortedRates[i].PwmThresholdUs}µs is only {diff}µs from {sortedRates[i - 1].Rpm} RPM @ {sortedRates[i - 1].PwmThresholdUs}µs (minimum recommended: 100µs)",
                    ControlKey = $"GunFx.RateOfFire[{currentIndex}].PwmThresholdUs",
                    Context = new { Current = sortedRates[i], Previous = sortedRates[i - 1], Difference = diff }
                });

                // Also mark the previous one
                issues.Add(new ValidationIssue
                {
                    Severity = ValidationSeverity.Warning,
                    Category = ValidationCategory.RateOfFire,
                    Message = $"{sortedRates[i - 1].Rpm} RPM: Threshold {sortedRates[i - 1].PwmThresholdUs}µs is only {diff}µs from {sortedRates[i].Rpm} RPM @ {sortedRates[i].PwmThresholdUs}µs (minimum recommended: 100µs)",
                    ControlKey = $"GunFx.RateOfFire[{prevIndex}].PwmThresholdUs",
                    Context = new { Current = sortedRates[i - 1], Next = sortedRates[i], Difference = diff }
                });
            }
        }

        return issues;
    }

    /// <summary>
    /// Validates servo configurations
    /// </summary>
    public IEnumerable<ValidationIssue> ValidateServos(TurretControlConfig turretControl)
    {
        var issues = new List<ValidationIssue>();

        // Check for duplicate servo IDs
        var servoIds = new List<(int Id, string Axis, string ControlKey)>();
        
        if (turretControl.Pitch != null)
            servoIds.Add((turretControl.Pitch.ServoId, "Pitch", "GunFx.TurretControl.Pitch.ServoId"));
        
        if (turretControl.Yaw != null)
            servoIds.Add((turretControl.Yaw.ServoId, "Yaw", "GunFx.TurretControl.Yaw.ServoId"));

        var duplicates = servoIds.GroupBy(s => s.Id).Where(g => g.Count() > 1);
        foreach (var group in duplicates)
        {
            var axes = string.Join(", ", group.Select(s => s.Axis));
            foreach (var servo in group)
            {
                issues.Add(new ValidationIssue
                {
                    Severity = ValidationSeverity.Error,
                    Category = ValidationCategory.Servo,
                    Message = $"Servo ID {group.Key} is used by multiple axes: {axes}",
                    ControlKey = servo.ControlKey,
                    Context = group.Key
                });
            }
        }

        return issues;
    }
}
