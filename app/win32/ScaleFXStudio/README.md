# ScaleFX Studio

Windows visual configuration editor for the ScaleFX effects system. Provides a graphical interface to configure and manage Pico-based scale model effects controllers.

## Features

- **Visual Configuration Editor** - Edit all ScaleFX settings through an intuitive Windows Forms interface
- **Engine FX Settings** - Configure engine sounds, throttle channel, and transition timings
- **Gun FX Settings** - Set up trigger channel, rates of fire, smoke generator, and turret servos
- **Servo Axis Configuration** - Configure pitch, yaw, and retract servo mappings with input/output ranges
- **Recoil Jerk Settings** - Configure per-servo recoil jerk effect parameters
- **Real-time Validation** - Input validation with visual feedback (1-10 channel range)
- **YAML Export** - Generate config.yaml files compatible with the ScaleFX Hub daemon

## Requirements

- Windows 10/11
- .NET 8.0 Runtime (or use self-contained build)

## Building

### Prerequisites

- [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0)
- Visual Studio 2022 (optional)

### Build Commands

```powershell
# Navigate to project directory
cd app\win32\ScaleFXStudio

# Build debug version
dotnet build

# Build release version
dotnet build -c Release

# Publish single-file executable (self-contained, no .NET runtime needed)
dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true

# Output will be in: bin\Release\net8.0-windows\win-x64\publish\ScaleFXStudio.exe
```

## Usage

1. **Launch** ScaleFX Studio
2. **Open** an existing config.yaml or start fresh
3. **Configure** your settings:
   - Set input channels (1-10) for each function
   - Configure PWM thresholds for triggers
   - Set up servo ranges and motion profiles
   - Add rates of fire with sound file paths
4. **Save** to generate a `config.yaml` file
5. **Transfer** the config file to your controller

## Configuration Sections

### Engine FX

| Setting | Description |
|---------|-------------|
| Input Channel | RC receiver channel for throttle (1-10) |
| Threshold | PWM value above which engine is "on" |
| Sound Files | Paths to start, loop, and stop WAV files |
| Transition Offsets | Timing for seamless audio blending |

### Gun FX

| Setting | Description |
|---------|-------------|
| Trigger Channel | RC receiver channel for gun trigger (1-10) |
| Rates of Fire | Multiple fire rates with RPM and sound files |
| Smoke Generator | Heater toggle channel and fan-off delay |
| Turret Control | Pitch/Yaw/Retract servo axis configuration |

### Servo Axis

Each servo axis includes:
- **Servo ID** - Pico output (1, 2, or 3)
- **Input Channel** - RC receiver channel (1-10)
- **Input Range** - Min/Max PWM from receiver (µs)
- **Output Range** - Min/Max PWM to servo (µs)
- **Motion Profile** - Max speed, acceleration, deceleration
- **Recoil Jerk** - Base jerk and variance for firing effect

## Deployment

The published executable is completely self-contained:
- Single `.exe` file (~15-25 MB)
- No .NET runtime installation required
- Just copy and run

## Project Structure

```
ScaleFXStudio/
├── Program.cs                 # Application entry point
├── MainForm.cs                # Main window and UI
├── MainForm.ConfigBinding.cs  # Configuration binding logic
├── Models/
│   └── ScaleFXConfiguration.cs   # Configuration data model
├── Services/
│   └── ConfigService.cs       # YAML serialization
└── Controls/
    └── ServoAxisPanel.cs      # Reusable servo configuration panel
```

## Development

The application uses:
- **WinForms** for the GUI
- **YamlDotNet** for YAML parsing/serialization
- **PropertyGrid** for auto-generated property editors
