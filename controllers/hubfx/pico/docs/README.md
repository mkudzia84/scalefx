# Documentation Index

This directory contains technical documentation for the HubFX Pico project.

## Documents

### [WIRING.md](WIRING.md)
Complete hardware wiring guide including:
- WM8960 Audio HAT connections
- TAS5825M amplifier connections
- SD card module wiring
- Pin summary tables
- Troubleshooting hardware issues

### [CODECS.md](CODECS.md)
Audio codec architecture documentation:
- Generic `AudioCodec` interface design
- WM8960 low-power codec
- TAS5825M high-power amplifier
- SimpleI2SCodec for basic DACs
- Creating custom codec drivers
- Codec comparison table

### [TAS5825M.md](TAS5825M.md)
Comprehensive TAS5825M usage guide:
- Hardware connections and power requirements
- I2C control and DSP programming
- Volume control (digital -100dB to +24dB)
- Supply voltage configuration (12V/15V/20V/24V)
- Troubleshooting and register reference
- Based on bassowl-hat project

## Quick Links

- **Main README**: [../README.md](../README.md)
- **Example Config**: [../config.yaml](../config.yaml)
- **Source Code**: [../src/](../src/)

## Documentation Organization

```
pico/
├── README.md              # Main project documentation
├── config.yaml            # Example configuration
├── docs/                  # Technical documentation (you are here)
│   ├── README.md         # This file
│   ├── WIRING.md         # Hardware wiring
│   ├── CODECS.md         # Codec architecture
│   └── TAS5825M.md       # TAS5825M guide
└── src/                   # Source code
    ├── audio/            # Audio subsystem
    ├── storage/          # Storage & config
    └── effects/          # Special effects
```

## Getting Help

1. **Build issues**: See main [README.md](../README.md#troubleshooting)
2. **Hardware problems**: See [WIRING.md](WIRING.md#troubleshooting)
3. **Audio codec setup**: See [CODECS.md](CODECS.md)
4. **TAS5825M specific**: See [TAS5825M.md](TAS5825M.md#troubleshooting)

## Contributing

When adding new codecs or features, please:
1. Update relevant documentation
2. Add wiring diagrams to WIRING.md
3. Include example code in CODECS.md
4. Update comparison tables

## Version History

- **v1.1.0** (2026-01-03): Documentation reorganized, TAS5825M support added
- **v1.0.0** (2025-12): Initial release with WM8960 support
