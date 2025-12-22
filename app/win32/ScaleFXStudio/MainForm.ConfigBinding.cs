using ScaleFXStudio.Models;

namespace ScaleFXStudio;

/// <summary>
/// Partial class containing config-to-UI binding logic (Load and Save).
/// </summary>
public partial class MainForm
{
    private void LoadConfigToUI()
    {
        _isLoading = true;
        try
        {
            // Engine FX - enabled if section exists with valid input_channel
            var engineHasContent = _config.EngineFx != null && 
                ((_config.EngineFx.EngineToggle?.InputChannel ?? 0) > 0 ||
                 !string.IsNullOrWhiteSpace(_config.EngineFx.Sounds?.Starting) ||
                 !string.IsNullOrWhiteSpace(_config.EngineFx.Sounds?.Running) ||
                 !string.IsNullOrWhiteSpace(_config.EngineFx.Sounds?.Stopping));
            _engineFxEnabled.Checked = engineHasContent;
            _engineFxPanel.Visible = _engineFxEnabled.Checked;

            // Engine Type
        if (!string.IsNullOrEmpty(_config.EngineFx?.Type))
        {
            var index = _engineTypeCombo.Items.IndexOf(_config.EngineFx.Type);
            _engineTypeCombo.SelectedIndex = index >= 0 ? index : 0;
        }
        else
        {
            _engineTypeCombo.SelectedIndex = 0; // Default to turbine
        }

        if (_config.EngineFx?.EngineToggle != null)
        {
            // Channel 0 means unassigned, select first item (channel 1) as placeholder
            var channel = _config.EngineFx.EngineToggle.InputChannel;
            _enginePinInput.SelectedIndex = channel > 0 ? Math.Clamp(channel - 1, 0, 11) : 0;

            if (_config.EngineFx.EngineToggle.ThresholdUs != 1500)
            {
                _engineThresholdEnabled.Checked = true;
                _engineThresholdInput.Value = _config.EngineFx.EngineToggle.ThresholdUs;
            }
            else
            {
                _engineThresholdEnabled.Checked = false;
                _engineThresholdInput.Value = 1500;
            }
        }

        // Engine Sounds - using SoundPanel components
        LoadEngineSoundsToUI();

        // Gun FX - enabled if section exists with valid trigger channel or other content
        var gunHasContent = _config.GunFx != null &&
            ((_config.GunFx.Trigger?.InputChannel ?? 0) > 0 ||
             (_config.GunFx.RateOfFire?.Count ?? 0) > 0 ||
             (_config.GunFx.TurretControl?.Pitch?.InputChannel ?? 0) > 0 ||
             (_config.GunFx.TurretControl?.Yaw?.InputChannel ?? 0) > 0 ||
             (_config.GunFx.Smoke?.HeaterToggleChannel ?? 0) > 0);
        _gunFxEnabled.Checked = gunHasContent;
        _gunFxPanel.Visible = _gunFxEnabled.Checked;

        if (_config.GunFx != null)
        {
            LoadGunFxToUI();
        }
        else
        {
            _turretEnabled.Checked = false;
            _turretPanel.Visible = false;
            _pitchAxis.Enabled = false;
            _pitchAxis.SettingsPanel.Visible = false;
            _yawAxis.Enabled = false;
            _yawAxis.SettingsPanel.Visible = false;
        }

        _isDirty = false;
        }
        finally
        {
            _isLoading = false;
        }
    }

    private void LoadEngineSoundsToUI()
    {
        if (_config.EngineFx?.Sounds != null)
        {
            var sounds = _config.EngineFx.Sounds;

            // Starting sound - enabled if file is not null/empty
            var startFile = sounds.Starting;
            _startSoundPanel.Enabled = !string.IsNullOrWhiteSpace(startFile);
            _startSoundPanel.FileName = _startSoundPanel.Enabled ? startFile! : "";
            
            if (sounds.Transitions != null)
            {
                _startSoundPanel.Offset = sounds.Transitions.StartingOffsetMs;
                _stopSoundPanel.Offset = sounds.Transitions.StoppingOffsetMs;
            }

            // Running sound - enabled if file is not null/empty
            var runFile = sounds.Running;
            _runSoundPanel.Enabled = !string.IsNullOrWhiteSpace(runFile);
            _runSoundPanel.FileName = _runSoundPanel.Enabled ? runFile! : "";

            // Stopping sound - enabled if file is not null/empty
            var stopFile = sounds.Stopping;
            _stopSoundPanel.Enabled = !string.IsNullOrWhiteSpace(stopFile);
            _stopSoundPanel.FileName = _stopSoundPanel.Enabled ? stopFile! : "";
        }
        else
        {
            // No sounds config - disable all
            _startSoundPanel.Enabled = false;
            _startSoundPanel.FileName = "";
            _runSoundPanel.Enabled = false;
            _runSoundPanel.FileName = "";
            _stopSoundPanel.Enabled = false;
            _stopSoundPanel.FileName = "";
        }

        _startSoundPanel.UpdateControlStates();
        _runSoundPanel.UpdateControlStates();
        _stopSoundPanel.UpdateControlStates();
    }

    private void LoadGunFxToUI()
    {
        // Trigger
        if (_config.GunFx?.Trigger != null)
        {
            var channel = _config.GunFx.Trigger.InputChannel;
            _triggerChannelInput.SelectedIndex = channel > 0 ? Math.Clamp(channel - 1, 0, 11) : 0;
        }

        // Smoke - enabled if section exists with valid channel
        if (_config.GunFx?.Smoke != null && _config.GunFx.Smoke.HeaterToggleChannel > 0)
        {
            _smokeEnabled.Checked = true;
            var channel = _config.GunFx.Smoke.HeaterToggleChannel;
            _smokeChannelInput.SelectedIndex = channel > 0 ? Math.Clamp(channel - 1, 0, 11) : 0;
            _smokeThresholdInput.Value = _config.GunFx.Smoke.HeaterPwmThresholdUs;
            _smokeFanDelayInput.Value = _config.GunFx.Smoke.FanOffDelayMs;
        }
        else
        {
            _smokeEnabled.Checked = false;
        }
        UpdateSmokeControls();

        // Rates of Fire
        RefreshRatesList();
        if (_ratesListBox.Items.Count > 0)
        {
            _ratesListBox.SelectedIndex = 0;
        }

        // Turret - enabled if either pitch or yaw servo has valid input_channel
        var pitchEnabled = (_config.GunFx?.TurretControl?.Pitch?.InputChannel ?? 0) > 0;
        var yawEnabled = (_config.GunFx?.TurretControl?.Yaw?.InputChannel ?? 0) > 0;
        _turretEnabled.Checked = pitchEnabled || yawEnabled;
        _turretPanel.Visible = _turretEnabled.Checked;

        // Turret - Pitch (using ServoAxisPanel)
        LoadServoAxisFromConfig(_pitchAxis, _config.GunFx?.TurretControl?.Pitch);

        // Turret - Yaw (using ServoAxisPanel)
        LoadServoAxisFromConfig(_yawAxis, _config.GunFx?.TurretControl?.Yaw);
    }

    private void LoadServoAxisFromConfig(ServoAxisPanel axis, ServoConfig? config)
    {
        // Enabled if config exists and has valid input_channel
        axis.Enabled = config != null && config.InputChannel > 0;
        axis.SettingsPanel.Visible = axis.Enabled;

        if (config != null)
        {
            var channel = config.InputChannel;
            axis.Channel = channel > 0 ? channel : 1;
            axis.ServoId = config.ServoId > 0 ? config.ServoId : 1;
            axis.InputRange.MinValue = config.InputMinUs;
            axis.InputRange.MaxValue = config.InputMaxUs;
            axis.OutputRange.MinValue = config.OutputMinUs;
            axis.OutputRange.MaxValue = config.OutputMaxUs;
            axis.SpeedControl.Value = Math.Clamp((int)config.MaxSpeedUsPerSec, 100, 10000);
            axis.AccelControl.Value = Math.Clamp((int)config.MaxAccelUsPerSec2, 100, 10000);
            axis.DecelControl.Value = Math.Clamp((int)config.MaxDecelUsPerSec2, 100, 10000);
            axis.RecoilJerkControl.Value = Math.Clamp(config.RecoilJerkUs, 0, 200);
            axis.RecoilVarianceControl.Value = Math.Clamp(config.RecoilJerkVarianceUs, 0, 100);
        }
    }

    private void SaveUIToConfig()
    {
        // Engine FX
        if (_engineFxEnabled.Checked)
        {
            SaveEngineFxFromUI();
        }
        else
        {
            _config.EngineFx = null;
        }

        // Gun FX
        if (_gunFxEnabled.Checked)
        {
            SaveGunFxFromUI();
        }
        else
        {
            _config.GunFx = null;
        }
    }

    private void SaveEngineFxFromUI()
    {
        _config.EngineFx ??= new EngineFxConfig();

        // Engine Type
        _config.EngineFx.Type = _engineTypeCombo.SelectedItem?.ToString() ?? "turbine";

        // Engine Toggle
        _config.EngineFx.EngineToggle ??= new PwmInputConfig();
        _config.EngineFx.EngineToggle.InputChannel = _enginePinInput.SelectedIndex + 1;
        _config.EngineFx.EngineToggle.ThresholdUs = _engineThresholdEnabled.Checked
            ? (int)_engineThresholdInput.Value
            : 1500;

        // Engine Sounds
        _config.EngineFx.Sounds ??= new EngineSoundsConfig();

        // Starting sound
        if (_startSoundPanel.Enabled)
        {
            _config.EngineFx.Sounds.Starting = _startSoundPanel.FileName;
        }
        else
        {
            _config.EngineFx.Sounds.Starting = null;
        }

        // Running sound
        if (_runSoundPanel.Enabled)
        {
            _config.EngineFx.Sounds.Running = _runSoundPanel.FileName;
        }
        else
        {
            _config.EngineFx.Sounds.Running = null;
        }

        // Stopping sound
        if (_stopSoundPanel.Enabled)
        {
            _config.EngineFx.Sounds.Stopping = _stopSoundPanel.FileName;
        }
        else
        {
            _config.EngineFx.Sounds.Stopping = null;
        }

        // Transitions (only if at least start or stop sound is enabled)
        if (_startSoundPanel.Enabled || _stopSoundPanel.Enabled)
        {
            _config.EngineFx.Sounds.Transitions = new TransitionsConfig
            {
                StartingOffsetMs = _startSoundPanel.Offset,
                StoppingOffsetMs = _stopSoundPanel.Offset
            };
        }
        else
        {
            _config.EngineFx.Sounds.Transitions = null;
        }
    }

    private void SaveGunFxFromUI()
    {
        _config.GunFx ??= new GunFxConfig();

        // Trigger
        _config.GunFx.Trigger ??= new TriggerConfig();
        _config.GunFx.Trigger.InputChannel = _triggerChannelInput.SelectedIndex + 1;

        // Smoke - set to null when disabled (section won't be serialized)
        if (_smokeEnabled.Checked)
        {
            _config.GunFx.Smoke = new SmokeConfig
            {
                HeaterToggleChannel = _smokeChannelInput.SelectedIndex + 1,
                HeaterPwmThresholdUs = (int)_smokeThresholdInput.Value,
                FanOffDelayMs = (int)_smokeFanDelayInput.Value
            };
        }
        else
        {
            _config.GunFx.Smoke = null;
        }

        // Rates of Fire - preserve existing list (updated via Add/Update/Remove buttons)
        // If no rates, set to null so section is omitted
        if (_config.GunFx.RateOfFire == null || _config.GunFx.RateOfFire.Count == 0)
        {
            _config.GunFx.RateOfFire = null;
        }

        // Turret Control
        if (_turretEnabled.Checked)
        {
            _config.GunFx.TurretControl ??= new TurretControlConfig();

            // Pitch - using ServoAxisPanel
            _config.GunFx.TurretControl.Pitch = SaveServoAxisToConfig(_pitchAxis);

            // Yaw - using ServoAxisPanel
            _config.GunFx.TurretControl.Yaw = SaveServoAxisToConfig(_yawAxis);
        }
        else
        {
            _config.GunFx.TurretControl = null;
        }
    }

    private ServoConfig? SaveServoAxisToConfig(ServoAxisPanel axis)
    {
        if (!axis.Enabled) return null;

        return new ServoConfig
        {
            InputChannel = axis.Channel,
            ServoId = axis.ServoId,
            InputMinUs = axis.InputRange.MinValue,
            InputMaxUs = axis.InputRange.MaxValue,
            OutputMinUs = axis.OutputRange.MinValue,
            OutputMaxUs = axis.OutputRange.MaxValue,
            MaxSpeedUsPerSec = axis.SpeedControl.Value,
            MaxAccelUsPerSec2 = axis.AccelControl.Value,
            MaxDecelUsPerSec2 = axis.DecelControl.Value,
            RecoilJerkUs = axis.RecoilJerkControl.Value,
            RecoilJerkVarianceUs = axis.RecoilVarianceControl.Value
        };
    }
}
