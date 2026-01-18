#!/bin/bash
#
# Raspberry Pi DigiAMP+ Driver Installation Script
# Configures DigiAMP+ (IQaudio DAC+) for Raspberry Pi
#
# Usage: sudo ./install-digiamp-driver.sh
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}DigiAMP+ Driver Configuration${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Error: Please run as root (use sudo)${NC}"
    exit 1
fi

# Check Raspberry Pi model
echo -e "${YELLOW}Detecting Raspberry Pi model...${NC}"
PI_MODEL=$(cat /proc/device-tree/model 2>/dev/null || echo "Unknown")
echo -e "  Model: $PI_MODEL"
echo ""

# Install dependencies
echo -e "${YELLOW}Installing audio tools...${NC}"
apt-get update

echo -e "${YELLOW}Note: DigiAMP+ driver is built into Trixie kernel${NC}"
echo -e "${YELLOW}No separate driver build required${NC}"
echo ""

apt-get install -y \
    i2c-tools \
    libasound2-dev \
    alsa-utils

echo -e "${GREEN}Dependencies installed${NC}"
echo ""

# Enable I2C interface
echo -e "${YELLOW}Enabling I2C interface...${NC}"
if ! raspi-config nonint do_i2c 0; then
    echo -e "${YELLOW}  Could not enable I2C automatically, enabling manually...${NC}"
    if ! grep -q "^dtparam=i2c_arm=on" /boot/config.txt; then
        echo "dtparam=i2c_arm=on" >> /boot/config.txt
    fi
fi
echo -e "${GREEN}I2C enabled${NC}"
echo ""

# Configure device tree overlay
echo -e "${YELLOW}Configuring device tree overlay...${NC}"
BOOT_CONFIG="/boot/config.txt"

# Remove conflicting audio configurations
sed -i '/^dtparam=audio=/d' "$BOOT_CONFIG"
sed -i '/^dtoverlay=vc4-kms-v3d/d' "$BOOT_CONFIG"
sed -i '/^dtoverlay=wm8960-soundcard/d' "$BOOT_CONFIG"

# Add DigiAMP+ overlay if not present
if ! grep -q "^dtoverlay=iqaudio-dacplus" "$BOOT_CONFIG"; then
    echo "" >> "$BOOT_CONFIG"
    echo "# Raspberry Pi DigiAMP+ (IQaudio DAC+) configuration" >> "$BOOT_CONFIG"
    echo "dtoverlay=iqaudio-dacplus" >> "$BOOT_CONFIG"
    echo -e "${GREEN}  Device tree overlay added${NC}"
else
    echo -e "${GREEN}  Device tree overlay already configured${NC}"
fi

echo ""

# Load I2C module
echo -e "${YELLOW}Loading I2C kernel module...${NC}"
modprobe i2c-dev 2>/dev/null || true
echo -e "${GREEN}I2C module loaded${NC}"
echo ""

# Configure ALSA
echo -e "${YELLOW}Configuring ALSA...${NC}"
if [ ! -f /etc/asound.conf ]; then
    cat > /etc/asound.conf << 'EOF'
# DigiAMP+ ALSA configuration
pcm.!default {
    type plug
    slave.pcm "hw:0,0"
}

ctl.!default {
    type hw
    card 0
}
EOF
    echo -e "${GREEN}  ALSA configuration created${NC}"
else
    echo -e "${YELLOW}  ALSA config exists, keeping existing${NC}"
fi
echo ""

# Add user to audio group
if [ -n "$SUDO_USER" ]; then
    usermod -a -G audio "$SUDO_USER" 2>/dev/null || true
    echo -e "${GREEN}User $SUDO_USER added to audio group${NC}"
fi
echo ""

# DigiAMP+ specific notes
echo -e "${YELLOW}Trixie/Debian 13 Compatibility:${NC}"
echo -e "  - WM8960 and DigiAMP+ drivers are built-in to Trixie kernel"
echo -e "  - Only device tree configuration is needed"
echo -e "  - No driver compilation required"
echo ""
echo -e "${YELLOW}DigiAMP+ Power Requirements:${NC}"
echo -e "  ${RED}⚠️  IMPORTANT: DigiAMP+ requires external power!${NC}"
echo -e "  - Connect 12-24V DC to screw terminals"
echo -e "  - Do NOT power from Raspberry Pi"
echo -e "  - Connect speakers to speaker terminals"
echo -e "  - Observe correct polarity (+/- terminals)"
echo ""

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Configuration Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "${YELLOW}Disabling ALSA restore to avoid conflicts...${NC}"
systemctl disable alsa-restore.service 2>/dev/null || true
systemctl mask alsa-restore.service 2>/dev/null || true
echo -e "${GREEN}alsa-restore disabled and masked${NC}"
echo ""
echo -e "${GREEN}Note: DigiAMP+ driver is pre-built in Raspberry Pi OS Trixie${NC}"
echo ""
echo -e "${RED}⚠️  REBOOT REQUIRED  ⚠️${NC}"
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "  1. Connect external 12-24V power supply to DigiAMP+"
echo "  2. Connect passive speakers (4Ω or 8Ω) to speaker terminals"
echo "  3. Reboot the Raspberry Pi: sudo reboot"
echo "  4. After reboot, verify audio device:"
echo "     aplay -l"
echo "     (Should show 'IQaudIODAC' or similar)"
echo "  5. Test audio output (START QUIET!):"
echo "     speaker-test -c 2 -t wav -l 1"
echo "  6. Configure audio levels:"
echo "     sudo sfxhub-audio-setup --verbose"
echo ""
echo -e "${YELLOW}Troubleshooting:${NC}"
echo "  - No audio: Verify external power connected"
echo "  - Check I2C codec: sudo i2cdetect -y 1 (TAS5756 at 0x4c)"
echo "  - Check volume: alsamixer -c 0"
echo "  - Driver logs: dmesg | grep -i iqaudio"
echo ""
echo -e "${YELLOW}Safety:${NC}"
echo "  - DigiAMP+ outputs up to 35W per channel"
echo "  - Ensure speakers are rated for the power"
echo "  - Start at low volume and increase gradually"
echo ""
